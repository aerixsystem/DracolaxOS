# DracolaxOS — Source Tree Structure

> Phase 0 canonical layout. Every directory has a fixed purpose.
> The DRX update engine uses these paths to locate components at runtime.

---

## Source Tree

```
dracolaxos/
├── kernel/                  Core kernel — Ring 0 only
│   ├── main.c               Entry point, subsystem init sequence
│   ├── init.c               Late init (launches services + GUI)
│   ├── shell.c              Kernel debug shell (Ring 0 REPL)
│   ├── log.c / klog.c       Kernel logging subsystem
│   ├── klibc.c              Minimal libc replacement (no stdlib)
│   ├── limits.c             Compile-time and runtime limits
│   ├── bootmode.c           Boot mode detection (normal/recovery)
│   ├── atlas.c              Sprite atlas loader
│   ├── lxs_kernel.c         LXScript kernel binding layer
│   │
│   ├── arch/x86_64/         Architecture-specific: GDT, IDT, TSS,
│   │   ├── boot.s           syscall/sysret, Ring-3 entry, RTC, PIC
│   │   ├── gdt.c / gdt.h
│   │   ├── idt.c / idt.h
│   │   ├── tss.c / tss.h
│   │   ├── irq.c / irq.h
│   │   ├── pic.c / pic.h
│   │   ├── isr_stubs.s
│   │   ├── syscall.c / syscall.h
│   │   ├── ring3.c / ring3.h
│   │   ├── rtc.c / rtc.h
│   │   └── gnu_stack.s
│   │
│   ├── mm/                  Memory management: physical, virtual, paging
│   │   ├── pmm.c / pmm.h    Physical memory manager
│   │   ├── vmm.c / vmm.h    Virtual memory manager
│   │   └── paging.c / paging.h  Page table setup
│   │
│   ├── fs/                  Filesystem layer
│   │   ├── vfs.c / vfs.h    Virtual filesystem switch
│   │   ├── ramfs.c / ramfs.h  In-memory RAM filesystem
│   │   └── procfs.c / procfs.h  Process info filesystem
│   │
│   ├── sched/               Scheduler and task management
│   │   ├── sched.c / sched.h
│   │   └── task.h           Task control block definition
│   │
│   ├── ipc/                 Inter-process communication
│   │   └── signal.c / signal.h  Signal delivery
│   │
│   ├── drivers/
│   │   ├── ata/             ATA PIO block device driver
│   │   │   └── ata_pio.c / ata_pio.h
│   │   ├── ps2/             PS/2 keyboard, mouse, VMware mouse, touchpad
│   │   │   ├── keyboard.c / keyboard.h
│   │   │   ├── mouse.c / mouse.h
│   │   │   ├── vmmouse.c / vmmouse.h
│   │   │   ├── input_router.c / input_router.h
│   │   │   ├── input_driver.c / input_driver.h
│   │   │   └── touchpad.c
│   │   ├── serial/          Serial port (debug output)
│   │   │   └── serial.c / serial.h
│   │   ├── vga/             Framebuffer, VGA, cursor, OpenGL stub
│   │   │   ├── vga.c / vga.h
│   │   │   ├── fb.c / fb.h
│   │   │   ├── cursor.c / cursor.h / cursor_data.h
│   │   │   ├── opengl.c / opengl.h
│   │   │   └── gfx_driver.c / gfx_driver.h
│   │   ├── audio/           Audio driver stub
│   │   │   └── audio_driver.c / audio_driver.h
│   │   ├── net/             Network driver stub
│   │   │   └── net_driver.c / net_driver.h
│   │   └── usb/             USB stub
│   │       └── usb_stub.c
│   │
│   ├── security/            Auth, licence, lockscreen, firewall
│   │   ├── dracoauth.c / dracoauth.h
│   │   ├── dracolicence.c / dracolicence.h
│   │   ├── dracolock.c / dracolock.h
│   │   └── draco-shield/    Network firewall (DracoShield)
│   │       ├── firewall.c / firewall.h
│   │       └── draco-shieldctl.c / draco-shieldctl.h
│   │
│   ├── linux/               Linux syscall compatibility shim
│   │   ├── include-uapi/    Upstream Linux UAPI headers (read-only)
│   │   ├── linux_compat.c
│   │   ├── linux_syscall_table.c
│   │   ├── linux_syscalls.c
│   │   ├── linux_fs.c / linux_fs.h
│   │   ├── linux_process.c / linux_process.h
│   │   └── linux_memory.c / linux_memory.h
│   │
│   └── loader/              ELF binary loader
│       └── elf_loader.c / elf_loader.h
│
├── gui/                     Graphical subsystem — compositor, WM, desktop
│   ├── compositor/          Double-buffer compositor, alpha blend, blit
│   │   └── compositor.c / compositor.h
│   ├── wm/                  Window manager: z-order, focus, hit-test
│   │   └── wm.c / wm.h
│   ├── desktop/             Desktop shell (wallpaper, dock, icon grid)
│   │   └── default-desktop/
│   │       ├── desktop.c / desktop.h
│   │       ├── background.h
│   │       └── image_to_header.py
│   ├── widgets/             (planned) Buttons, labels, input boxes
│   ├── images/              *.dxi icon files (runtime)
│   └── fonts/               Bitmap fonts (runtime)
│
├── apps/                    Ring-3 userland applications
│   ├── appman/              App registry and launcher
│   │   ├── appman.c / appman.h
│   │   └── apps.c
│   ├── terminal/            (planned)
│   ├── filemanager/
│   │   └── file_manager.c / file_manager.h
│   ├── installer/           Package installer UI
│   │   └── installer.c / installer.h
│   ├── debug_console/       On-screen debug console app
│   │   └── debug_console.c / debug_console.h
│   ├── disk_manager/
│   │   └── disk_manager.c / disk_manager.h
│   ├── trash_manager/
│   │   └── trash_manager.c / trash_manager.h
│   ├── settings/            (planned)
│   ├── calculator/          draco.json manifest
│   └── hello-world/         draco.json manifest
│
├── drx/                     DRX update and package engine
│   ├── core/                (planned) Manifest parser, staging, atomic swap
│   ├── net/                 (planned) HTTP downloader
│   ├── cli/                 Command-line tools
│   │   ├── draco-install.c  Package installer CLI
│   │   ├── draco-pm-intercept.c
│   │   └── draco-deb-to-dracopkg.sh
│   ├── recovery/            (planned) Boot-time rollback checker
│   └── draco-updates/       GitHub-hosted update index (git submodule)
│       ├── index.json        Full package catalogue
│       └── latest.json       Latest stable/beta pointers
│
├── lxscript/                LXScript language runtime (kernel + host)
│   ├── lxscript.c / lxscript.h  Top-level API
│   ├── lexer/
│   ├── parser/
│   ├── codegen/
│   ├── vm/
│   ├── stdlib/
│   ├── examples/            *.lxs example scripts
│   └── tools/               Host-side lxs_cli
│
├── services/                Background system services (Ring 3)
│   ├── service_manager.c    Service supervisor
│   ├── network_manager.c
│   ├── audio_service.c
│   ├── power_manager.c
│   ├── notification_daemon.c
│   ├── session_manager.c
│   └── login_manager.c
│
├── libc/                    Minimal Ring-3 libc (freestanding)
│
├── runtimes/
│   └── wine/                Wine integration layer (DRX component)
│
├── tools/                   Host-side developer utilities
│   ├── dxi-convert/         img-to-dxi.py — converts PNG to .dxi icon format
│   └── lxs/                 Standalone LXScript sample scripts
│
├── build/                   Build system artifacts
│   ├── linker.ld            Kernel linker script
│   ├── iso/                 GRUB config, splash themes, fonts
│   └── ci/                  GitHub Actions workflow
│
├── docs/                    Internal documentation
│   ├── STRUCTURE.md         This file
│   ├── DXI_FORMAT.md        .dxi icon binary format spec
│   ├── DRX_SPEC.md          DRX update engine protocol spec
│   ├── WINE_INTEGRATION.md  Wine DRX component design
│   ├── architecture.md      High-level OS architecture
│   ├── api_reference.md     Kernel API reference
│   ├── developer_notes.md   Dev notes and gotchas
│   └── install_guide.md     Build and run guide
│
├── tests/                   Test suite (host-side runners)
│
├── storage/                 Runtime OS storage tree (NOT source)
│   └── main/system/         Fonts, images, logs, manifests
│
├── Makefile                 Root build entry point
├── restructure.sh           Phase 0 migration script (run once)
├── .gitignore
└── README.md
```

---

## OS Runtime Storage Layout

Separate from the source tree. Lives on the boot medium at runtime.

```
/storage/
├── apps/                    Installed app data
├── network/                 Network state
├── usb/                     USB mount points
├── ramdisk/                 Temporary RAM disk mount
└── main/
    ├── apps/
    ├── cache/
    ├── crash/               Crash dumps
    ├── logs/
    │   ├── kernel/
    │   └── system/
    ├── system/
    │   ├── core/
    │   ├── kernel/
    │   ├── services/
    │   ├── ui/
    │   ├── runtimes/
    │   │   ├── wine/
    │   │   └── linux/
    │   ├── packages/
    │   ├── updates/
    │   ├── backup/
    │   ├── staging/          DRX atomic staging area
    │   ├── shared/           System images, fonts, audio assets
    │   ├── logs/
    │   └── manifest.json     Installed package manifest
    ├── temp/
    └── users/
```
