DracolaxOS — Built-in App Icons
================================

Location : gui/icons/
Format   : DXI (Draco eXtendend Image) — 32x32 BGRA pixels
Header   : icon_data.h — C header embedding all icons as const uint8_t arrays

HOW ICONS ARE USED
------------------
1. At build time  : icon_data.h embeds every icon's raw .dxi bytes.
2. At boot time   : kernel/init.c iterates builtin_icons[] from icon_data.h
                    and writes each icon to the VFS path:
                    /storage/main/system/shared/images/<appslug>.dxi
3. At runtime     :
   a) gui/compositor/compositor.c  — loads icon into window title bar
      (see render_window(), wicon_cache[] / wicon_tried[])
   b) gui/desktop/default-desktop/desktop.c — loads icon for desktop grid
      (see load_dxi_icons(), g_dxi_icons[])

FALLBACK BEHAVIOUR
------------------
If a .dxi file is NOT found in the VFS (missing from icon_data.h, or VFS
mount failed), both the compositor and the desktop fall back to a coloured
rounded-square placeholder with a 2-letter abbreviation of the app name.

No code changes are needed to add an icon — just:
  1. Generate the .dxi file (use tools/hdata_editor.py or img-to-dxi.py).
  2. Add it to icon_data.h using the BUILTIN_ICON_COUNT / builtin_icons[]
     table in gui/icons/icon_data.h.
  3. Rebuild.

MISSING ICONS (TO DO)
---------------------
The following apps do NOT yet have hand-crafted icons and use the fallback:
  - Disk Manager
  - Trash Manager
  - Draco Shield
  - Draco Manager
  - Login Manager

To fix: create <appslug>.dxi for each, add to icon_data.h, rebuild.

TOOLS
-----
  tools/hdata_editor.py  — GUI tool to view/edit .h files containing
                           embedded binary data (images, icons, etc.)
  img-to-dxi.py          — Convert PNG/JPEG to .dxi format

DXI FORMAT
----------
Offset  Size  Field
  0      4    Magic: "DRCO"
  4      2    Width  (uint16_t LE)
  6      2    Height (uint16_t LE)
  8      1    BPP (32 = BGRA)
  9      1    Flags (0 = none)
 10      6    Reserved (zeros)
 16    W*H*4  Pixel data (BGRA, row-major)
