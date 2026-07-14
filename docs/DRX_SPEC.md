# DRX Update Engine — Protocol & Architecture Specification

**Component:** `drx/`  
**Update repo:** `drx/draco-updates/` (GitHub: `aerixsystem/draco-updates`)

---

## Overview

DRX (Draco Update eXchange) is the atomic package and update engine for DracolaxOS. It provides:

- OTA kernel and package updates via GitHub-hosted manifests
- Atomic swap with automatic rollback on failure
- Stable and beta release channels
- Wine integration as a first-class DRX package

---

## Repository Layout (GitHub)

```
aerixsystem/draco-updates/
├── index.json      — full package catalogue (all versions, all channels)
├── latest.json     — current stable and beta version pointers
├── icon.png        — DRX branding
└── README.md
```

### `latest.json` schema

```json
{
  "stable": {
    "version": "1.0.3",
    "kernel_url": "https://github.com/aerixsystem/draco-updates/releases/download/v1.0.3/kernel.elf",
    "manifest_url": "https://github.com/aerixsystem/draco-updates/releases/download/v1.0.3/manifest.json",
    "sha256": "abc123…"
  },
  "beta": {
    "version": "1.1.0-beta2",
    "kernel_url": "…",
    "manifest_url": "…",
    "sha256": "def456…"
  }
}
```

### `index.json` schema

```json
{
  "packages": [
    {
      "id": "wine",
      "name": "Wine",
      "version": "9.0-drx1",
      "channel": "stable",
      "url": "…",
      "sha256": "…",
      "size_bytes": 12345678,
      "depends": [],
      "install_path": "/storage/main/system/runtimes/wine"
    }
  ]
}
```

---

## Update Flow

```
drx update
    │
    ├─ 1. Fetch latest.json from GitHub
    ├─ 2. Compare version against /storage/main/system/manifest.json
    │       └─ If same: print "Already up to date." and exit
    │
    ├─ 3. Download package to /storage/main/system/staging/
    ├─ 4. Verify SHA-256 checksum
    │       └─ If mismatch: delete staging, abort
    │
    ├─ 5. Backup current files to /storage/main/system/backup/
    ├─ 6. Atomic swap: rename staging → live paths
    ├─ 7. Write new manifest.json
    └─ 8. On next boot: recovery checker validates; rolls back if kernel fails to start
```

---

## Atomic Swap Protocol

The swap is designed to be crash-safe. No intermediate state leaves the system unbootable:

1. New files land in `/storage/main/system/staging/` first.
2. Backup copies of replaced files go to `/storage/main/system/backup/`.
3. Each file is renamed (not copied) from staging to its live path — rename is atomic on the RAMFS layer.
4. `manifest.json` is written last. An incomplete update = old manifest still present = recovery checker triggers rollback.

### Rollback

On boot, `drx/recovery/` checker reads `manifest.json` and verifies the kernel ELF signature. If the check fails:

1. Files in `/storage/main/system/backup/` are swapped back.
2. Old `manifest.json` is restored.
3. System reboots into the previous version.
4. A crash log is written to `/storage/main/crash/`.

---

## CLI Commands

```bash
drx update              # check for updates on the stable channel and apply
drx update --beta       # check beta channel
drx install <pkg-id>    # install a package from index.json
drx remove  <pkg-id>    # remove an installed package
drx rollback            # manually roll back to backup
drx status              # show installed version and channel
drx list                # list available packages
```

---

## Wine as a DRX Package

Wine is distributed as a DRX package (`id: "wine"`), not baked into the kernel ISO. This keeps the base ISO small.

Install:
```bash
drx install wine
```

This downloads and unpacks Wine to `/storage/main/system/runtimes/wine/` and registers it in `manifest.json`. The kernel's Linux compatibility shim (`kernel/linux/`) then uses the Wine runtime prefix when an ELF PE-format binary is detected by `kernel/loader/elf_loader.c`.

See `docs/WINE_INTEGRATION.md` for the integration architecture.

---

## Security

- All downloads are verified with SHA-256 before any file is touched.
- `manifest.json` is read-only once written; only DRX can update it.
- DracoShield firewall (`kernel/security/draco-shield/`) must allow outbound HTTPS on port 443 for updates to work. In restricted mode, DRX will print an error and exit without modifying anything.
