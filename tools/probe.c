// Development probe: list IORegistry entries, dump properties, poke a property.
//   probe list                     every IOService entry as "class  name"
//   probe props <class>            properties of every service matching <class>
//   probe set <class> <key> <val>  val: true|false|<int>|<string>
//   probe assert <type> <secs>     hold a powerd assertion (e.g. ChargeInhibit) for secs
//   probe get <class> <key>...     selected properties on one line
//   probe ps | assertions          power-source info / powerd assertion state
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/iokit.h"

static void show(CFTypeRef obj) {
    CFStringRef desc = CFCopyDescription(obj);
    char buf[65536];
    if (CFStringGetCString(desc, buf, sizeof buf, kCFStringEncodingUTF8)) puts(buf);
    CFRelease(desc);
}

static CFTypeRef parse(const char *s) {
    if (!strcmp(s, "true")) return kCFBooleanTrue;
    if (!strcmp(s, "false")) return kCFBooleanFalse;
    char *end;
    long n = strtol(s, &end, 0);
    if (*s && !*end) return CFNumberCreate(NULL, kCFNumberLongType, &n);
    return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        io_iterator_t it;
        if (IORegistryCreateIterator(kIOMasterPortDefault, "IOService", kIORegistryIterateRecursively, &it)) return 1;
        io_object_t o;
        while ((o = IOIteratorNext(it))) {
            io_name_t cls, name;
            IOObjectGetClass(o, cls);
            IORegistryEntryGetName(o, name);
            printf("%-40s %s\n", cls, name);
            IOObjectRelease(o);
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "props")) {
        io_iterator_t it;
        if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching(argv[2]), &it)) return 1;
        io_object_t o;
        while ((o = IOIteratorNext(it))) {
            io_name_t cls;
            IOObjectGetClass(o, cls);
            printf("=== %s\n", cls);
            CFMutableDictionaryRef props = NULL;
            if (!IORegistryEntryCreateCFProperties(o, &props, NULL, 0) && props) show(props);
            IOObjectRelease(o);
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "get")) {
        io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(argv[2]));
        if (!s) { fprintf(stderr, "no %s\n", argv[2]); return 1; }
        for (int i = 3; i < argc; i++) {
            CFStringRef key = CFStringCreateWithCString(NULL, argv[i], kCFStringEncodingUTF8);
            CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, NULL, 0);
            char buf[256] = "-";
            if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) strcpy(buf, CFBooleanGetValue(v) ? "true" : "false");
            else if (v && CFGetTypeID(v) == CFNumberGetTypeID()) { long long n; CFNumberGetValue(v, kCFNumberLongLongType, &n); snprintf(buf, sizeof buf, "%lld", n); }
            else if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8);
            else if (v) strcpy(buf, "<obj>");
            printf("%s=%s ", argv[i], buf);
        }
        printf("\n");
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "set")) {
        io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(argv[2]));
        if (!s) { fprintf(stderr, "no %s\n", argv[2]); return 1; }
        CFStringRef key = CFStringCreateWithCString(NULL, argv[3], kCFStringEncodingUTF8);
        kern_return_t kr = IORegistryEntrySetCFProperty(s, key, parse(argv[4]));
        printf("set %s=%s -> 0x%x\n", argv[3], argv[4], kr);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "assert")) {
        IOPMAssertionID id = 0;
        CFStringRef type = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
        IOReturn r = IOPMAssertionCreateWithName(type, 255, CFSTR("chargecap-probe"), &id);
        printf("assert %s -> 0x%x id=%u\n", argv[2], r, id);
        fflush(stdout);
        sleep(atoi(argv[3]));
        IOPMAssertionRelease(id);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "ps")) {
        CFTypeRef blob = IOPSCopyPowerSourcesInfo();
        CFArrayRef list = blob ? IOPSCopyPowerSourcesList(blob) : NULL;
        for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++)
            show(IOPSGetPowerSourceDescription(blob, CFArrayGetValueAtIndex(list, i)));
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "assertions")) {
        CFDictionaryRef d = NULL;
        printf("status -> 0x%x\n", IOPMCopyAssertionsStatus(&d));
        if (d) show(d);
        d = NULL;
        printf("byprocess -> 0x%x\n", IOPMCopyAssertionsByProcess(&d));
        if (d) show(d);
        return 0;
    }
    fprintf(stderr, "usage: probe list | props <class> | set <class> <key> <val> | assert <type> <secs>\n");
    return 2;
}
