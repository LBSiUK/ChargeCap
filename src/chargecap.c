#include "chargecap.h"
#include "iokit.h"
#include <unistd.h>

static CFStringRef kEnabled = CFSTR("Enabled");
static CFStringRef kLimit = CFSTR("Limit");
static CFStringRef kResume = CFSTR("Resume");

void cc_prefs_clamp(cc_prefs *p) {
    if (p->limit < CC_MIN_LIMIT) p->limit = CC_MIN_LIMIT;
    if (p->limit > 100) p->limit = 100;
    if (p->resume > p->limit - 1) p->resume = p->limit - 1;
    if (p->resume < CC_MIN_RESUME) p->resume = CC_MIN_RESUME;
}

static int dict_int(CFDictionaryRef d, CFStringRef key, int fallback) {
    CFTypeRef v = d ? CFDictionaryGetValue(d, key) : NULL;
    int n = fallback;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberIntType, &n);
    return n;
}

void cc_prefs_load(cc_prefs *p) {
    p->enabled = true;
    p->limit = CC_DEFAULT_LIMIT;
    p->resume = CC_DEFAULT_RESUME;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)CC_PREFS_PATH,
                                                           sizeof CC_PREFS_PATH - 1, false);
    CFReadStreamRef stream = CFReadStreamCreateWithFile(NULL, url);
    CFRelease(url);
    CFPropertyListRef plist = NULL;
    if (CFReadStreamOpen(stream)) {
        plist = CFPropertyListCreateWithStream(NULL, stream, 0, kCFPropertyListImmutable, NULL, NULL);
        CFReadStreamClose(stream);
    }
    CFRelease(stream);

    if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        CFTypeRef en = CFDictionaryGetValue(plist, kEnabled);
        if (en && CFGetTypeID(en) == CFBooleanGetTypeID()) p->enabled = CFBooleanGetValue(en);
        p->limit = dict_int(plist, kLimit, p->limit);
        p->resume = dict_int(plist, kResume, p->resume);
    }
    if (plist) CFRelease(plist);
    cc_prefs_clamp(p);
}

bool cc_prefs_save(const cc_prefs *in) {
    cc_prefs p = *in;
    cc_prefs_clamp(&p);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    CFNumberRef limit = CFNumberCreate(NULL, kCFNumberIntType, &p.limit);
    CFNumberRef resume = CFNumberCreate(NULL, kCFNumberIntType, &p.resume);
    CFDictionarySetValue(d, kEnabled, p.enabled ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(d, kLimit, limit);
    CFDictionarySetValue(d, kResume, resume);
    CFRelease(limit);
    CFRelease(resume);
    CFDataRef data = CFPropertyListCreateData(NULL, d, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    CFRelease(d);
    if (!data) return false;

    // Write a temp file and rename it, so the daemon never reads half a plist.
    static const char tmp[] = CC_PREFS_PATH ".tmp";
    bool ok = false;
    FILE *f = fopen(tmp, "w");
    if (f) {
        ok = fwrite(CFDataGetBytePtr(data), 1, CFDataGetLength(data), f) == (size_t)CFDataGetLength(data);
        ok = (fclose(f) == 0) && ok;
        if (getuid() == 0) chown(tmp, 501, 501); // keep it writable by the app, which runs as mobile
        ok = ok && rename(tmp, CC_PREFS_PATH) == 0;
    }
    CFRelease(data);
    if (ok)
        CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(), CFSTR(CC_NOTIFY_PREFS),
                                             NULL, NULL, true);
    return ok;
}

static io_service_t charger(void) {
    return IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOPMPowerSource"));
}

static int reg_int(io_service_t s, CFStringRef key, int fallback) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, NULL, 0);
    int n = fallback;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberIntType, &n);
    if (v) CFRelease(v);
    return n;
}

static bool reg_bool(io_service_t s, CFStringRef key) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, NULL, 0);
    bool b = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);
    if (v) CFRelease(v);
    return b;
}

bool cc_battery_read(cc_battery *b) {
    *b = (cc_battery){.percent = -1};

    CFTypeRef blob = IOPSCopyPowerSourcesInfo();
    CFArrayRef list = blob ? IOPSCopyPowerSourcesList(blob) : NULL;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef ps = IOPSGetPowerSourceDescription(blob, CFArrayGetValueAtIndex(list, i));
        CFStringRef type = ps ? CFDictionaryGetValue(ps, CFSTR("Type")) : NULL;
        if (!type || CFStringCompare(type, CFSTR("InternalBattery"), 0) != kCFCompareEqualTo) continue;
        int cur = dict_int(ps, CFSTR("Current Capacity"), -1), max = dict_int(ps, CFSTR("Max Capacity"), 100);
        if (cur >= 0 && max > 0) b->percent = (cur * 100 + max / 2) / max;
        CFStringRef state = CFDictionaryGetValue(ps, CFSTR("Power Source State"));
        b->external = state && CFStringCompare(state, CFSTR("AC Power"), 0) == kCFCompareEqualTo;
        break;
    }
    if (list) CFRelease(list);
    if (blob) CFRelease(blob);

    io_service_t s = charger();
    if (s) {
        b->charging = reg_bool(s, CFSTR("IsCharging"));
        b->milliamps = reg_int(s, CFSTR("InstantAmperage"), 0);
        b->cycles = reg_int(s, CFSTR("CycleCount"), 0);
        b->max_mah = reg_int(s, CFSTR("AppleRawMaxCapacity"), reg_int(s, CFSTR("MaxCapacity"), 0));
        b->design_mah = reg_int(s, CFSTR("DesignCapacity"), 0);
        b->temp_c100 = reg_int(s, CFSTR("Temperature"), 0);
        IOObjectRelease(s);
    }

    CFDictionaryRef status = NULL;
    if (IOPMCopyAssertionsStatus(&status) == KERN_SUCCESS && status) {
        b->inhibited = dict_int(status, CFSTR("ChargeInhibit"), 0) > 0;
        CFRelease(status);
    }
    return b->percent >= 0;
}

bool cc_charger_set_charging(bool on) {
    io_service_t s = charger();
    if (!s) return false;
    kern_return_t kr = IORegistryEntrySetCFProperty(s, CFSTR("IsCharging"), on ? kCFBooleanTrue : kCFBooleanFalse);
    IOObjectRelease(s);
    return kr == KERN_SUCCESS;
}
