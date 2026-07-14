# DracolaxOS CMakeLists.txt — v1.0
# Usage:
#   mkdir build && cd build
#   cmake ..
#   cmake --build .          # builds kernel.elf
#   cmake --build . -t iso   # builds DracolaxOS_v2_x64.iso
#   cmake --build . -t run-qemu
#   cmake --build . -t run-debug
#   cmake --build . -t run-vbox
#   cmake --build . -t tests
cmake_minimum_required(VERSION 3.16)
project(DracolaxOS C ASM_NASM)

# ── Toolchain detection ─────────────────────────────────────────
if(DEFINED ENV{CROSS_COMPILE})
    set(CC "$ENV{CROSS_COMPILE}gcc")
    set(LD_BIN "$ENV{CROSS_COMPILE}ld")
else()
    find_program(CC_CROSS x86_64-elf-gcc)
    if(CC_CROSS)
        set(CC "${CC_CROSS}")
        find_program(LD_BIN x86_64-elf-ld REQUIRED)
        set(CROSS 1)
    else()
        set(CC "gcc")
        set(LD_BIN "gcc")
        set(CROSS 0)
    endif()
endif()

set(CMAKE_C_COMPILER   ${CC})
set(CMAKE_ASM_COMPILER nasm)
set(CMAKE_C_STANDARD   11)

# ── Compiler flags ──────────────────────────────────────────────
set(CFLAGS
    -ffreestanding -O2 -Wall -Wextra
    -fno-stack-protector -fno-pie -fno-builtin
    -mcmodel=large -mno-red-zone -mno-sse -mno-mmx
    -Ikernel -Ikernel/linux/include-uapi
    -Ikernel/security -Igui/apps -Iservices -Idrivers
    -nostdinc
)

set(ASMFLAGS -f elf64)

if(CROSS EQUAL 1)
    set(LDFLAGS -T kernel/linker.ld -nostdlib
        -z max-page-size=0x1000 -z noexecstack)
else()
    set(LDFLAGS -T kernel/linker.ld -nostdlib -no-pie
        -z max-page-size=0x1000 -z noexecstack)
endif()

# ── Source lists ────────────────────────────────────────────────
set(C_SOURCES
    kernel/main.c
    kernel/vga.c
    kernel/serial.c
    kernel/log.c
    kernel/klibc.c
    kernel/gdt.c
    kernel/tss.c
    kernel/ring3.c
    kernel/idt.c
    kernel/irq.c
    kernel/pic.c
    kernel/paging.c
    kernel/pmm.c
    kernel/vmm.c
    kernel/sched.c
    kernel/vfs.c
    kernel/ramfs.c
    kernel/keyboard.c
    kernel/input_router.c
    kernel/signal.c
    kernel/mouse.c
    kernel/vmmouse.c
    kernel/syscall.c
    kernel/init.c
    kernel/shell.c
    kernel/procfs.c
    kernel/fb.c
    kernel/cursor.c
    kernel/limits.c
    kernel/klog.c
    kernel/bootmode.c
    kernel/atlas.c
    kernel/loader/elf_loader.c
    kernel/linux/linux_compat.c
    kernel/linux/linux_syscall_table.c
    kernel/linux/linux_syscalls.c
    kernel/linux/linux_fs.c
    kernel/linux/linux_process.c
    kernel/linux/linux_memory.c
    kernel/security/dracoauth.c
    kernel/security/dracolock.c
    kernel/security/dracolicence.c
    draco-shield/firewall.c
    draco-shield/draco-shieldctl.c
    drivers/graphics/gfx_driver.c
    drivers/input/input_driver.c
    drivers/input/touchpad.c
    drivers/audio/audio_driver.c
    drivers/usb/usb_stub.c
    drivers/storage/ata_pio.c
    drivers/network/net_driver.c
    services/service_manager.c
    services/network_manager.c
    services/audio_service.c
    services/power_manager.c
    services/notification_daemon.c
    services/session_manager.c
    services/login_manager.c
    gui/compositor/compositor.c
    gui/desktop/default-desktop/desktop.c
    gui/apps/appman.c
    gui/apps/apps.c
    gui/apps/installer/installer.c
    gui/apps/debug_console.c
    gui/apps/file_manager/file_manager.c
    gui/apps/disk_manager.c
    gui/apps/trash_manager.c
    kernel/rtc.c
    userland/tools/draco-install/draco-install.c
)

set(ASM_SOURCES
    kernel/boot.s
    kernel/isr_stubs.s
    kernel/gnu_stack.s
)

# ── Build objects ────────────────────────────────────────────────
set(KERNEL_ELF "${CMAKE_BINARY_DIR}/kernel.elf")
set(ISO_DIR    "${CMAKE_BINARY_DIR}/iso_build")
set(ISO        "${CMAKE_BINARY_DIR}/DracolaxOS_v2_x64.iso")

# Compile C sources
set(ALL_OBJS "")
foreach(SRC ${C_SOURCES})
    get_filename_component(OBJ_NAME ${SRC} NAME_WE)
    get_filename_component(OBJ_DIR  ${SRC} DIRECTORY)
    set(OBJ "${CMAKE_BINARY_DIR}/obj/${OBJ_DIR}/${OBJ_NAME}.o")
    get_filename_component(OBJ_PARENT ${OBJ} DIRECTORY)
    file(MAKE_DIRECTORY ${OBJ_PARENT})
    add_custom_command(
        OUTPUT  ${OBJ}
        COMMAND ${CC} ${CFLAGS} -c ${CMAKE_SOURCE_DIR}/${SRC} -o ${OBJ}
        DEPENDS ${CMAKE_SOURCE_DIR}/${SRC}
        COMMENT "CC  ${SRC}"
    )
    list(APPEND ALL_OBJS ${OBJ})
endforeach()

# Assemble ASM sources
foreach(SRC ${ASM_SOURCES})
    get_filename_component(OBJ_NAME ${SRC} NAME_WE)
    get_filename_component(OBJ_DIR  ${SRC} DIRECTORY)
    set(OBJ "${CMAKE_BINARY_DIR}/obj/${OBJ_DIR}/${OBJ_NAME}.o")
    get_filename_component(OBJ_PARENT ${OBJ} DIRECTORY)
    file(MAKE_DIRECTORY ${OBJ_PARENT})
    add_custom_command(
        OUTPUT  ${OBJ}
        COMMAND nasm ${ASMFLAGS} ${CMAKE_SOURCE_DIR}/${SRC} -o ${OBJ}
        DEPENDS ${CMAKE_SOURCE_DIR}/${SRC}
        COMMENT "AS  ${SRC}"
    )
    list(APPEND ALL_OBJS ${OBJ})
endforeach()

# Link kernel.elf
add_custom_command(
    OUTPUT  ${KERNEL_ELF}
    COMMAND ${LD_BIN} ${LDFLAGS} -o ${KERNEL_ELF} ${ALL_OBJS}
    DEPENDS ${ALL_OBJS}
    COMMENT "LD  kernel.elf"
)
add_custom_target(kernel ALL DEPENDS ${KERNEL_ELF})

# ── ISO ──────────────────────────────────────────────────────────
add_custom_target(iso
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ISO_DIR}/boot/grub
    COMMAND ${CMAKE_COMMAND} -E copy ${KERNEL_ELF} ${ISO_DIR}/boot/kernel.elf
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_SOURCE_DIR}/iso/boot/grub/grub.cfg
            ${ISO_DIR}/boot/grub/grub.cfg
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/iso/boot/themes ${ISO_DIR}/boot/themes
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/iso/boot/grub/fonts ${ISO_DIR}/boot/grub/fonts
    COMMAND grub-mkrescue -o ${ISO} ${ISO_DIR}
    DEPENDS kernel
    COMMENT "Building ISO: ${ISO}"
)

# ── Run targets ──────────────────────────────────────────────────
add_custom_target(run-qemu
    COMMAND qemu-system-x86_64
        -cdrom ${ISO}
        -m 512M
        -vga std
        -serial stdio
        -cpu qemu64,+lahf_lm
        -device usb-ehci,id=ehci
        -device usb-tablet
        -device isa-debug-exit,iobase=0x604,iosize=0x04
    DEPENDS iso
    COMMENT "QEMU: booting DracolaxOS"
)

add_custom_target(run-debug
    COMMAND qemu-system-x86_64
        -cdrom ${ISO}
        -m 512M
        -vga std
        -serial stdio
        -cpu qemu64,+lahf_lm
        -no-reboot -no-shutdown
        -s -S
    DEPENDS iso
    COMMENT "QEMU debug: attach GDB at localhost:1234"
)

add_custom_target(run-headless
    COMMAND qemu-system-x86_64
        -cdrom ${ISO}
        -m 512M
        -nographic
        -serial mon:stdio
        -cpu qemu64,+lahf_lm
    DEPENDS iso
    COMMENT "QEMU headless"
)

set(VBOX_VM "DracolaxOS_v2")
add_custom_target(run-vbox
    COMMAND bash -c
        "VBoxManage showvminfo ${VBOX_VM} >/dev/null 2>&1 || (\
          VBoxManage createvm --name ${VBOX_VM} --ostype Other_64 --register && \
          VBoxManage modifyvm ${VBOX_VM} --memory 512 --vram 16 \
            --cpus 1 --cpu-profile host --acpi on --ioapic on \
            --boot1 dvd --boot2 none && \
          VBoxManage storagectl ${VBOX_VM} --name SATA --add sata && \
          VBoxManage storageattach ${VBOX_VM} --storagectl SATA \
            --port 0 --device 0 --type dvddrive --medium ${ISO})"
    COMMAND VBoxManage startvm ${VBOX_VM} --type gui
    DEPENDS iso
    COMMENT "VirtualBox: booting DracolaxOS"
)

# ── Tests ────────────────────────────────────────────────────────
add_custom_target(tests
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run_all_tests.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running test suite"
)

# ── Install deps ─────────────────────────────────────────────────
add_custom_target(install-deps
    COMMAND sudo apt-get install -y
        build-essential nasm grub-pc-bin grub-common
        xorriso mtools qemu-system-x86
        gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu
        cmake
    COMMENT "Installing build dependencies"
)
