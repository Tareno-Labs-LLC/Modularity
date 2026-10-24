#!/bin/sh
# ModuProbe - one-shot device report for porting the Modularity player to a handheld.
#
# Drop this into the firmware's Ports folder, launch it once from the menu, then copy
# ModuProbe-report.txt (written next to this script) back to the PC. It only reads; it
# installs and changes nothing.
#
# What it answers: is there a usable GLES 2.0 / EGL driver (and which vendor), what does
# the framebuffer look like, which libc and SDL the player would link against, and how the
# frontend launches a port (cwd, user, environment, parent process).
#
# Plain POSIX sh on purpose: these firmwares ship busybox, not bash.

HERE=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
[ -n "$HERE" ] || HERE=/tmp
OUT="$HERE/ModuProbe-report.txt"
[ -w "$HERE" ] || OUT=/tmp/ModuProbe-report.txt

section() {
    echo ""
    echo "===== $1 ====="
}

run() {
    echo "\$ $*"
    "$@" 2>&1
}

# Search the usual library roots only; a full / walk on an SD card takes minutes and wanders
# into the ROM folders.
LIBDIRS="/lib /usr/lib /usr/local/lib /opt /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf"

{
    echo "ModuProbe report"
    date 2>/dev/null

    section "launch context"
    echo "script: $0"
    echo "cwd: $(pwd)"
    echo "report dir: $HERE"
    run id
    echo "parent process:"
    PPID_CMD=$(tr '\0' ' ' < /proc/$PPID/cmdline 2>/dev/null)
    echo "  pid $PPID: $PPID_CMD"
    section "environment"
    env 2>/dev/null | sort

    section "system"
    run uname -a
    cat /etc/os-release 2>/dev/null || cat /etc/issue 2>/dev/null
    head -n 30 /proc/cpuinfo 2>/dev/null
    head -n 5 /proc/meminfo 2>/dev/null
    run df -h

    section "libc"
    ls -l /lib/libc.so* /lib/ld-linux* /lib/ld-musl* /lib/libc-* 2>/dev/null
    # glibc prints its version when executed directly.
    for c in /lib/libc.so.6 /lib/arm-linux-gnueabihf/libc.so.6; do
        [ -x "$c" ] && "$c" 2>&1 | head -n 1
    done
    ldd --version 2>&1 | head -n 1

    section "GL / EGL / PowerVR libraries"
    for d in $LIBDIRS; do
        [ -d "$d" ] || continue
        find "$d" -maxdepth 3 \( -iname 'libGLES*' -o -iname 'libEGL*' -o -iname 'libGL.so*' \
            -o -iname '*pvr*' -o -iname 'libIMG*' -o -iname 'libsrv*' -o -iname 'libusc*' \
            -o -iname 'libgbm*' -o -iname 'libdrm*' -o -iname 'libmali*' \) 2>/dev/null
    done | sort -u
    section "GPU kernel side"
    ls -l /dev/pvrsrvkm /dev/dri /dev/mali* /dev/galcore 2>&1
    grep -i -E 'pvr|sgx|img|mali|drm' /proc/modules 2>/dev/null
    grep -i -E 'pvr|sgx|mali|drm' /proc/devices 2>/dev/null
    ls /sys/class/drm 2>/dev/null

    section "framebuffer"
    ls -l /dev/fb* 2>&1
    for f in virtual_size bits_per_pixel stride name modes; do
        v=$(cat /sys/class/graphics/fb0/$f 2>/dev/null)
        [ -n "$v" ] && echo "fb0 $f: $v"
    done

    section "SDL"
    for d in $LIBDIRS; do
        [ -d "$d" ] || continue
        find "$d" -maxdepth 3 -iname 'libSDL*' 2>/dev/null
    done | sort -u
    sdl2-config --version 2>/dev/null && echo "(sdl2-config present)"

    section "other libraries the player uses"
    for d in $LIBDIRS; do
        [ -d "$d" ] || continue
        find "$d" -maxdepth 3 \( -iname 'libstdc++*' -o -iname 'libasound*' -o -iname 'libfreetype*' \
            -o -iname 'libz.so*' -o -iname 'libpng*' -o -iname 'libopenal*' -o -iname 'libpthread*' \) 2>/dev/null
    done | sort -u

    section "input + audio"
    ls -l /dev/input 2>&1
    cat /proc/bus/input/devices 2>/dev/null
    aplay -l 2>&1 | head -n 20

    section "running processes"
    ps 2>/dev/null | head -n 60

    section "where ports and apps live"
    for d in /userdata/roms/ports /mnt/SDCARD /mnt/mmc /mnt/sdcard /media /userdata /roms; do
        [ -d "$d" ] && { echo "$d:"; ls "$d" 2>/dev/null | head -n 40; }
    done

    echo ""
    echo "done"
} > "$OUT" 2>&1

sync
# Leave the message on screen briefly in case the frontend shows a console.
echo "ModuProbe: report written to $OUT"
sleep 2
exit 0
