#!/bin/bash
# Ember launcher for ArkOS4Clone / dArkOS Ports.
# Lives at /roms/ports/Ember.sh, with the app in /roms/ports/ember/.
# On first launch it compiles the app on the device (about a minute).

DIR="$(cd "$(dirname "$0")" && pwd)/ember"
cd "$DIR" || exit 1

sudo chmod 666 /dev/tty1 2>/dev/null
printf "\033c" > /dev/tty1

if [ ! -x ./ember ] || [ -n "$(find src Makefile -newer ./ember 2>/dev/null)" ]; then
    echo "Ember: building, this takes about a minute on first launch..." > /dev/tty1
    if ! make > build.log 2>&1; then
        echo "Build failed - see ember/build.log" > /dev/tty1
        sleep 5
        exit 1
    fi
fi

./ember > ember.log 2>&1

printf "\033c" > /dev/tty1
