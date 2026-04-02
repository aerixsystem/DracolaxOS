# DracolaxOS Makefile v3.0 — x86_64
# Targets: build iso run-qemu run-debug run-headless run-vbox clean install-deps tests
# Project restructured per Phase 0 canonical layout.
# -------------------------------------------------------------------------
ifneq (, $(shell which x86_64-elf-gcc 2>/dev/null))
    CC    := x86_64-elf-gcc
    LD    := x86_64-elf-ld
    CROSS := 1
else
    CC    := gcc
    LD    := gcc
    CROSS := 0
endif
AS   := nasm
GRUB := grub-mkrescue

CFLAGS := -ffreestanding -std=c11 -O2 -Wall -Wextra \
          -fno-stack-protector -fno-pie -fno-builtin \
          -mcmodel=large -mno-red-zone -mno-sse -mno-mmx \
          -nostdinc \
          -Ikernel \
          -Ikernel/arch/x86_64 \
          -Ikernel/mm \
          -Ikernel/fs \
          -Ikernel/sched \
          -Ikernel/ipc \
          -Ikernel/drivers/ata \
          -Ikernel/drivers/ps2 \
          -Ikernel/drivers/serial \
          -Ikernel/drivers/vga \
          -Ikernel/drivers/audio \
          -Ikernel/drivers/net \
          -Ikernel/drivers/usb \
          -Ikernel/security \
          -Ikernel/security/draco-shield \
          -Ikernel/linux/include-uapi \
          -Ilxscript \
          -Iservices \
          -Igui/compositor \
          -Igui/wm \
          -Igui/desktop/default-desktop \
          -Iapps/appman \
          -Iapps/debug_console \
          -Iapps/disk_manager \
          -Iapps/filemanager \
          -Iapps/installer \
          -Iapps/trash_manager

ifeq ($(CROSS),1)
    LDFLAGS := -T build/linker.ld -nostdlib -z max-page-size=0x1000 -z noexecstack
else
    LDFLAGS := -T build/linker.ld -nostdlib -no-pie \
               -z max-page-size=0x1000 -z noexecstack
endif

ASMFLAGS := -f elf64

# -------------------------------------------------------------------------
# Sources
# -------------------------------------------------------------------------

# kernel core
KERNEL_CORE := \
    kernel/main.c \
    kernel/init.c \
    kernel/shell.c \
    kernel/log.c \
    kernel/klibc.c \
    kernel/klog.c \
    kernel/limits.c \
    kernel/bootmode.c \
    kernel/atlas.c \
    kernel/lxs_kernel.c

# kernel/arch/x86_64
KERNEL_ARCH := \
    kernel/arch/x86_64/gdt.c \
    kernel/arch/x86_64/tss.c \
    kernel/arch/x86_64/idt.c \
    kernel/arch/x86_64/irq.c \
    kernel/arch/x86_64/pic.c \
    kernel/arch/x86_64/syscall.c \
    kernel/arch/x86_64/ring3.c \
    kernel/arch/x86_64/rtc.c

# kernel/mm
KERNEL_MM := \
    kernel/mm/paging.c \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c

# kernel/fs
KERNEL_FS := \
    kernel/fs/vfs.c \
    kernel/fs/ramfs.c \
    kernel/fs/procfs.c

# kernel/sched
KERNEL_SCHED := \
    kernel/sched/sched.c

# kernel/ipc
KERNEL_IPC := \
    kernel/ipc/signal.c

# kernel/drivers
KERNEL_DRV := \
    kernel/drivers/ata/ata_pio.c \
    kernel/drivers/ps2/keyboard.c \
    kernel/drivers/ps2/mouse.c \
    kernel/drivers/ps2/vmmouse.c \
    kernel/drivers/ps2/input_router.c \
    kernel/drivers/ps2/input_driver.c \
    kernel/drivers/ps2/touchpad.c \
    kernel/drivers/serial/serial.c \
    kernel/drivers/vga/vga.c \
    kernel/drivers/vga/fb.c \
    kernel/drivers/vga/cursor.c \
    kernel/drivers/vga/opengl.c \
    kernel/drivers/vga/gfx_driver.c \
    kernel/drivers/audio/audio_driver.c \
    kernel/drivers/net/net_driver.c \
    kernel/drivers/usb/usb_stub.c

# kernel/security
KERNEL_SEC := \
    kernel/security/dracoauth.c \
    kernel/security/dracolock.c \
    kernel/security/dracolicence.c \
    kernel/security/draco-shield/firewall.c \
    kernel/security/draco-shield/draco-shieldctl.c

# kernel/linux compat
KERNEL_LINUX := \
    kernel/linux/linux_compat.c \
    kernel/linux/linux_syscall_table.c \
    kernel/linux/linux_syscalls.c \
    kernel/linux/linux_fs.c \
    kernel/linux/linux_process.c \
    kernel/linux/linux_memory.c

# kernel/loader
KERNEL_LOADER := \
    kernel/loader/elf_loader.c

# lxscript
LXSCRIPT := \
    lxscript/lexer/lexer.c \
    lxscript/parser/parser.c \
    lxscript/codegen/codegen.c \
    lxscript/vm/vm.c

# services
SERVICES := \
    services/service_manager.c \
    services/network_manager.c \
    services/audio_service.c \
    services/power_manager.c \
    services/notification_daemon.c \
    services/session_manager.c \
    services/login_manager.c

# gui
GUI := \
    gui/compositor/compositor.c \
    gui/wm/wm.c \
    gui/desktop/default-desktop/desktop.c

# apps
APPS := \
    apps/appman/appman.c \
    apps/appman/apps.c \
    apps/installer/installer.c \
    apps/debug_console/debug_console.c \
    apps/filemanager/file_manager.c \
    apps/disk_manager/disk_manager.c \
    apps/trash_manager/trash_manager.c

# drx cli
DRX := \
    drx/cli/draco-install.c

C_SOURCES := \
    $(KERNEL_CORE) \
    $(KERNEL_ARCH) \
    $(KERNEL_MM) \
    $(KERNEL_FS) \
    $(KERNEL_SCHED) \
    $(KERNEL_IPC) \
    $(KERNEL_DRV) \
    $(KERNEL_SEC) \
    $(KERNEL_LINUX) \
    $(KERNEL_LOADER) \
    $(LXSCRIPT) \
    $(SERVICES) \
    $(GUI) \
    $(APPS) \
    $(DRX)

ASM_SOURCES := \
    kernel/arch/x86_64/boot.s \
    kernel/arch/x86_64/isr_stubs.s \
    kernel/arch/x86_64/gnu_stack.s

OBJS := $(C_SOURCES:.c=.o) $(ASM_SOURCES:.s=.o)

KERNEL_ELF := kernel.elf
ISO_DIR    := iso_build
ISO        := DracolaxOS_v1_x64.iso

# -------------------------------------------------------------------------
.PHONY: all build iso run-qemu run-debug run-headless run-vbox clean install-deps tests

all: build iso

build: $(KERNEL_ELF)

$(KERNEL_ELF): $(OBJS)
	@echo "  LD  $@"
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.c
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	@echo "  AS  $<"
	$(AS) $(ASMFLAGS) $< -o $@

iso: build
	@echo "  ISO building..."
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	cp build/iso/boot/grub/grub.cfg $(ISO_DIR)/boot/grub/
	cp -r build/iso/boot/themes $(ISO_DIR)/boot/ 2>/dev/null || true
	cp -r build/iso/boot/grub/fonts $(ISO_DIR)/boot/grub/ 2>/dev/null || true
	$(GRUB) -o $(ISO) $(ISO_DIR)
	@echo "  ISO created: $(ISO)"

run-qemu: iso
	qemu-system-x86_64 \
	  -cdrom $(ISO) \
	  -m 512M \
	  -vga std \
	  -serial stdio \
	  -cpu qemu64,+lahf_lm \
	  -device usb-ehci,id=ehci \
	  -device usb-tablet \
	  -device isa-debug-exit,iobase=0x604,iosize=0x04

run-debug: iso
	qemu-system-x86_64 \
	  -cdrom $(ISO) \
	  -m 512M \
	  -vga std \
	  -serial stdio \
	  -cpu qemu64,+lahf_lm \
	  -no-reboot \
	  -no-shutdown \
	  -s -S &
	@echo "QEMU paused — attach GDB: target remote :1234"

run-headless: iso
	qemu-system-x86_64 \
	  -cdrom $(ISO) \
	  -m 512M \
	  -nographic \
	  -serial mon:stdio \
	  -cpu qemu64,+lahf_lm

VBOX_VM := DracolaxOS_v1
run-vbox: iso
	@VBoxManage showvminfo $(VBOX_VM) >/dev/null 2>&1 || ( \
	  VBoxManage createvm --name $(VBOX_VM) --ostype Other_64 --register && \
	  VBoxManage modifyvm $(VBOX_VM) \
	    --memory 512 --vram 16 \
	    --cpus 1 --cpu-profile host \
	    --acpi on --ioapic on \
	    --boot1 dvd --boot2 none && \
	  VBoxManage storagectl $(VBOX_VM) --name SATA --add sata && \
	  VBoxManage storageattach $(VBOX_VM) --storagectl SATA \
	    --port 0 --device 0 --type dvddrive --medium $(ISO) \
	)
	VBoxManage startvm $(VBOX_VM) --type gui

tests:
	@bash tests/run_all_tests.sh

clean:
	rm -f $(OBJS) $(KERNEL_ELF) $(ISO)
	rm -rf $(ISO_DIR)

install-deps:
	sudo apt-get install -y \
	  build-essential nasm grub-pc-bin grub-common \
	  xorriso mtools qemu-system-x86 \
	  gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu
