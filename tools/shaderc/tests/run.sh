#!/bin/sh
# Shader compiler tests. Run with `make test`.
#
#   pass/  happy path shaders that must compile (shaderc runs dvp-as on a vertex shader's VU1 code);
#          the listing (shaderc --listing: the .pvs file as text, then the VU1 assembly) must match
#          pass/expected/
#   fail/  unhappy path shaders that must be rejected; the error message must match
#          fail/<shader>.expected
#
# UPDATE=1 rewrites the expected files from the current output.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SHADERC=$(cd "$HERE/.." && pwd)/shaderc
UPDATE=${UPDATE:-0}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

passed=0
failed=0

ok()   { passed=$((passed + 1)); echo "PASS $1"; }
bad()  { failed=$((failed + 1)); echo "FAIL $1: $2"; }

# ---------------------------------------------------------------- must compile
cd "$HERE/pass"
for src in *.vert *.frag; do
    name=$(echo "$src" | tr . _)

    if ! glslangValidator -V -g -o "$OUT/$src.spv" "$src" >"$OUT/$name.glslang"; then
        bad "$src" "glslangValidator failed: $(cat "$OUT/$name.glslang")"
        continue
    fi
    if ! "$SHADERC" --listing "$OUT/$src.spv" -o "$OUT/$name" 2>"$OUT/$name.err"; then
        bad "$src" "$(cat "$OUT/$name.err")"
        continue
    fi

    if [ "$UPDATE" = 1 ]; then
        mkdir -p expected
        cp "$OUT/$name.txt" "expected/$name.txt"
        ok "$src"
    elif diff -u "expected/$name.txt" "$OUT/$name.txt"; then
        ok "$src"
    else
        bad "$src" "listing differs from expected (diff above)"
    fi
done

# ---------------------------------------------------------------- must be rejected
cd "$HERE/fail"
for src in *.vert *.frag; do
    if ! glslangValidator -V -g -o "$OUT/$src.spv" "$src" >"$OUT/$src.glslang"; then
        bad "$src" "glslangValidator failed: $(cat "$OUT/$src.glslang")"
        continue
    fi
    if "$SHADERC" "$OUT/$src.spv" -o "$OUT/rejected" 2>"$OUT/$src.err"; then
        bad "$src" "compiled, but should have been rejected"
        continue
    fi

    if [ "$UPDATE" = 1 ]; then
        cp "$OUT/$src.err" "$src.expected"
        ok "$src"
    elif diff -u "$src.expected" "$OUT/$src.err"; then
        ok "$src"
    else
        bad "$src" "error message differs from expected (diff above)"
    fi
done

echo "$passed passed, $failed failed"
[ "$failed" = 0 ]
