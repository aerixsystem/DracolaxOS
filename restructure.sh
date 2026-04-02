#!/usr/bin/env bash
# DracolaxOS Phase 0 — Restructure Script
# Moves all files to the canonical layout defined in DRACOLAX_ROADMAP.md
# Run from the root of the DracolaxOS project directory.
set -e

mv_safe() {
    local src="$1" dst="$2"
    if [ -e "$src" ]; then
        mkdir -p "$(dirname "$dst")"
        mv "$src" "$dst"
        echo "  MV  $src -> $dst"
    else
        echo "  SKIP (not found): $src"
    fi
}

echo "=== DracolaxOS Phase 0: Restructure ==="

# ─── kernel/arch/x86_64 ────────────────────────────────────────────────────
echo "[kernel/arch/x86_64]"
mv_safe kernel/gdt.c        kernel/arch/x86_64/gdt.c
mv_safe kernel/gdt.h        kernel/arch/x86_64/gdt.h
mv_safe kernel/idt.c        kernel/arch/x86_64/idt.c
mv_safe kernel/idt.h        kernel/arch/x86_64/idt.h
mv_safe kernel/tss.c        kernel/arch/x86_64/tss.c
mv_safe kernel/tss.h        kernel/arch/x86_64/tss.h
mv_safe kernel/irq.c        kernel/arch/x86_64/irq.c
mv_safe kernel/irq.h        kernel/arch/x86_64/irq.h
mv_safe kernel/pic.c        kernel/arch/x86_64/pic.c
mv_safe kernel/pic.h        kernel/arch/x86_64/pic.h
mv_safe kernel/isr_stubs.s  kernel/arch/x86_64/isr_stubs.s
mv_safe kernel/syscall.c    kernel/arch/x86_64/syscall.c
mv_safe kernel/syscall.h    kernel/arch/x86_64/syscall.h
mv_safe kernel/ring3.c      kernel/arch/x86_64/ring3.c
mv_safe kernel/ring3.h      kernel/arch/x86_64/ring3.h
mv_safe kernel/rtc.c        kernel/arch/x86_64/rtc.c
mv_safe kernel/rtc.h        kernel/arch/x86_64/rtc.h
mv_safe kernel/boot.s       kernel/arch/x86_64/boot.s
mv_safe kernel/gnu_stack.s  kernel/arch/x86_64/gnu_stack.s

# ─── kernel/mm ─────────────────────────────────────────────────────────────
echo "[kernel/mm]"
mv_safe kernel/pmm.c     kernel/mm/pmm.c
mv_safe kernel/pmm.h     kernel/mm/pmm.h
mv_safe kernel/vmm.c     kernel/mm/vmm.c
mv_safe kernel/vmm.h     kernel/mm/vmm.h
mv_safe kernel/paging.c  kernel/mm/paging.c
mv_safe kernel/paging.h  kernel/mm/paging.h

# ─── kernel/fs ─────────────────────────────────────────────────────────────
echo "[kernel/fs]"
mv_safe kernel/vfs.c    kernel/fs/vfs.c
mv_safe kernel/vfs.h    kernel/fs/vfs.h
mv_safe kernel/ramfs.c  kernel/fs/ramfs.c
mv_safe kernel/ramfs.h  kernel/fs/ramfs.h
mv_safe kernel/procfs.c kernel/fs/procfs.c
mv_safe kernel/procfs.h kernel/fs/procfs.h

# ─── kernel/sched ──────────────────────────────────────────────────────────
echo "[kernel/sched]"
mv_safe kernel/sched.c  kernel/sched/sched.c
mv_safe kernel/sched.h  kernel/sched/sched.h
mv_safe kernel/task.h   kernel/sched/task.h

# ─── kernel/ipc ────────────────────────────────────────────────────────────
echo "[kernel/ipc]"
mv_safe kernel/signal.c kernel/ipc/signal.c
mv_safe kernel/signal.h kernel/ipc/signal.h

# ─── kernel/drivers/ata ────────────────────────────────────────────────────
echo "[kernel/drivers/ata]"
mv_safe drivers/storage/ata_pio.c kernel/drivers/ata/ata_pio.c
mv_safe drivers/storage/ata_pio.h kernel/drivers/ata/ata_pio.h

# ─── kernel/drivers/ps2 ────────────────────────────────────────────────────
echo "[kernel/drivers/ps2]"
mv_safe kernel/keyboard.c     kernel/drivers/ps2/keyboard.c
mv_safe kernel/keyboard.h     kernel/drivers/ps2/keyboard.h
mv_safe kernel/mouse.c        kernel/drivers/ps2/mouse.c
mv_safe kernel/mouse.h        kernel/drivers/ps2/mouse.h
mv_safe kernel/vmmouse.c      kernel/drivers/ps2/vmmouse.c
mv_safe kernel/vmmouse.h      kernel/drivers/ps2/vmmouse.h
mv_safe kernel/input_router.c kernel/drivers/ps2/input_router.c
mv_safe kernel/input_router.h kernel/drivers/ps2/input_router.h
mv_safe drivers/input/input_driver.c kernel/drivers/ps2/input_driver.c
mv_safe drivers/input/input_driver.h kernel/drivers/ps2/input_driver.h
mv_safe drivers/input/touchpad.c     kernel/drivers/ps2/touchpad.c
mv_safe drivers/input/README.md      kernel/drivers/ps2/README.md

# ─── kernel/drivers/serial ─────────────────────────────────────────────────
echo "[kernel/drivers/serial]"
mv_safe kernel/serial.c kernel/drivers/serial/serial.c
mv_safe kernel/serial.h kernel/drivers/serial/serial.h

# ─── kernel/drivers/vga ────────────────────────────────────────────────────
echo "[kernel/drivers/vga]"
mv_safe kernel/vga.c       kernel/drivers/vga/vga.c
mv_safe kernel/vga.h       kernel/drivers/vga/vga.h
mv_safe kernel/fb.c        kernel/drivers/vga/fb.c
mv_safe kernel/fb.h        kernel/drivers/vga/fb.h
mv_safe kernel/cursor.c    kernel/drivers/vga/cursor.c
mv_safe kernel/cursor.h    kernel/drivers/vga/cursor.h
mv_safe kernel/cursor_data.h kernel/drivers/vga/cursor_data.h
mv_safe kernel/opengl.c    kernel/drivers/vga/opengl.c
mv_safe kernel/opengl.h    kernel/drivers/vga/opengl.h
mv_safe drivers/graphics/gfx_driver.c kernel/drivers/vga/gfx_driver.c
mv_safe drivers/graphics/gfx_driver.h kernel/drivers/vga/gfx_driver.h
mv_safe drivers/video/README.md        kernel/drivers/vga/README.md

# ─── kernel/drivers/audio ──────────────────────────────────────────────────
echo "[kernel/drivers/audio]"
mv_safe drivers/audio/audio_driver.c kernel/drivers/audio/audio_driver.c
mv_safe drivers/audio/audio_driver.h kernel/drivers/audio/audio_driver.h

# ─── kernel/drivers/net ────────────────────────────────────────────────────
echo "[kernel/drivers/net]"
mv_safe drivers/network/net_driver.c kernel/drivers/net/net_driver.c
mv_safe drivers/network/net_driver.h kernel/drivers/net/net_driver.h

# ─── kernel/drivers/usb ────────────────────────────────────────────────────
echo "[kernel/drivers/usb]"
mv_safe drivers/usb/usb_stub.c kernel/drivers/usb/usb_stub.c

# ─── kernel/security (draco-shield) ────────────────────────────────────────
echo "[kernel/security/draco-shield]"
mv_safe draco-shield/firewall.c        kernel/security/draco-shield/firewall.c
mv_safe draco-shield/firewall.h        kernel/security/draco-shield/firewall.h
mv_safe draco-shield/draco-shieldctl.c kernel/security/draco-shield/draco-shieldctl.c
mv_safe draco-shield/draco-shieldctl.h kernel/security/draco-shield/draco-shieldctl.h

# ─── apps/ ─────────────────────────────────────────────────────────────────
echo "[apps/]"
mv_safe gui/apps/appman.c                      apps/appman/appman.c
mv_safe gui/apps/appman.h                      apps/appman/appman.h
mv_safe gui/apps/apps.c                        apps/appman/apps.c
mv_safe gui/apps/debug_console.c               apps/debug_console/debug_console.c
mv_safe gui/apps/debug_console.h               apps/debug_console/debug_console.h
mv_safe gui/apps/disk_manager.c                apps/disk_manager/disk_manager.c
mv_safe gui/apps/disk_manager.h                apps/disk_manager/disk_manager.h
mv_safe gui/apps/trash_manager.c               apps/trash_manager/trash_manager.c
mv_safe gui/apps/trash_manager.h               apps/trash_manager/trash_manager.h
mv_safe gui/apps/file_manager/file_manager.c   apps/filemanager/file_manager.c
mv_safe gui/apps/file_manager/file_manager.h   apps/filemanager/file_manager.h
mv_safe gui/apps/installer/installer.c         apps/installer/installer.c
mv_safe gui/apps/installer/installer.h         apps/installer/installer.h

# userland apps -> apps/
mv_safe userland/apps/calculator/draco.json    apps/calculator/draco.json
mv_safe userland/apps/hello-world/draco.json   apps/hello-world/draco.json
mv_safe userland/apps/DEVELOPING.md            apps/DEVELOPING.md

# ─── drx/ ──────────────────────────────────────────────────────────────────
echo "[drx/]"
mv_safe userland/tools/draco-install/draco-install.c       drx/cli/draco-install.c
mv_safe userland/tools/draco-install/draco-install.h       drx/cli/draco-install.h
mv_safe userland/tools/draco-install/draco-pm-intercept.c  drx/cli/draco-pm-intercept.c
mv_safe userland/tools/draco-install/draco-deb-to-dracopkg.sh drx/cli/draco-deb-to-dracopkg.sh

# Move the draco-updates repo into drx/
mv_safe updates/draco-updates drx/draco-updates

# ─── tools/ ────────────────────────────────────────────────────────────────
echo "[tools/]"
mv_safe img-to-dxi.py tools/dxi-convert/img-to-dxi.py

# lxs examples -> tools/lxs
mv_safe userland/tools/lxs/hello.lxs     tools/lxs/hello.lxs
mv_safe userland/tools/lxs/os_bindings.lxs tools/lxs/os_bindings.lxs
mv_safe userland/tools/lxs/sysinfo.lxs  tools/lxs/sysinfo.lxs

# ─── build/ ────────────────────────────────────────────────────────────────
echo "[build/]"
mv_safe kernel/linker.ld build/linker.ld
mv_safe iso              build/iso
mv_safe CMakeLists.txt   build/CMakeLists.txt

# ci -> build/ci
mv_safe ci build/ci

# ─── docs/ ─────────────────────────────────────────────────────────────────
echo "[docs/]"
# existing docs stay in docs/, STRUCTURE.md and README.md will be written by the script

# ─── libc/ stub ────────────────────────────────────────────────────────────
echo "[libc/]"
mkdir -p libc

# ─── runtimes/wine stub ────────────────────────────────────────────────────
echo "[runtimes/wine]"
mkdir -p runtimes/wine

# ─── Clean up empty old directories ────────────────────────────────────────
echo "[cleanup]"
for d in draco-shield drivers/graphics drivers/input drivers/audio drivers/network drivers/storage drivers/usb drivers updates userland gui/apps; do
    [ -d "$d" ] && rmdir --ignore-fail-on-non-empty -p "$d" 2>/dev/null || true
done

echo ""
echo "=== Restructure complete ==="
echo "Next: run 'make' from root to verify the build."
