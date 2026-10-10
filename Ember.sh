#!/bin/bash
# Ember launcher for ArkOS4Clone / dArkOS Ports.
# Lives at /roms/ports/Ember.sh, with the app in /roms/ports/ember/.
# Release packages include a prebuilt binary. If you change the source (or
# there is no binary yet), it is compiled here first, which takes about a minute.

DIR="$(cd "$(dirname "$0")" && pwd)/ember"
cd "$DIR" || exit 1

sudo chmod 666 /dev/tty1 2>/dev/null
printf "\033c" > /dev/tty1

needs_build=0
if [ ! -x ./ember ]; then
    needs_build=1
elif command -v make >/dev/null && [ -n "$(find src Makefile VERSION -newer ./ember 2>/dev/null)" ]; then
    needs_build=1
fi

if [ "$needs_build" = 1 ]; then
    echo "Ember: building, this takes about a minute..." > /dev/tty1
    if ! make > build.log 2>&1; then
        if [ -x ./ember ]; then
            echo "Build failed (see ember/build.log), starting the previous build" > /dev/tty1
            sleep 2
        else
            echo "Build failed - see ember/build.log" > /dev/tty1
            sleep 5
            exit 1
        fi
    fi
fi

./ember > ember.log 2>&1

printf "\033c" > /dev/tty1
