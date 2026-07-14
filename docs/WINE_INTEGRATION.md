# Wine Integration — DRX Component Design

**Status:** Planned — Phase 4  
**Package ID:** `wine`  
**Install path:** `/storage/main/system/runtimes/wine/`

---

## Goal

Allow DracolaxOS to run unmodified Windows PE32+ (x86_64) binaries by shipping Wine as a first-class DRX package. Wine is not compiled into the kernel ISO — it is downloaded and installed via `drx install wine`.

---

## Architecture

```
User launches .exe
        │
        ▼
kernel/loader/elf_loader.c
   Detects PE32+ magic (0x4D5A / MZ header)
        │
        ├─ Wine installed? Check manifest.json for "wine" entry
        │       └─ Not installed → show "Install Wine via drx?" prompt
        │
        └─ Invoke Wine loader
               /storage/main/system/runtimes/wine/bin/wine64
               with the user's binary as argv[1]
                       │
                       ▼
               Wine translates Win32 API calls
               → DracolaxOS native syscalls (via kernel/linux/ compat shim)
               → kernel/arch/x86_64/syscall.c
```

---

## ELF Loader Changes (kernel/loader/elf_loader.c)

The loader already handles native ELF64 binaries. PE detection is added as a pre-check:

```c
/* In elf_loader_exec(): */
if (buf[0] == 0x4D && buf[1] == 0x5A) {   /* MZ magic = Windows PE */
    return wine_exec(path, argc, argv);     /* hand off to Wine shim */
}
```

`wine_exec()` constructs the argument vector `[wine64_path, binary_path, …argv]` and spawns it via `sched_spawn()`.

---

## Linux Compat Shim Role (kernel/linux/)

Wine relies on a large subset of Linux syscalls. The existing `kernel/linux/linux_syscalls.c` table covers the most common ones. Wine-specific additions required:

| Syscall | Notes |
|---------|-------|
| `mmap` / `munmap` | Memory-mapped file I/O for PE sections |
| `clone` | Thread creation (Wine uses pthreads internally) |
| `futex` | Thread synchronisation |
| `openat` / `readv` / `writev` | File I/O |
| `ioctl` (partial) | Graphics (via Mesa/software rasteriser) |
| `prctl` | Process name, signal mask |

These are stubbed in `linux_syscalls.c`; full implementations are added during Phase 4.

---

## DRX Package Contents

The `wine` DRX package unpacks to:

```
/storage/main/system/runtimes/wine/
├── bin/
│   ├── wine64             — Wine64 main binary
│   └── wineserver         — Wine server process
├── lib/
│   └── wine/              — Wine PE loader libraries
├── prefix/                — Default Wine prefix (C: drive emulation)
│   ├── drive_c/
│   └── system.reg
└── wine.manifest          — DRX sub-manifest for this package
```

---

## Graphics

Wine uses a software OpenGL rasteriser (llvmpipe or softpipe) through the Mesa stack. DracolaxOS exposes a minimal OpenGL 3.3 surface via `kernel/drivers/vga/opengl.c` (currently a stub). Phase 4 will implement enough of the OpenGL stub to satisfy Wine's D3D translation layer (DXVK or WineD3D software path).

Hardware GPU acceleration is not planned for v1.

---

## File System Mapping

Wine expects a POSIX-like VFS. The DracolaxOS VFS (`kernel/fs/vfs.c`) provides this. Wine's `drive_c` maps to a directory under the RAMFS:

```
Wine path  C:\Users\Public\Desktop
           ↕ Wine prefix mapping
VFS path   /storage/main/system/runtimes/wine/prefix/drive_c/Users/Public/Desktop
```

User documents and downloads map to:

```
C:\Users\dracolax\Documents → /storage/main/users/dracolax/Documents
C:\Users\dracolax\Downloads → /storage/main/users/dracolax/Downloads
```

---

## Installation

```bash
drx install wine
```

This downloads the Wine DRX package (~80 MB compressed), verifies SHA-256, and unpacks to `/storage/main/system/runtimes/wine/`. The kernel is not modified or rebooted. Wine is immediately available after install.

To test:
```bash
wine notepad.exe
```

---

## Limitations (v1)

- No audio support in Wine (audio driver stub not yet wired to Wine's audio backend)
- No clipboard integration
- No Direct3D hardware acceleration (software rasteriser only)
- 64-bit PE binaries only (no WOW64 for 32-bit .exe)
- No installer UI integration (manual `drx install` only in Phase 4)
