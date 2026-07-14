#!/bin/bash
# tests/run_compat_tests.sh
#
# Run DracolaxOS compatibility tests in QEMU.
# Boots the ISO, sends shell commands over serial, checks output.
#
# Usage:
#   ./tests/run_compat_tests.sh [path/to/dracolaxos.iso]
#
# Requirements: qemu-system-i386, expect, socat

set -euo pipefail

ISO="${1:-dracolaxos.iso}"
SERIAL_LOG="/tmp/draco_test_$$.log"
PASS=0
FAIL=0

cleanup() {
    kill "$QPID" 2>/dev/null || true
    rm -f "$SERIAL_LOG" /tmp/draco_serial_$$
}
trap cleanup EXIT

echo "=== DracolaxOS Compatibility Tests ==="
echo "ISO: $ISO"
echo ""

# Start QEMU with serial redirected to a file
mkfifo /tmp/draco_serial_$$ 2>/dev/null || true

qemu-system-i386 \
    -cdrom "$ISO" \
    -m 128M \
    -serial stdio \
    -no-reboot \
    -display none \
    > "$SERIAL_LOG" 2>&1 &
QPID=$!

echo "QEMU PID: $QPID"
echo "Waiting 20s for boot..."
sleep 20

check() {
    local desc="$1"
    local pattern="$2"
    if grep -q "$pattern" "$SERIAL_LOG" 2>/dev/null; then
        echo "  PASS: $desc"
        PASS=$((PASS+1))
    else
        echo "  FAIL: $desc (pattern='$pattern')"
        FAIL=$((FAIL+1))
    fi
}

echo ""
echo "--- Boot checks ---"
check "Kernel banner printed"        "DracolaxOS"
check "GDT initialised"              "GDT loaded"
check "IDT initialised"              "IDT loaded"
check "Paging enabled"               "Paging enabled"
check "Interrupts enabled"           "Interrupts enabled"
check "Init task started"            "INIT: starting"
check "RAMFS mounted"                "RAMFS"
check "procfs mounted"               "/proc"
check "Draco Shield initialised"     "SHIELD.*initialised\|Draco Shield"
check "Linux compat layer ready"     "LINUX: compatibility"
check "Syscall INT 0x80 registered"  "SYSCALL"
check "Shell launched"               "INIT: launching shell"

echo ""
echo "--- Summary ---"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "Serial output dump:"
    cat "$SERIAL_LOG" | head -60
    exit 1
fi

echo "All tests passed!"
exit 0
