// chargecapd: pauses charging at the chosen percent and resumes it lower down.
//
// The pause is a powerd "ChargeInhibit" assertion, which powerd drops by itself if this
// process dies, so a crash can never leave the iPad unable to charge. While the iPad charges
// up to the limit we hold "PreventUserIdleSystemSleep", or it could sleep straight past it.
//
// A pause only holds the level where it is, so on iOS 15 a battery above the limit is run down
// with a "DisableInflow" assertion: the charger stops feeding the iPad, cable still in, until the
// battery is back at the limit. powerd drops that one on process death too.
#include "chargecap.h"
#include "iokit.h"
#include <Availability.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FORCED_MARKER CC_FORCED_MARKER
#define PID_PATH CC_PID_PATH
// DisableInflow has only been tried on the iOS 15 iPad Air 2, not on the iOS 9 mini.
#define CAN_DRAIN (__IPHONE_OS_VERSION_MIN_REQUIRED >= 150000)
// iOS's percent rebounds a point or two when a heavy discharge stops, so don't start another for that.
#define DRAIN_MARGIN 2

static cc_prefs prefs;
static IOPMAssertionID inhibit, drain, awake;
static int blank_token;
static bool forced;
static int stuck; // timer ticks the charger kept charging under our inhibit

static void logline(const char *fmt, ...) {
    struct stat st;
    if (stat(CC_LOG_PATH, &st) == 0 && st.st_size > 128 * 1024) rename(CC_LOG_PATH, CC_LOG_PATH ".1");
    FILE *f = fopen(CC_LOG_PATH, "a");
    if (!f) return;
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void hold(IOPMAssertionID *id, CFStringRef type, CFStringRef name) {
    if (*id) return;
    IOReturn r = IOPMAssertionCreateWithName(type, kIOPMAssertionLevelOn, name, id);
    if (r != KERN_SUCCESS) {
        *id = 0;
        logline("could not create assertion (0x%x)", r);
    }
}

static void drop(IOPMAssertionID *id) {
    if (!*id) return;
    IOPMAssertionRelease(*id);
    *id = 0;
}

static void pause_charging(void) {
    hold(&inhibit, CFSTR("ChargeInhibit"), CFSTR("ChargeCap: battery at limit"));
}

static void set_forced(bool on) {
    if (on) {
        forced = cc_charger_set_charging(false);
        if (forced) fclose(fopen(FORCED_MARKER, "w"));
    } else if (forced || access(FORCED_MARKER, F_OK) == 0) {
        cc_charger_set_charging(true);
        unlink(FORCED_MARKER);
        forced = false;
    }
}

static void allow_charging(void) {
    drop(&inhibit);
    drop(&drain);
    set_forced(false);
    stuck = 0;
}

static bool screen_is_on(void) {
    uint64_t blanked = 0;
    notify_get_state(blank_token, &blanked);
    return blanked == 0;
}

static const char *state_name(void) { return drain ? "running down" : inhibit ? "paused" : "charging"; }

static void evaluate(bool tick) {
    cc_battery b;
    if (!cc_battery_read(&b)) return;
    const char *was = state_name();

    if (!prefs.enabled || !b.external) {
        allow_charging();
        drop(&awake);
    } else {
        // Only a separate, lower screen-off level makes the screen matter.
        bool off_floor = !screen_is_on() && prefs.resume_off < prefs.resume;
        int floor = off_floor ? prefs.resume_off : prefs.resume;
        // The raw figure reads a few points lower than the status bar. A gap of 10 means the status bar
        // is stuck, so stop rather than run the battery down for ever.
        int drain_above = drain ? prefs.limit : prefs.limit + DRAIN_MARGIN;
        if (CAN_DRAIN && b.percent > drain_above && b.raw_percent > prefs.limit - 10) {
            hold(&drain, CFSTR("DisableInflow"), CFSTR("ChargeCap: battery above limit"));
            drop(&inhibit);
        } else {
            if (drain) {
                pause_charging();
                drop(&drain);
            }
            if (inhibit && b.percent <= floor) allow_charging();
            // With the screen off, nothing above that level needs a charge, even one already under way.
            else if (!inhibit && (b.percent >= prefs.limit || (off_floor && b.percent > floor))) pause_charging();
        }

        if (inhibit) {
            drop(&awake);
            // powerd normally stops the charger within a few seconds. If it is still charging two
            // minutes on, take the assertion again; two minutes after that, write to the charger.
            if (tick && b.charging && !forced) {
                stuck++;
                if (stuck == 2) {
                    logline("charger still charging under the inhibit; asking powerd again");
                    drop(&inhibit);
                    pause_charging();
                } else if (stuck >= 4) {
                    set_forced(true);
                    logline("powerd did not stop the charger; wrote to it directly (%s)", forced ? "ok" : "failed");
                }
            } else if (!b.charging) {
                stuck = 0;
            }
        } else {
            hold(&awake, CFSTR("PreventUserIdleSystemSleep"), CFSTR("ChargeCap: charging to limit"));
        }
    }

    if (strcmp(was, state_name()))
        logline("%s at %d%%, screen %s (limit %d%%, resume %d%%, screen off %d%%)", state_name(), b.percent,
                screen_is_on() ? "on" : "off", prefs.limit, prefs.resume, prefs.resume_off);
}

static void reload(void) {
    cc_prefs old = prefs;
    cc_prefs_load(&prefs);
    if (old.enabled != prefs.enabled || old.limit != prefs.limit || old.resume != prefs.resume ||
        old.resume_off != prefs.resume_off)
        logline("settings: %s, limit %d%%, resume %d%%, screen off %d%%", prefs.enabled ? "on" : "off", prefs.limit,
                prefs.resume, prefs.resume_off);
}

static void on_power_change(void *ctx) { evaluate(false); }

static void on_prefs_change(CFNotificationCenterRef c, void *o, CFStringRef n, const void *obj, CFDictionaryRef info) {
    reload();
    evaluate(false);
}

static void on_tick(CFRunLoopTimerRef t, void *ctx) {
    reload();
    evaluate(true);
}

// Logged while plugged in, so the log shows whether the iPad ever slept while charging to the limit.
static io_connect_t root_port;
static void on_system_power(void *ref, io_service_t service, uint32_t type, void *arg) {
    cc_battery b;
    switch (type) {
    case kIOMessageCanSystemSleep:
        IOAllowPowerChange(root_port, (intptr_t)arg);
        break;
    case kIOMessageSystemWillSleep:
        if (prefs.enabled && cc_battery_read(&b) && b.external)
            logline("iPad sleeping at %d%%, %s", b.percent, state_name());
        IOAllowPowerChange(root_port, (intptr_t)arg);
        break;
    case kIOMessageSystemHasPoweredOn:
        if (prefs.enabled && cc_battery_read(&b) && b.external) logline("iPad woke at %d%%", b.percent);
        evaluate(false);
        break;
    }
}

int main(void) {
    signal(SIGTERM, SIG_IGN);
    dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(term, ^{
        allow_charging();
        drop(&awake);
        unlink(PID_PATH);
        logline("stopped");
        exit(0);
    });
    dispatch_resume(term);

    FILE *pid = fopen(PID_PATH, "w");
    if (pid) {
        fprintf(pid, "%d\n", getpid());
        fclose(pid);
    }

    cc_prefs_load(&prefs);
    logline("started: %s, limit %d%%, resume %d%%, screen off %d%%", prefs.enabled ? "on" : "off", prefs.limit,
            prefs.resume, prefs.resume_off);
    set_forced(false); // undo a direct charger write left by an earlier run

    CFRunLoopAddSource(CFRunLoopGetCurrent(), IOPSNotificationCreateRunLoopSource(on_power_change, NULL),
                       kCFRunLoopDefaultMode);
    CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), NULL, on_prefs_change,
                                    CFSTR(CC_NOTIFY_PREFS), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    IONotificationPortRef power_port;
    io_object_t power_notifier;
    root_port = IORegisterForSystemPower(NULL, &power_port, on_system_power, &power_notifier);
    if (root_port)
        CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(power_port), kCFRunLoopDefaultMode);
    else
        logline("could not register for sleep/wake notifications");
    // The screen turning on or off moves the floor.
    notify_register_dispatch("com.apple.springboard.hasBlankedScreen", &blank_token, dispatch_get_main_queue(),
                             ^(int t) { evaluate(false); });
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 60, 60, 0, 0, on_tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

    evaluate(false);
    CFRunLoopRun();
    return 0;
}
