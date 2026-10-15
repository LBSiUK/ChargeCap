// chargecap: show or change the charge limit from a shell.
#include "chargecap.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int usage(void) {
    fprintf(stderr,
            "usage: chargecap                 show battery and limit\n"
            "       chargecap limit <%d-100>  pause charging at this percent\n"
            "       chargecap resume <n>      charge again once the battery falls to n percent\n"
            "       chargecap on | off        turn the limit on or off\n"
            "       chargecap log             recent daemon log\n",
            CC_MIN_LIMIT);
    return 2;
}

static bool daemon_running(void) {
    FILE *f = fopen(CC_PID_PATH, "r");
    int pid = 0;
    if (f) {
        if (fscanf(f, "%d", &pid) != 1) pid = 0;
        fclose(f);
    }
    return pid > 0 && kill(pid, 0) == 0;
}

static int status(void) {
    cc_prefs p;
    cc_prefs_load(&p);
    cc_battery b;
    if (!cc_battery_read(&b)) {
        fprintf(stderr, "chargecap: cannot read the battery\n");
        return 1;
    }
    const char *state = !b.external ? "on battery"
                        : b.inhibited ? "plugged in, charging paused"
                        : b.charging ? "charging"
                                     : "plugged in, not charging";
    printf("Battery  %d%%, %s (%+d mA)\n", b.percent, state, b.milliamps);
    if (p.enabled)
        printf("Limit    pause at %d%%, resume at %d%%\n", p.limit, p.resume);
    else
        printf("Limit    off\n");
    if (b.design_mah > 0)
        printf("Health   %d%% of design (%d of %d mAh), %d cycles, %.1f C\n", (b.max_mah * 100 + b.design_mah / 2) / b.design_mah,
               b.max_mah, b.design_mah, b.cycles, b.temp_c100 / 100.0);
    printf("Daemon   %s\n", daemon_running() ? "running" : "NOT running");
    return 0;
}

static int show_log(void) {
    FILE *f = fopen(CC_LOG_PATH, "r");
    if (!f) {
        fprintf(stderr, "chargecap: no log yet\n");
        return 1;
    }
    // Print the last 20 lines (the device has no tail).
    static char lines[20][256];
    int n = 0;
    while (fgets(lines[n % 20], sizeof lines[0], f)) n++;
    fclose(f);
    for (int i = n > 20 ? n - 20 : 0; i < n; i++) fputs(lines[i % 20], stdout);
    return 0;
}

static bool parse_percent(const char *s, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < 0 || v > 100) return false;
    *out = (int)v;
    return true;
}

int main(int argc, char **argv) {
    if (argc == 1) return status();
    if (argc == 2 && !strcmp(argv[1], "log")) return show_log();
    if (argc == 2 && !strcmp(argv[1], "release-charger")) {
        // For the package scripts: undo the daemon's direct charger write, if it made one.
        if (access(CC_FORCED_MARKER, F_OK) == 0 && cc_charger_set_charging(true)) unlink(CC_FORCED_MARKER);
        return 0;
    }

    cc_prefs p;
    cc_prefs_load(&p);
    if (argc == 2 && !strcmp(argv[1], "on")) p.enabled = true;
    else if (argc == 2 && !strcmp(argv[1], "off")) p.enabled = false;
    else if (argc == 3 && !strcmp(argv[1], "limit")) {
        int limit;
        if (!parse_percent(argv[2], &limit)) return usage();
        if (limit < CC_MIN_LIMIT) {
            fprintf(stderr, "chargecap: the limit can't go below %d%%\n", CC_MIN_LIMIT);
            return 1;
        }
        p.resume = limit - (p.limit - p.resume); // keep the same gap below the limit
        p.limit = limit;
    } else if (argc == 3 && !strcmp(argv[1], "resume") && parse_percent(argv[2], &p.resume)) {
        if (p.resume >= p.limit) {
            fprintf(stderr, "chargecap: resume must be below the limit (%d%%)\n", p.limit);
            return 1;
        }
    } else
        return usage();

    if (!cc_prefs_save(&p)) {
        perror("chargecap: saving " CC_PREFS_PATH);
        return 1;
    }
    sleep(2); // let the daemon and the charger catch up before reporting
    return status();
}
