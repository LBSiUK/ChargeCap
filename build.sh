#!/bin/bash
# Builds ChargeCap and packages it as Cydia/Sileo .debs:
#   build/uk.lbsi.chargecap_<version>_iphoneos-arm.deb     rootful: iOS 9 (armv7)
#   build/uk.lbsi.chargecap_<version>_iphoneos-arm64.deb   rootless (/var/jb): iOS 15 (arm64)
#   ./build.sh install    also copy the right one to IPAD and install it (default mobile@192.168.0.11,
#                         PASS=alpine). With a non-root login (mobile on palera1n) it installs through sudo.
set -euo pipefail
cd "$(dirname "$0")"

VERSION=1.1.0
SDK9="${SDK:-$HOME/sdks/iPhoneOS9.3-armv7.sdk}"
IPAD="${IPAD:-mobile@192.168.0.11}"
PASS="${PASS:-alpine}"
[ -d "$SDK9" ] || { echo "missing SDK $SDK9 (see README: making the armv7 SDK)"; exit 1; }

FLAGS=(-Os -Wall -Wextra -Wno-unused-parameter -Isrc)
# -marm: Xcode's linker no longer sets the Thumb bit on armv7 entry points, so Thumb code
# would start in ARM mode and crash. ARM-mode code has no such bit to lose.
ARMV7=(xcrun clang -isysroot "$SDK9" -arch armv7 -marm -miphoneos-version-min=9.0 "${FLAGS[@]}")
ARM64=(xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=15.0 "${FLAGS[@]}")
LIBS=(-framework CoreFoundation -framework IOKit)
APPLIBS=(-fobjc-arc -framework Foundation -framework UIKit -framework CoreGraphics)
quiet() { grep -v "liblaunch.dylib" || true; }

rm -rf build/bin build/root* && mkdir -p build/bin
B=build/bin
"${ARMV7[@]}" src/chargecapd.c src/chargecap.c "${LIBS[@]}" -o $B/chargecapd-armv7 2>&1 | quiet
"${ARMV7[@]}" src/cli.c src/chargecap.c "${LIBS[@]}" -o $B/chargecap-armv7 2>&1 | quiet
"${ARMV7[@]}" app/main.m src/chargecap.c "${LIBS[@]}" "${APPLIBS[@]}" -o $B/app-armv7 2>&1 | quiet
"${ARM64[@]}" src/chargecapd.c src/chargecap.c "${LIBS[@]}" -o $B/chargecapd-arm64
"${ARM64[@]}" src/cli.c src/chargecap.c "${LIBS[@]}" -o $B/chargecap-arm64
"${ARM64[@]}" app/main.m src/chargecap.c "${LIBS[@]}" "${APPLIBS[@]}" -o $B/app-arm64

ldid -S $B/chargecapd-armv7 $B/chargecap-armv7
ldid -S -Iuk.lbsi.chargecap $B/app-armv7
# iOS 15 kills a fake-signed binary at launch unless it claims platform-application.
ldid -Spackaging/chargecapd.entitlements -Iuk.lbsi.chargecapd $B/chargecapd-arm64
ldid -Spackaging/chargecapd.entitlements -Iuk.lbsi.chargecap-cli $B/chargecap-arm64
ldid -Spackaging/app.entitlements -Iuk.lbsi.chargecap $B/app-arm64

[ -f packaging/app/AppIcon76x76~ipad.png ] || swift tools/make_art.swift packaging/app

# make_deb <architecture> <install prefix> <slice> <minimum iOS> <required CPU>
make_deb() {
    local arch=$1 prefix=$2 slice=$3 root=build/root-$1
    local app=$root$prefix/Applications/ChargeCap.app
    mkdir -p $root/DEBIAN $root$prefix/usr/libexec $root$prefix/usr/bin $root$prefix/Library/LaunchDaemons $app
    cp $B/chargecapd-$slice $root$prefix/usr/libexec/chargecapd
    cp $B/chargecap-$slice $root$prefix/usr/bin/chargecap
    cp $B/app-$slice $app/ChargeCap
    cp packaging/app/*~ipad.png $app/
    sed "s/@VERSION@/$VERSION/g; s/@FIRMWARE@/$4/g; s/@CPU@/$5/g" packaging/app/Info.plist > $app/Info.plist
    plutil -convert binary1 $app/Info.plist
    sed "s#@PREFIX@#$prefix#g" packaging/uk.lbsi.chargecap.plist > $root$prefix/Library/LaunchDaemons/uk.lbsi.chargecap.plist
    sed "s/@VERSION@/$VERSION/g; s/@ARCH@/$arch/g; s/@FIRMWARE@/$4/g" packaging/control > $root/DEBIAN/control
    for s in postinst prerm postrm; do
        sed "s#@PREFIX@#$prefix#g" packaging/DEBIAN/$s > $root/DEBIAN/$s
        chmod 755 $root/DEBIAN/$s
    done
    find $root -name .DS_Store -delete
    dpkg-deb --root-owner-group -Zgzip -b $root "build/uk.lbsi.chargecap_${VERSION}_$arch.deb" >/dev/null
    echo "built build/uk.lbsi.chargecap_${VERSION}_$arch.deb"
}
make_deb iphoneos-arm "" armv7 9.0 armv7
make_deb iphoneos-arm64 /var/jb arm64 15.0 arm64

if [ "${1:-}" = install ]; then
    O=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o PubkeyAuthentication=no
       -o ConnectTimeout=8 -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa
       -o ControlMaster=auto -o "ControlPath=/tmp/cc-ctl-${IPAD##*.}" -o ControlPersist=600)
    # sshpass on this host sometimes misses the first password prompt, so retry.
    retry() { for i in 1 2 3 4; do "$@" && return 0; sleep 1; done; return 1; }
    on() { retry sshpass -p "$PASS" ssh "${O[@]}" "$IPAD" "$@"; }
    arch=$(on 'if [ -d /var/jb/usr/bin ]; then echo iphoneos-arm64; else echo iphoneos-arm; fi')
    sudo=""
    [ "${IPAD%%@*}" = root ] || sudo="echo '$PASS' | sudo -S -p ''"
    retry sshpass -p "$PASS" scp -O "${O[@]}" "build/uk.lbsi.chargecap_${VERSION}_$arch.deb" "$IPAD:/tmp/chargecap.deb"
    on "PATH=/var/jb/usr/bin:/var/jb/bin:\$PATH; $sudo dpkg -i /tmp/chargecap.deb && rm /tmp/chargecap.deb && chargecap"
fi
