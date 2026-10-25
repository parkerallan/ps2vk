#!/bin/sh
# Shader compiler tests. Run with `make test`.
#
#   pass/  happy path shaders that must compile; the generated .vsm/.c files must match
#          pass/expected/, and the .vsm must assemble with dvp-as (if on PATH)
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

if ! command -v dvp-as >/dev/null 2>&1; then
    echo "note: dvp-as not on PATH, skipping the VU1 assembly check"
fi

# ---------------------------------------------------------------- must compile
cd "$HERE/pass"
for src in *.vert *.frag; do
    name=$(echo "$src" | tr . _)

    if ! glslangValidator -V -g -o "$OUT/$src.spv" "$src" >"$OUT/$name.glslang"; then
        bad "$src" "glslangValidator failed: $(cat "$OUT/$name.glslang")"
        continue
    fi
    if ! "$SHADERC" "$OUT/$src.spv" -o "$OUT/$name" 2>"$OUT/$name.err"; then
        bad "$src" "$(cat "$OUT/$name.err")"
        continue
    fi

    same=1
    for ext in vsm c; do
        [ -f "$OUT/$name.$ext" ] || continue
        if [ "$UPDATE" = 1 ]; then
            mkdir -p expected
            cp "$OUT/$name.$ext" "expected/$name.$ext"
        elif ! diff -u "expected/$name.$ext" "$OUT/$name.$ext"; then
            same=0
        fi
    done

    if [ "$same" = 0 ]; then
        bad "$src" "output differs from expected (diff above)"
    elif [ -f "$OUT/$name.vsm" ] && command -v dvp-as >/dev/null 2>&1 &&
         ! dvp-as "$OUT/$name.vsm" -o "$OUT/$name.o" 2>"$OUT/$name.as"; then
        bad "$src" "dvp-as rejected the VU1 code: $(cat "$OUT/$name.as")"
    else
        ok "$src"
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
