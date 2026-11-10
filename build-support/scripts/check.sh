#!/bin/sh
# Runs inside the build sandbox after `make` (see build.sh).
set -eu
INTERNAL="$PWD/build/internal"
mkdir -p "$INTERNAL/tests"
# Native core checks. No FMA contraction: results must equal the WASM build.
"$CC" -O2 -ffp-contract=off -std=c11 -Wall -Wextra -Werror -isysroot "$SDK" --ld-path="$LD" \
    -I "$INTERNAL/gen" tests/core-check.c -o "$INTERNAL/tests/core-check"
"$INTERNAL/tests/core-check"
if [ -f samples/DSC_0080.NEF ] && [ -f samples/DSC_0080.DNG ]; then
    "$INTERNAL/host-tools/js-runner" "$INTERNAL/wasm/engine.wasm" tests/wasm-check.js
    "$INTERNAL/host-tools/raw-reference" \
        "$INTERNAL/tests/DNG-plain.dng" "$INTERNAL/tests/DNG-plain.jpg" \
        "$INTERNAL/tests/DNG-swap.dng" "$INTERNAL/tests/DNG-swap.jpg"
fi
# Syntax of all web sources (module syntax stripped for a plain parse).
for SOURCE in web/app.js web/worker.js web/service-worker.js; do
    sed 's/^export //;s/import\.meta\.url/"file:\/\/\/x\/"/g' "$SOURCE" > "$INTERNAL/tmp/js-syntax.js"
    printf 'new (Object.getPrototypeOf(async function(){}).constructor)(readText("build/internal/tmp/js-syntax.js")); var testDone=true;\n' > "$INTERNAL/tmp/parse.js"
    "$INTERNAL/host-tools/js-runner" "$INTERNAL/wasm/engine.wasm" "$INTERNAL/tmp/parse.js"
done
sed -n '/<script type="module">/,/<\/script>/p' web/index.html | sed '1d;$d' > "$INTERNAL/tmp/shell-syntax.js"
printf 'new (Object.getPrototypeOf(async function(){}).constructor)(readText("build/internal/tmp/shell-syntax.js")); var testDone=true;\n' > "$INTERNAL/tmp/parse.js"
"$INTERNAL/host-tools/js-runner" "$INTERNAL/wasm/engine.wasm" "$INTERNAL/tmp/parse.js"
printf 'All checks passed.\n'
