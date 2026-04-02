# DracolaxOS App Development Guide

## Overview

Apps for DracolaxOS live in `userland/apps/<app-name>/` and are distributed as `.dracopkg` files. Each app must include a `draco.json` manifest.

## Directory Structure

```
userland/apps/<app-name>/
  draco.json          ← required manifest
  src/                ← C source files
    main.c
  Makefile or CMakeLists.txt
  README.md           ← optional
```

## draco.json — App Manifest

Every app **must** have a `draco.json` at its root. The package manager reads this file to install, update, and remove apps.

```json
{
  "name": "my-app",
  "display_name": "My App",
  "version": "1.0.0",
  "description": "What the app does.",
  "author": "YourName",
  "license": "MIT",
  "entry": "my-app",
  "type": "cli",
  "requirements": {
    "os_version": ">=2.0",
    "memory_kb": 256,
    "syscalls": ["read", "write", "exit", "open", "close"]
  },
  "install_path": "/storage/apps/my-app",
  "tags": ["tool", "example"],
  "node_id": "05",
  "storage": {
    "data":    "/storage/apps/my-app/data",
    "configs": "/storage/apps/my-app/configs",
    "cache":   "/storage/apps/my-app/cache"
  }
}
```

### Fields

| Field | Required | Description |
|-------|----------|-------------|
| `name` | ✓ | Unique lowercase identifier. Used by `draco install`. |
| `display_name` | ✓ | Human-readable name shown in the app manager. |
| `version` | ✓ | Semantic version (`MAJOR.MINOR.PATCH`). |
| `description` | ✓ | Short description (shown in `draco list`). |
| `author` | ✓ | Your name or handle. |
| `license` | — | SPDX license identifier (e.g. `"MIT"`, `"GPL-2.0"`). |
| `entry` | ✓ | ELF binary name (no extension) produced by the build. |
| `type` | ✓ | `"cli"` (shell app) or `"gui"` (windowed app). |
| `requirements.os_version` | ✓ | Minimum DracolaxOS version required. |
| `requirements.memory_kb` | — | Expected peak memory in KB. |
| `requirements.syscalls` | — | Linux-compat syscalls the app uses. |
| `install_path` | ✓ | Where the binary is placed at install time. |
| `tags` | — | Array of search tags for `draco search`. |
| `node_id` | — | Storage node (`"05"` = global apps node). |
| `storage.data` | — | Persistent data directory (created at install). |
| `storage.configs` | — | Per-app config directory. |
| `storage.cache` | — | Cache directory (may be cleared by the OS). |

## Writing an App

DracolaxOS supports the Linux x86_64 ABI. You can compile standard C programs with a cross-compiler targeting `x86_64-linux-gnu` and run them via the `exec` shell command.

### Minimal Example

```c
/* src/main.c */
#include <unistd.h>
#include <string.h>

int main(void) {
    const char *msg = "Hello from DracolaxOS!\n";
    write(1, msg, strlen(msg));
    return 0;
}
```

### Building

```makefile
# Makefile
CC = x86_64-linux-gnu-gcc
CFLAGS = -static -O2 -Wall

my-app: src/main.c
	$(CC) $(CFLAGS) -o my-app src/main.c
```

```bash
make
# Produces: my-app (ELF64 static binary)
```

### Installing

```bash
# On DracolaxOS shell:
draco install ./my-app.dracopkg

# Or copy manually:
exec /path/to/my-app
```

## Storage Access

Apps should read/write data through their declared storage paths:

```c
/* Good: use declared storage paths from draco.json */
int fd = open("/storage/apps/my-app/data/save.dat", O_RDWR | O_CREAT, 0644);

/* Avoid: writing to arbitrary paths */
int fd = open("/tmp/save.dat", O_RDWR);  /* /tmp is ramdisk, lost on reboot */
```

Storage paths are created automatically by `draco install`. The OS cleans up `cache/` on system temp purge; `data/` and `configs/` are preserved.

## Packaging

Convert your app to `.dracopkg` (a wrapped ELF + manifest):

```bash
# Using the built-in converter (on host Linux):
./userland/tools/draco-install/draco-deb-to-dracopkg.sh my-app
# Or build the draco-install tool and run:
draco-install pack my-app/
```

## Syscall Compatibility

DracolaxOS implements a subset of the Linux x86_64 syscall table. Supported syscalls include: `read`, `write`, `open`, `close`, `exit`, `exit_group`, `brk`, `mmap`, `munmap`, `getpid`, `stat`, `fstat`, `lstat`, `lseek`, `dup`, `dup2`, `pipe`, `fork` (stub), `execve` (stub).

Syscalls not yet implemented return `-ENOSYS`. Check `kernel/linux/syscall_64.tbl` for the current list.

## GUI Apps (Future)

GUI app support (windowed, via the DracolaxOS compositor) is planned for v3.0. Currently only CLI apps are supported via the `exec` shell command.

## Submitting Apps

1. Ensure `draco.json` is complete and valid JSON.
2. Test with `exec <your-binary>` in the DracolaxOS shell.
3. Package with `draco-install pack`.
4. Open a PR to `userland/apps/` with your app folder.
