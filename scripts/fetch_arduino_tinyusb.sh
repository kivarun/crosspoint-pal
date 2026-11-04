#!/usr/bin/env bash
# Fetch the arduino_tinyusb IDF component that the prebuilt Arduino core was
# built from, so custom_sdkconfig envs (e.g. x4c-blelp) link USB-MSC again.
#
# Sources, both pinned:
# - Component wrapper: pioarduino/esp32-arduino-lib-builder @ ce4c80a2f6cbf8c55ba246ffa2000fcdb33d9e6c
#   (CMakeLists.txt, Kconfig.projbuild, include/tusb_config.h; no tinyusb patches
#   are carried at that commit).
# - TinyUSB stack: hathach/tinyusb @ 53f8c53c2cbd73a91a172f1ae35e9abc00eb5075
#   (master, matches "tinyusb:" in the libs package versions.txt).
#
# Installs into <project>/components/arduino_tinyusb, which ESP-IDF scans
# automatically; PlatformIO does not compile that directory itself.
# Idempotent: re-running verifies pinned content and re-fetches what is
# missing or mismatched.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$PROJECT_DIR/components/arduino_tinyusb"
LIBBUILDER_COMMIT="ce4c80a2f6cbf8c55ba246ffa2000fcdb33d9e6c"
TINYUSB_COMMIT="53f8c53c2cbd73a91a172f1ae35e9abc00eb5075"
BASE_URL="https://raw.githubusercontent.com/pioarduino/esp32-arduino-lib-builder/$LIBBUILDER_COMMIT/components/arduino_tinyusb"

# sha256 of each wrapper file at the pinned lib-builder commit.
FILE_SHA256="
CMakeLists.txt 88fc27316d8908ee9777c8ce08386af054297060ef8b6d2cf4650d1fd77b495c
Kconfig.projbuild 8096e2a0369df9a70eacf40982b15ec1c22691ccd10857700a33b5bde940ad40
include/tusb_config.h e6812ac623dc6f5f474c0961b5ee065d82867d1440765c85a4662c2db4d5a831
"

fetch_file() {
    local rel="$1"
    local url="$BASE_URL/$rel"
    local tmp="$DEST/$rel.tmp"
    echo "fetch_arduino_tinyusb: downloading $rel"
    mkdir -p "$(dirname "$DEST/$rel")"
    curl -sf "$url" -o "$tmp"
    mv "$tmp" "$DEST/$rel"
}

verify_file() {
    local rel="$1"
    local expected="$2"
    if [ ! -f "$DEST/$rel" ]; then
        fetch_file "$rel"
    fi
    local actual
    actual="$(sha256sum "$DEST/$rel" | cut -d' ' -f1)"
    if [ "$actual" != "$expected" ]; then
        echo "fetch_arduino_tinyusb: $rel content mismatch, re-fetching" >&2
        rm -f "$DEST/$rel"
        fetch_file "$rel"
        actual="$(sha256sum "$DEST/$rel" | cut -d' ' -f1)"
        if [ "$actual" != "$expected" ]; then
            echo "fetch_arduino_tinyusb: $rel still mismatched after re-fetch" >&2
            exit 1
        fi
    fi
}

while read -r rel expected; do
    [ -n "$rel" ] || continue
    verify_file "$rel" "$expected"
done <<< "$FILE_SHA256"

# Drift guard: the fetched stack must stay the one the installed prebuilt core
# was built from. A libs-package bump changes versions.txt before the script
# is re-pinned — fail loudly here instead of silently changing USB composition.
LIBS_DIR="${HOME}/.platformio/packages/framework-arduinoespressif32-libs"
VERSIONS="$LIBS_DIR/esp32s3/versions.txt"
if [ -f "$VERSIONS" ]; then
    # versions.txt truncates the hash (e.g. 53f8c53c2); accept an unambiguous
    # prefix of the pin, either direction, and fail on a real mismatch.
    pinned="$(echo "$TINYUSB_COMMIT" | cut -c1-12)"
    shipped="$(awk '/^tinyusb:/ {print $NF; exit}' "$VERSIONS")"
    case "$pinned" in
        "$shipped"*) ;;
        *)
            case "$shipped" in
                "$pinned"*) ;;
                *)
                    echo "fetch_arduino_tinyusb: TinyUSB drift: libs package was built from $shipped, script pins $pinned" >&2
                    echo "fetch_arduino_tinyusb: re-pin TINYUSB_COMMIT in scripts/fetch_arduino_tinyusb.sh to the new core's stack" >&2
                    exit 1
                    ;;
            esac
            ;;
    esac
else
    echo "fetch_arduino_tinyusb: note: $VERSIONS not found, drift check skipped" >&2
fi

state_changed=0
if [ -d "$DEST/tinyusb/.git" ]; then
    # Pin enforcement is content-level: a dirty or drifted checkout is reset to
    # the pinned commit, not just its HEAD compared.
    actual="$(git -C "$DEST/tinyusb" rev-parse HEAD)"
    dirty="$(git -C "$DEST/tinyusb" status --porcelain)"
    if [ "$actual" != "$TINYUSB_COMMIT" ] || [ -n "$dirty" ]; then
        if [ "$actual" != "$TINYUSB_COMMIT" ]; then
            echo "fetch_arduino_tinyusb: wrong TinyUSB revision $actual, forcing $TINYUSB_COMMIT" >&2
            git -C "$DEST/tinyusb" fetch origin "$TINYUSB_COMMIT"
        else
            echo "fetch_arduino_tinyusb: dirty TinyUSB checkout, resetting to the pinned commit" >&2
        fi
        git -C "$DEST/tinyusb" reset --hard "$TINYUSB_COMMIT"
        git -C "$DEST/tinyusb" clean -qfd
        state_changed=1
    fi
    actual="$(git -C "$DEST/tinyusb" rev-parse HEAD)"
    if [ "$actual" != "$TINYUSB_COMMIT" ]; then
        echo "fetch_arduino_tinyusb: TinyUSB checkout does not match the pin after reset" >&2
        exit 1
    fi
else
    # A tree without .git cannot be verified; re-clone it even if files exist.
    if [ -f "$DEST/tinyusb/src/tusb.h" ]; then
        echo "fetch_arduino_tinyusb: TinyUSB tree without .git, re-cloning" >&2
        state_changed=1
    fi
    echo "fetch_arduino_tinyusb: cloning TinyUSB"
    rm -rf "$DEST/tinyusb"
    git clone --filter=blob:none https://github.com/hathach/tinyusb.git "$DEST/tinyusb"
    git -C "$DEST/tinyusb" checkout --detach "$TINYUSB_COMMIT"
    state_changed=1
fi

if [ ! -f "$DEST/tinyusb/src/tusb.h" ]; then
    echo "fetch_arduino_tinyusb: TinyUSB source tree incomplete" >&2
    exit 1
fi

# The hybrid rebuild cache keys on custom_sdkconfig only, not on the presence
# of this component: a build that ran before the component existed cached the
# USB-less libs under the same request. Busting the recorded request here makes
# the documented recovery (build fails -> fetch -> rebuild) actually rebuild.
if [ "$state_changed" != "0" ]; then
    rm -f "$LIBS_DIR/esp32s3/sdkconfig.crosspoint"
fi

echo "fetch_arduino_tinyusb: ready at $DEST"
echo "  wrapper: pioarduino/esp32-arduino-lib-builder @ $LIBBUILDER_COMMIT"
echo "  tinyusb: hathach/tinyusb @ $(git -C "$DEST/tinyusb" rev-parse HEAD)"
