#!/bin/bash
# tests/run_all_tests.sh — Run all host-side unit tests
set -e
PASS=0; FAIL=0

run_test() {
    local src="$1"
    local bin="/tmp/$(basename ${src%.c})"
    echo "==> Building $src"
    gcc -O2 -Wall -o "$bin" "$src" 2>&1 || { echo "[FAIL] build failed: $src"; FAIL=$((FAIL+1)); return; }
    echo "==> Running $bin"
    if "$bin"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

run_test "$SCRIPT_DIR/test_allocator.c"
run_test "$SCRIPT_DIR/test_keyboard.c"
run_test "$SCRIPT_DIR/test_log_rotation.c"
run_test "$SCRIPT_DIR/test_wm.c"

# LXScript parser test (needs extra sources)
echo "==> Building LXScript parser test"
bin="/tmp/test_lxs_parser"
if gcc -O2 -Wall -std=c11 -I"$SCRIPT_DIR/../lxscript" \
     "$SCRIPT_DIR/test_lxscript_parser.c" \
     "$SCRIPT_DIR/../lxscript/lexer/lexer.c" \
     "$SCRIPT_DIR/../lxscript/parser/parser.c" \
     -o "$bin" 2>&1; then
    echo "==> Running $bin"
    if "$bin"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi
else
    echo "[FAIL] build failed: test_lxscript_parser.c"
    FAIL=$((FAIL+1))
fi

echo ""
echo "=========================="
echo "Test results: $PASS passed, $FAIL failed"
echo "=========================="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
