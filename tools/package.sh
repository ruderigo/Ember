#!/bin/bash
# Builds ready-to-install packages in dist/ from an already built ./ember.
#
#   dist/ember-arkos-<arch>.zip      unzip into the ports folder on the SD card
#   dist/ember-arkos-<arch>.tar.gz   same contents, for installing over SSH
#   dist/SHA256SUMS.txt
#
# Each package holds Ember.sh plus an ember/ folder with the prebuilt binary
# and the full source, so the handheld can still rebuild it with `make`.
set -euo pipefail
cd "$(dirname "$0")/.."

[ -x ./ember ] || { echo "build ./ember first (make)" >&2; exit 1; }
VERSION=$(cat VERSION)
case "$(uname -m)" in
    aarch64|arm64) ARCH=arm64 ;;
    *) ARCH=$(uname -m) ;;
esac
NAME="ember-arkos-$ARCH"

rm -rf dist
STAGE=dist/stage
mkdir -p "$STAGE/ember"
cp -p Ember.sh "$STAGE/"
chmod +x "$STAGE/Ember.sh"
cp -pr src tools Makefile VERSION README.md CHANGELOG.md LICENSE "$STAGE/ember/"
cp ember "$STAGE/ember/ember"
strip "$STAGE/ember/ember" 2>/dev/null || true
# the binary must be newer than the sources, or the launcher rebuilds it
touch "$STAGE/ember/ember"

(cd "$STAGE" && tar czf "../$NAME.tar.gz" --owner=0 --group=0 Ember.sh ember)
(cd "$STAGE" && zip -qrX "../$NAME.zip" Ember.sh ember)
rm -rf "$STAGE"
(cd dist && sha256sum ./*.zip ./*.tar.gz > SHA256SUMS.txt)

echo "Ember $VERSION packages:"
ls -l dist
