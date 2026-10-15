// chargecapd: pauses charging at the chosen percent and resumes it lower down.
//
// The pause is a powerd "ChargeInhibit" assertion, which powerd drops by itself if this
// process dies, so a crash can never leave the iPad unable to charge. While the iPad charges
// up to the limit we hold "PreventUserIdleSystemSleep", or it could sleep straight past it.
#include "chargecap.h"
#include "iokit.h"
#include <dispatch/dispatch.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FORCED_MARKER CC_FORCED_MARKER
#define PID_PATH CC_PID_PATH

static cc_prefs prefs;
static IOPMAssertionID inhibit, awake;
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
    set_forced(false);
    stuck = 0;
}

static void evaluate(bool tick) {
    cc_battery b;
    if (!cc_battery_read(&b)) return;
    bool was = inhibit != 0;

    if (!prefs.enabled || !b.external) {
        allow_charging();
        drop(&awake);
    } else {
        if (inhibit && b.percent <= prefs.resume) allow_charging();
        else if (!inhibit && b.percent >= prefs.limit) pause_charging();

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

    if (was != (inhibit != 0))
        logline("%s charging at %d%% (limit %d%%, resume %d%%)", inhibit ? "paused" : "resumed", b.percent,
                prefs.limit, prefs.resume);
}

static void reload(void) {
    cc_prefs old = prefs;
    cc_prefs_load(&prefs);
    if (old.enabled != prefs.enabled || old.limit != prefs.limit || old.resume != prefs.resume)
        logline("settings: %s, limit %d%%, resume %d%%", prefs.enabled ? "on" : "off", prefs.limit, prefs.resume);
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
            logline("iPad sleeping at %d%%, charging %s", b.percent, inhibit ? "paused" : "on");
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
    logline("started: %s, limit %d%%, resume %d%%", prefs.enabled ? "on" : "off", prefs.limit, prefs.resume);
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
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 60, 60, 0, 0, on_tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

    evaluate(false);
    CFRunLoopRun();
    return 0;
}
