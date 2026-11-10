#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$ROOT"
TARGET=${1:-all}
case "$TARGET" in all|web|check|serve|clean|rebuild-toolchain) ;; *) echo "Unknown build target: $TARGET" >&2; exit 2;; esac
INTERNAL="$ROOT/build/internal"
LOCK="$INTERNAL/build.lock"
CLEAN_ACL='group:everyone deny add_file,add_subdirectory'
seal_tree() {
    [ -d "$1" ] && [ ! -L "$1" ] || return 0
    # Seal before listing children: no new entries can appear behind the
    # traversal. Deleting existing entries is still allowed.
    /bin/chmod +a# 0 "$CLEAN_ACL" "$1" || return 1
    printf '%s\0' "$1" >&4 || { /bin/chmod -a# 0 "$1"; return 1; }
    for SEAL_CHILD in "$1/"* "$1/".[!.]* "$1/"..?*; do
        seal_tree "$SEAL_CHILD" || return 1
    done
}
unseal_tree() {
    [ -d "$1" ] && [ ! -L "$1" ] || return 0
    /usr/bin/xargs -0 /bin/sh -c '
        for SEALED_DIR do
            [ -d "$SEALED_DIR" ] && [ ! -L "$SEALED_DIR" ] || continue
            /bin/chmod -h -a# 0 "$SEALED_DIR" || exit 1
        done
    ' sh <&3
}
remove_tree() (
    # Finder may recreate .DS_Store while rm traverses an open directory.
    # Deny creation temporarily, then delete once. Change directories only;
    # symlink targets and hard-linked files must keep their permissions.
    trap 'exit 130' INT TERM HUP
    if [ -d "$1" ] && [ ! -L "$1" ]; then
        # Track only directories successfully sealed. Keep an unlinked journal
        # open outside the target, including when deleting the lock itself.
        JOURNAL=$(/usr/bin/mktemp "$LOCK/.clean.XXXXXX")
        trap '/bin/rm -f "$JOURNAL"' EXIT
        exec 3< "$JOURNAL" 4> "$JOURNAL"
        /bin/unlink "$JOURNAL"
        # Remove exactly the entry inserted at index 0, even if an identical
        # rule already existed. Skip directories already deleted.
        trap 'unseal_tree "$1"' EXIT
        seal_tree "$1" || exit 1
    fi
    /bin/rm -rf "$1"
)
release_lock() {
    remove_tree "$LOCK"
    if [ "$TARGET" = clean ]; then
        # Only remove empty parents after unlocking. A new build or Finder can
        # already be using them; never recursively delete them at this point.
        /bin/rmdir "$INTERNAL" 2>/dev/null || :
        /bin/rmdir "$ROOT/build" 2>/dev/null || :
    fi
}
if [ "${IRCONV_BUILD_INNER:-}" != 1 ]; then
    case "$ROOT" in *'"'*|*'\'*) echo 'Unsupported quotation character in project path' >&2; exit 2;; esac
    mkdir -p "$INTERNAL"
    if ! mkdir "$LOCK" 2>/dev/null; then
        if [ -f "$LOCK/pid" ]; then
            OWNER=$(cat "$LOCK/pid")
            if ! kill -0 "$OWNER" 2>/dev/null; then
                echo "Interrupted build lock ($OWNER). Remove build/internal/build.lock after verifying no build is running." >&2; exit 1
            fi
        fi
        echo 'Another build is running. Retry after it finishes; clean never interrupts it.' >&2
        exit 1
    fi
    echo $$ > "$LOCK/pid"
    trap 'release_lock' EXIT
    trap 'exit 130' INT TERM HUP
    # Acquire the lock before creating temporary files or replacing the profile.
    mkdir -p "$INTERNAL/tmp" "$INTERNAL/cache" "$INTERNAL/logs"
    cat > "$INTERNAL/build.sb" <<PROFILE
(version 1)
(allow default)
(deny network*)
(deny file-read* (subpath "/opt/homebrew") (subpath "/usr/local"))
(deny file-write*)
(allow file-write* (subpath "$ROOT/build") (literal "/dev/null"))
PROFILE
    /usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin TMPDIR="$INTERNAL/tmp" \
        CLANG_MODULE_CACHE_PATH="$INTERNAL/cache/modules" IRCONV_BUILD_INNER=1 \
        IRCONV_DEVELOPER_DIR="${DEVELOPER_DIR:-}" \
        /usr/bin/sandbox-exec -f "$INTERNAL/build.sb" /bin/sh "$0" "$TARGET"
    if [ "$TARGET" = serve ]; then
        release_lock; trap - EXIT INT TERM HUP
        exec "$INTERNAL/host-tools/server" "$ROOT/build/web" 8080
    fi
    exit 0
fi
if [ "$TARGET" = clean ]; then
    # Keep the lock's directory alive until all generated contents are gone.
    for CLEAN_PARENT in "$ROOT/build" "$INTERNAL"; do
        for CLEAN_PATH in "$CLEAN_PARENT/"* "$CLEAN_PARENT/".[!.]* "$CLEAN_PARENT/"..?*; do
            [ "$CLEAN_PATH" != "$INTERNAL" ] && [ "$CLEAN_PATH" != "$LOCK" ] || continue
            remove_tree "$CLEAN_PATH"
        done
    done
    exit 0
fi
if [ "$TARGET" = rebuild-toolchain ]; then rm -rf "$INTERNAL/toolchain"; TARGET=web; fi
DEVELOPER=${IRCONV_DEVELOPER_DIR:-$(/usr/bin/xcode-select -p)}
if [ -x "$DEVELOPER/usr/bin/clang" ]; then
    CC="$DEVELOPER/usr/bin/clang"; LD="$DEVELOPER/usr/bin/ld"; SDK="$DEVELOPER/SDKs/MacOSX.sdk"
else
    CC="$DEVELOPER/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang"
    LD="$DEVELOPER/Toolchains/XcodeDefault.xctoolchain/usr/bin/ld"
    SDK="$DEVELOPER/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
fi
[ -x "$CC" ] && [ -x "$LD" ] && [ -d "$SDK" ] || { echo 'Install/select the Apple Command Line Tools first.' >&2; exit 1; }
SDKROOT=$SDK
export CC SDK LD SDKROOT
mkdir -p "$INTERNAL/state" "$INTERNAL/host-tools" "$INTERNAL/tests" "$INTERNAL/gen"
DESCRIPTOR="$INTERNAL/tmp/compiler-state"
{ "$CC" --version; printf '%s\n' "$CC" "$SDK"; /usr/bin/stat -f '%z %m' "$CC"; cat "$SDK/SDKSettings.json"; } > "$DESCRIPTOR"
ICC_SOURCE='/System/Library/ColorSync/Profiles/sRGB Profile.icc'
ICC_P3_SOURCE='/System/Library/ColorSync/Profiles/Display P3.icc'
ICC_ADOBE_SOURCE='/System/Library/ColorSync/Profiles/AdobeRGB1998.icc'
snapshot() {
    SNAP="$INTERNAL/tmp/$1-inputs"
    rm -rf "$SNAP"; mkdir -p "$SNAP"
    cp "$DESCRIPTOR" "$SNAP/compiler"
    cp build-support/scripts/build.sh "$SNAP/recipe"
    shift
    for INPUT in "$@"; do
        mkdir -p "$SNAP/$(dirname "$INPUT")"
        cp -R "$INPUT" "$SNAP/$INPUT"
    done
}
unchanged() { [ -d "$INTERNAL/state/$1" ] && /usr/bin/diff -qr "$SNAP" "$INTERNAL/state/$1" >/dev/null; }
remember() { rm -rf "$INTERNAL/state/$1"; mv "$SNAP" "$INTERNAL/state/$1"; }
host_tool() {
    NAME=$1; shift
    snapshot "host-$NAME" "build-support/tools/$NAME.m"
    if [ ! -x "$INTERNAL/host-tools/$NAME" ] || ! unchanged "host-$NAME"; then
        "$CC" -O2 -fobjc-arc -isysroot "$SDK" --ld-path="$LD" "build-support/tools/$NAME.m" "$@" -o "$INTERNAL/tmp/$NAME"
        mv "$INTERNAL/tmp/$NAME" "$INTERNAL/host-tools/$NAME"
        remember "host-$NAME"
    fi
}
build_linker() {
    ARCHIVE=; COUNT=0
    for FILE in vendor/toolchain/xcc/* vendor/toolchain/xcc/.[!.]*; do
        [ -f "$FILE" ] || continue
        [ "$(basename "$FILE")" != .DS_Store ] || continue
        ARCHIVE=$FILE; COUNT=$((COUNT + 1))
    done
    [ "$COUNT" = 1 ] || { echo 'vendor/toolchain/xcc must contain exactly one original source archive.' >&2; exit 1; }
    snapshot linker build-support/patches/xcc-host-only.patch
    cp "$ARCHIVE" "$SNAP/archive"
    LINKER="$INTERNAL/toolchain/xcc/source/wcc"
    if [ -x "$LINKER" ] && unchanged linker; then return; fi
    echo 'Building the small WASM linker (XCC wcc) from its original source archive…'
    rm -rf "$INTERNAL/toolchain/xcc"
    mkdir -p "$INTERNAL/toolchain/xcc/unpack"
    /usr/bin/tar -xf "$ARCHIVE" -C "$INTERNAL/toolchain/xcc/unpack"
    set -- "$INTERNAL/toolchain/xcc/unpack/"*
    [ "$#" = 1 ] && [ -d "$1" ] || { echo 'Expected one upstream source root in archive.' >&2; exit 1; }
    mv "$1" "$INTERNAL/toolchain/xcc/source"
    (cd "$INTERNAL/toolchain/xcc/source" && /usr/bin/patch -p1 < "$ROOT/build-support/patches/xcc-host-only.patch" && \
        /usr/bin/make CC="\"$CC\"" OPTIMIZE=-O2 LDFLAGS="--ld-path=\"$LD\"" wcc) > "$INTERNAL/logs/xcc.log" 2>&1 || { cat "$INTERNAL/logs/xcc.log" >&2; exit 1; }
    remember linker
}
# The unmodified macOS ICC profiles (sRGB, Display P3, Adobe RGB) are embedded
# into exported JPEGs; their colorants also define the output colour spaces.
generate_one_icc() {
    [ -f "$1" ] || { echo "Missing $1" >&2; exit 1; }
    cp "$1" "$INTERNAL/tmp/$2.icc"
    (cd "$INTERNAL/tmp" && /usr/bin/xxd -i -n "$2" "$2.icc") > "$INTERNAL/gen/$2.h.tmp"
    mv "$INTERNAL/gen/$2.h.tmp" "$INTERNAL/gen/$2.h"
}
generate_sources() {
    generate_one_icc "$ICC_SOURCE" srgb_icc
    generate_one_icc "$ICC_P3_SOURCE" p3_icc
    generate_one_icc "$ICC_ADOBE_SOURCE" adobergb_icc
}
WEB_FILES='app.js app.css worker.js'
build_web() {
    build_linker
    host_tool package-check -framework Foundation -framework CoreGraphics -framework ImageIO
    host_tool site-assets -framework Foundation -framework CoreGraphics -framework ImageIO
    host_tool js-runner -framework Foundation -framework JavaScriptCore
    generate_sources
    snapshot web core web assets build-support/patches/xcc-host-only.patch build-support/tools/package-check.m build-support/tools/site-assets.m build-support/tools/js-runner.m tests/wasm-smoke.js vendor/SOURCE_ARCHIVES.md
    cp "$INTERNAL/state/linker/archive" "$SNAP/linker-archive"
    for ICC_HEADER in srgb_icc p3_icc adobergb_icc; do cp "$INTERNAL/gen/$ICC_HEADER.h" "$SNAP/$ICC_HEADER.h"; done
    if [ ! -d build/web ] && [ -d "$INTERNAL/web/previous" ]; then mv "$INTERNAL/web/previous" build/web; fi
    if [ -f build/web/index.html ] && unchanged web && "$INTERNAL/host-tools/package-check" build/web; then echo 'Web app is current.'; return; fi
    WEB_SNAP=$SNAP
    echo 'Building the WASM engine and the offline website…'
    mkdir -p "$INTERNAL/wasm"
    "$CC" --target=wasm32-unknown-unknown -ffreestanding -fno-builtin -nostdlibinc -isystem "$("$CC" -print-resource-dir)/include" \
        -O3 -std=c11 -Wall -Wextra -Werror -I "$INTERNAL/gen" \
        -c web/native/wasm-adapter.c -o "$INTERNAL/wasm/engine.o"
    set --
    for NAME in $(/usr/bin/sed -n 's/^EXPORT [^(]*[ *]\([a-z_0-9]*\)(.*/\1/p' web/native/wasm-adapter.c); do set -- "$@" -e "$NAME"; done
    [ "$#" -gt 10 ] || { echo 'No WASM exports found.' >&2; exit 1; }
    "$LINKER" -nostdlib --entry-point= --stack-size=1048576 "$@" \
        -o "$INTERNAL/wasm/engine.wasm" "$INTERNAL/wasm/engine.o"
    "$INTERNAL/host-tools/js-runner" "$INTERNAL/wasm/engine.wasm" tests/wasm-smoke.js
    RELEASE=$(/usr/bin/uuidgen | tr '[:upper:]' '[:lower:]')
    STAGE="$INTERNAL/web/staging"
    rm -rf "$STAGE"; mkdir -p "$STAGE/releases/$RELEASE"
    if [ -d build/web/releases ]; then cp -R build/web/releases/. "$STAGE/releases/"; fi
    for FILE in $WEB_FILES; do cp "web/$FILE" "$STAGE/releases/$RELEASE/"; done
    cp "$INTERNAL/wasm/engine.wasm" "$STAGE/releases/$RELEASE/"
    cp vendor/XCC-LICENSE.txt "$STAGE/releases/$RELEASE/XCC-LICENSE.txt"
    printf '%s\n' "$RELEASE" > "$STAGE/release.txt"
    "$INTERNAL/host-tools/site-assets" assets/icon.png assets/icon-maskable.png "$STAGE" "$RELEASE" web/index.html
    /usr/bin/sed "s/__RELEASE__/$RELEASE/g" web/service-worker.js > "$STAGE/service-worker.js"
    INVENTORY="$STAGE/releases/$RELEASE/offline-resources.json"
    printf '{"release":"%s","resources":[\n' "$RELEASE" > "$INVENTORY"
    FIRST=1
    for FILE in "$STAGE/releases/$RELEASE/"*; do
        [ "$FILE" != "$INVENTORY" ] || continue
        REL=${FILE#"$STAGE/"}; SIZE=$(/usr/bin/stat -f '%z' "$FILE")
        [ "$FIRST" = 1 ] || printf ',\n' >> "$INVENTORY"
        FIRST=0; printf ' {"path":"%s","size":%s}' "$REL" "$SIZE" >> "$INVENTORY"
    done
    printf '\n]}\n' >> "$INVENTORY"
    "$INTERNAL/host-tools/package-check" "$STAGE"
    PREVIOUS="$INTERNAL/web/previous"
    rm -rf "$PREVIOUS"
    if [ -d build/web ]; then mv build/web "$PREVIOUS"; fi
    if ! mv "$STAGE" build/web; then [ ! -d "$PREVIOUS" ] || mv "$PREVIOUS" build/web; exit 1; fi
    rm -rf "$PREVIOUS"
    SNAP=$WEB_SNAP; remember web
}
case "$TARGET" in
    all|web|check|serve) build_web;;
esac
if [ "$TARGET" = serve ]; then host_tool server -framework Foundation; fi
if [ "$TARGET" = check ]; then
    host_tool server -framework Foundation
    host_tool raw-reference -framework Foundation -framework CoreGraphics -framework ImageIO -framework CoreImage
    /bin/sh build-support/scripts/check.sh
fi
