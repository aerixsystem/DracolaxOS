#!/bin/bash
# draco-deb-to-dracopkg.sh
# Converts a Debian .deb package to a Draco package layout.
#
# Usage: ./draco-deb-to-dracopkg.sh <package.deb> [output-dir]
#
# Output layout:
#   <pkgname>/
#     draco-meta.json      — package metadata
#     files/               — extracted contents from data.tar.*
#
# Requirements on host: ar, tar, jq (optional)

set -euo pipefail

DEB="${1:?Usage: $0 <package.deb> [output-dir]}"
OUTDIR="${2:-./dracopkgs}"
TMPDIR_WORK="$(mktemp -d)"

cleanup() { rm -rf "$TMPDIR_WORK"; }
trap cleanup EXIT

echo "[draco-deb-to-dracopkg] Processing: $DEB"

# ---- Extract .deb (ar archive) -------------------------------------------
cd "$TMPDIR_WORK"
ar x "$DEB" 2>/dev/null || {
    echo "ERROR: 'ar' not found. Install binutils."
    exit 1
}

# Get package name
PKGNAME="$(basename "$DEB" .deb | sed 's/_[0-9].*$//')"
PKGVER="$(basename "$DEB" .deb | sed 's/^[^_]*_//' | sed 's/_[^_]*$//')"
PKGARCH="$(basename "$DEB" .deb | sed 's/.*_//')"

echo "[draco-deb-to-dracopkg] Package: $PKGNAME $PKGVER ($PKGARCH)"

# ---- Parse control information -------------------------------------------
CONTROL_TAR="$(ls control.tar* 2>/dev/null | head -1)"
if [ -n "$CONTROL_TAR" ]; then
    mkdir -p ctrl
    tar -xf "$CONTROL_TAR" -C ctrl 2>/dev/null || true
    DESCRIPTION="$(grep '^Description:' ctrl/control 2>/dev/null | head -1 | sed 's/^Description: //')"
    DEPENDS="$(grep '^Depends:' ctrl/control 2>/dev/null | head -1 | sed 's/^Depends: //')"
else
    DESCRIPTION="Unknown"
    DEPENDS=""
fi

# ---- Check for dynamic dependencies on glibc -----------------------------
IS_STATIC=1
if echo "${DEPENDS:-}" | grep -qiE '(libc6|glibc|libstdc\+\+)'; then
    IS_STATIC=0
    echo "WARNING: Package depends on glibc/libc6 — dynamic binary (experimental)"
    echo "         Prefer musl-compiled static packages."
fi

# ---- Extract data contents -----------------------------------------------
DATA_TAR="$(ls data.tar* 2>/dev/null | head -1)"
if [ -z "$DATA_TAR" ]; then
    echo "ERROR: No data.tar.* found in $DEB"
    exit 1
fi

DEST="$OUTDIR/$PKGNAME/files"
mkdir -p "$DEST"
tar -xf "$DATA_TAR" -C "$DEST" 2>/dev/null || true
echo "[draco-deb-to-dracopkg] Extracted $(find "$DEST" -type f | wc -l) files"

# ---- Write Draco metadata ------------------------------------------------
META="$OUTDIR/$PKGNAME/draco-meta.json"
cat > "$META" << JSON
{
  "name":        "$PKGNAME",
  "version":     "$PKGVER",
  "arch":        "$PKGARCH",
  "description": "$DESCRIPTION",
  "depends":     "$DEPENDS",
  "static":      $IS_STATIC,
  "install_path":"/storage/main/apps/$PKGNAME",
  "origin":      "deb.debian.org"
}
JSON

echo "[draco-deb-to-dracopkg] Metadata written to $META"

# ---- Find main executables -----------------------------------------------
echo "[draco-deb-to-dracopkg] Executables found:"
find "$DEST/usr/bin" "$DEST/bin" "$DEST/sbin" "$DEST/usr/sbin" \
     -type f -perm /111 2>/dev/null | sed 's|.*/||' | head -20 || true

# ---- Verify static (ELF check) -------------------------------------------
if command -v readelf >/dev/null 2>&1; then
    DYNA=$(find "$DEST" -type f -perm /111 \
           -exec readelf -d {} 2>/dev/null \; 2>/dev/null |
           grep 'Shared library' | wc -l)
    if [ "$DYNA" -gt 0 ]; then
        echo "WARNING: $DYNA dynamic library reference(s) found."
        echo "         This package requires a dynamic loader to run."
        echo "         Consider: musl-gcc -static -o <binary> <source>"
    else
        echo "[draco-deb-to-dracopkg] All executables appear static. "
    fi
fi

echo "[draco-deb-to-dracopkg] Done. Output: $OUTDIR/$PKGNAME/"
