// Shared by the daemon, the CLI and the app: settings and battery state.
#ifndef CHARGECAP_H
#define CHARGECAP_H

#include <stdbool.h>

#define CC_PREFS_PATH "/var/mobile/Library/Preferences/uk.lbsi.chargecap.plist"
#define CC_LOG_PATH "/var/mobile/Library/Logs/ChargeCap.log"
#define CC_NOTIFY_PREFS "uk.lbsi.chargecap.prefs-changed"
#define CC_PID_PATH "/var/run/chargecapd.pid"
// Present while the daemon has written IsCharging=false to the charger itself (its fallback
// when powerd won't pause charging), so whoever stops it knows to undo that.
#define CC_FORCED_MARKER "/var/run/chargecapd.forced"

#define CC_DEFAULT_LIMIT 80
#define CC_DEFAULT_RESUME 75
#define CC_MIN_LIMIT 50
#define CC_MIN_RESUME 20

typedef struct {
    bool enabled;
    int limit;  // pause charging once the battery reaches this percent
    int resume; // charge again once it falls to this percent
} cc_prefs;

typedef struct {
    int percent;     // as the status bar shows it
    bool external;   // plugged into power
    bool charging;   // charger is putting current into the battery
    bool inhibited;  // some process holds a ChargeInhibit assertion
    int milliamps;   // battery current, + charging, - discharging
    int cycles;
    int max_mah;     // what the battery holds today
    int design_mah;  // what it held new
    int temp_c100;   // hundredths of a degree C
} cc_battery;

void cc_prefs_load(cc_prefs *p);
bool cc_prefs_save(const cc_prefs *p); // also tells the daemon
void cc_prefs_clamp(cc_prefs *p);

bool cc_battery_read(cc_battery *b);
bool cc_charger_set_charging(bool on); // direct charger write, the fallback when powerd won't

#endif
