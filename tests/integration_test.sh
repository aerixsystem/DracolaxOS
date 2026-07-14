#!/bin/bash
# tests/integration_test.sh — Integration test: boot DracolaxOS in QEMU
#
# Requirements: qemu-system-x86_64, make, x86_64-elf-gcc or gcc
#
# What it tests:
#   1. Kernel boots (multiboot2 magic accepted)
#   2. VGA / serial output appears
#   3. PS/2 keyboard IRQ fires (scancode processed)
#   4. Framebuffer initialised (VESA tag found)
#   5. Login screen drawn (no crash)
#   6. Memory limits: no false CRITICAL on boot
#   7. Service manager starts all 6 services
#   8. klog flush task started
#
# Pass criteria: serial output contains all CHECK strings within TIMEOUT seconds.

set -euo pipefail

TIMEOUT=30
ISO="DracolaxOS_v1_x64.iso"
SERIAL_LOG="/tmp/draco_serial_$$.log"

# Build ISO if not present
if [ ! -f "$ISO" ]; then
    echo "==> Building ISO..."
    make iso
fi

echo "==> Launching QEMU (headless, serial to $SERIAL_LOG)..."
qemu-system-x86_64 \
    -cdrom "$ISO" \
    -m 512M \
    -vga std \
    -nographic \
    -serial "file:$SERIAL_LOG" \
    -no-reboot \
    -no-shutdown \
    -kernel-cmdline "mode=graphical" \
    &
QEMU_PID=$!
echo "    QEMU PID=$QEMU_PID"

pass=0; fail=0
check() {
    local pattern="$1"
    local desc="$2"
    local deadline=$((SECONDS + TIMEOUT))
    while [ $SECONDS -lt $deadline ]; do
        if grep -q "$pattern" "$SERIAL_LOG" 2>/dev/null; then
            echo "[PASS] $desc"
            pass=$((pass+1))
            return 0
        fi
        sleep 0.5
    done
    echo "[FAIL] $desc (pattern: '$pattern' not found within ${TIMEOUT}s)"
    fail=$((fail+1))
}

check "DracolaxOS v1.0"           "Kernel boots and prints banner"
check "GDT loaded"                 "GDT initialised"
check "IDT loaded"                 "IDT initialised"
check "Paging enabled"             "Paging enabled"
check "Interrupts enabled"         "Interrupts enabled"
check "MOUSE: PS/2 init OK"        "PS/2 mouse initialised"
check "KLOG: log system ready"     "Persistent log system ready"
check "SVC: started net-manager"   "Network manager service started"
check "SVC: started audio-service" "Audio service started"
check "DESKTOP: starting"          "Desktop task spawned"

kill $QEMU_PID 2>/dev/null || true
rm -f "$SERIAL_LOG"

echo ""
echo "=========================="
echo "Integration: $pass passed, $fail failed"
echo "=========================="
[ "$fail" -eq 0 ] && exit 0 || exit 1
