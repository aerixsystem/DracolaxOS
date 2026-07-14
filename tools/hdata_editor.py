#!/usr/bin/env python3
"""
hdata_editor.py — DracolaxOS Header Data Editor
================================================
GUI tool for creating, viewing, and editing C .h files that embed binary
data (images, icons, audio, video frames, raw bytes) as const uint8_t[] arrays.

UI Layout
---------
┌─────────────────────────────────────────────────────────────────────────────┐
│ File  Edit  Header-Info  Settings                                            │  ← Menu bar
├──────────────────────────────────────────────┬──────────────────────────────┤
│ Data entries                  [+] [−] [↑] [↓]│  Preview                     │
│ ─────────────────────────────────────────────│  ──────────────────────────  │
│  static const uint8_t terminal[]  (32×32 DXI)│  [image / hex dump]          │
│  static const uint8_t file_manager[] (32×32) │                              │
│  …                                           │                              │
├──────────────────────────────────────────────┤                              │
│ Aggregates / lookup tables                   │                              │
│  static const builtin_icon_t builtin_icons[] │                              │
│  …                                           │                              │
└──────────────────────────────────────────────┴──────────────────────────────┘

Right-click on a data entry → Edit / Rename / Delete / Export

CLI Usage
---------
  hdata_editor.py <target.h>                  Open file in GUI
  hdata_editor.py list   <target.h>           List all data entry names
  hdata_editor.py add    <target.h> <name> <src_file>   Add entry from file
  hdata_editor.py delete <target.h> <name>    Delete entry by name
  hdata_editor.py update <target.h> <name> <src_file>   Replace entry data
  hdata_editor.py export <target.h> <name> <out_file>   Export raw bytes
  hdata_editor.py info   <target.h>           Show header #defines, #includes…
"""

import sys
import os
import re
import struct
import argparse
import textwrap
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog
try:
    from PIL import Image, ImageTk
    PIL_AVAILABLE = True
except ImportError:
    PIL_AVAILABLE = False

# ─────────────────────────────────────────────────────────────────────────────
# .h file parser / writer
# ─────────────────────────────────────────────────────────────────────────────

class HeaderFile:
    """Parses and writes C .h files containing embedded binary arrays."""

    # Regex patterns
    _RE_GUARD   = re.compile(r'#ifndef\s+(\w+)')
    _RE_DEFINE  = re.compile(r'#define\s+(\w+)(?:\s+(.*))?')
    _RE_INCLUDE = re.compile(r'#include\s+[<"]([^>"]+)[>"]')
    _RE_TYPEDEF = re.compile(r'typedef\s+struct\s*\{([^}]*)\}\s*(\w+)\s*;', re.DOTALL)
    _RE_ARRAY   = re.compile(
        r'static\s+const\s+(\w+)\s+(\w+)\s*\[\s*\]\s*(?:__attribute__[^=]*)?\s*=\s*\{([^}]*)\}\s*;',
        re.DOTALL)
    _RE_STRUCT_ARRAY = re.compile(
        r'static\s+const\s+(\w+)\s+(\w+)\s*\[\s*\]\s*(?:__attribute__[^=]*)?\s*=\s*\{(.*?)\}\s*;',
        re.DOTALL)

    def __init__(self):
        self.path        = None
        self.guard       = None          # e.g. "ICON_DATA_H"
        self.defines     = []            # list of (name, value, comment)
        self.includes    = []            # list of include strings
        self.typedefs    = []            # list of (body_text, type_name)
        self.arrays      = []            # list of DataEntry
        self.struct_arrays = []          # list of StructArrayEntry
        self.preamble    = ""            # text before the first #ifndef

    # ── DataEntry ────────────────────────────────────────────────────────────
    class DataEntry:
        __slots__ = ('ctype', 'name', 'raw_bytes', 'comment')
        def __init__(self, ctype, name, raw_bytes, comment=""):
            self.ctype     = ctype
            self.name      = name
            self.raw_bytes = raw_bytes   # bytes object
            self.comment   = comment

        def byte_count(self):
            return len(self.raw_bytes)

        def to_c(self, cols=16):
            lines = [f"static const {self.ctype} {self.name}[] = {{"]
            row = []
            for i, b in enumerate(self.raw_bytes):
                row.append(f"0x{b:02X}")
                if len(row) == cols:
                    lines.append("    " + ",".join(row) + ",")
                    row = []
            if row:
                lines.append("    " + ",".join(row))
            lines.append("};")
            if self.comment:
                lines.insert(0, f"/* {self.comment} */")
            return "\n".join(lines)

    class StructArrayEntry:
        __slots__ = ('ctype', 'name', 'body_text', 'comment')
        def __init__(self, ctype, name, body_text, comment=""):
            self.ctype     = ctype
            self.name      = name
            self.body_text = body_text
            self.comment   = comment

        def to_c(self):
            lines = []
            if self.comment:
                lines.append(f"/* {self.comment} */")
            lines.append(f"static const {self.ctype} {self.name}[] = {{")
            lines.append(self.body_text)
            lines.append("};")
            return "\n".join(lines)

    # ── Load ─────────────────────────────────────────────────────────────────
    def load(self, path):
        self.path = path
        with open(path, 'r', errors='replace') as f:
            src = f.read()
        self._parse(src)

    def _parse(self, src):
        # Guard
        m = self._RE_GUARD.search(src)
        if m:
            self.guard = m.group(1)

        # #define lines
        for m in self._RE_DEFINE.finditer(src):
            name = m.group(1)
            val  = (m.group(2) or "").strip()
            self.defines.append((name, val, ""))

        # #include lines
        for m in self._RE_INCLUDE.finditer(src):
            self.includes.append(m.group(1))

        # typedef struct
        for m in self._RE_TYPEDEF.finditer(src):
            self.typedefs.append((m.group(1), m.group(2)))

        # Binary arrays (uint8_t, uint32_t, etc.)
        for m in self._RE_ARRAY.finditer(src):
            ctype = m.group(1)
            name  = m.group(2)
            body  = m.group(3).strip()
            # Try to parse as byte array
            tokens = [t.strip() for t in body.split(',') if t.strip()]
            raw = bytearray()
            ok  = True
            for tok in tokens:
                try:
                    v = int(tok, 0)
                    raw.append(v & 0xFF)
                except ValueError:
                    ok = False; break
            if ok and raw:
                self.arrays.append(self.DataEntry(ctype, name, bytes(raw)))
            else:
                self.struct_arrays.append(
                    self.StructArrayEntry(ctype, name, m.group(3)))

    # ── Save ─────────────────────────────────────────────────────────────────
    def save(self, path=None):
        path = path or self.path
        if not path:
            raise ValueError("No path set")
        with open(path, 'w') as f:
            f.write(self._generate())
        self.path = path

    def _generate(self):
        lines = []
        guard = self.guard or "HEADER_DATA_H"
        lines.append(f"/* Auto-generated by hdata_editor.py — do not edit by hand */")
        lines.append(f"#ifndef {guard}")
        lines.append(f"#define {guard}")
        lines.append("")

        for inc in self.includes:
            q = '<' if '/' not in inc else '"'
            e = '>' if q == '<' else '"'
            lines.append(f"#include {q}{inc}{e}")
        if self.includes:
            lines.append("")

        for name, val, _ in self.defines:
            if name == guard:
                continue
            lines.append(f"#define {name}  {val}".rstrip())
        if self.defines:
            lines.append("")

        for body, tname in self.typedefs:
            lines.append(f"typedef struct {{{body}}} {tname};")
        if self.typedefs:
            lines.append("")

        for entry in self.arrays:
            lines.append(entry.to_c())
            lines.append("")

        for entry in self.struct_arrays:
            lines.append(entry.to_c())
            lines.append("")

        lines.append(f"#endif /* {guard} */")
        return "\n".join(lines) + "\n"

    # ── Helpers ───────────────────────────────────────────────────────────────
    def find_entry(self, name):
        for e in self.arrays:
            if e.name == name:
                return e
        return None

    def add_entry_from_file(self, name, src_path, ctype="uint8_t"):
        with open(src_path, 'rb') as f:
            data = f.read()
        comment = f"Source: {os.path.basename(src_path)}  ({len(data)} bytes)"
        e = self.DataEntry(ctype, name, data, comment)
        self.arrays.append(e)
        return e

    def delete_entry(self, name):
        self.arrays = [e for e in self.arrays if e.name != name]

    def update_entry(self, name, src_path):
        e = self.find_entry(name)
        if not e:
            raise KeyError(f"Entry '{name}' not found")
        with open(src_path, 'rb') as f:
            e.raw_bytes = f.read()

    def export_entry(self, name, out_path):
        e = self.find_entry(name)
        if not e:
            raise KeyError(f"Entry '{name}' not found")
        with open(out_path, 'wb') as f:
            f.write(e.raw_bytes)

# ─────────────────────────────────────────────────────────────────────────────
# Preview helpers
# ─────────────────────────────────────────────────────────────────────────────

def _try_dxi_preview(raw: bytes):
    """Return PIL Image if data looks like a DXI file, else None."""
    if len(raw) < 16:
        return None
    magic = raw[:4]
    if magic != b'DRCO':
        return None
    w, h = struct.unpack_from('<HH', raw, 4)
    bpp  = raw[8]
    if bpp != 32 or len(raw) < 16 + w * h * 4:
        return None
    pixel_data = raw[16:16 + w * h * 4]
    # BGRA → RGBA
    img = Image.new('RGBA', (w, h))
    pixels = []
    for i in range(0, len(pixel_data), 4):
        b, g, r, a = pixel_data[i], pixel_data[i+1], pixel_data[i+2], pixel_data[i+3]
        pixels.append((r, g, b, a))
    img.putdata(pixels)
    return img

def _try_image_preview(raw: bytes):
    """Return PIL Image by trying to decode as generic image format."""
    if not PIL_AVAILABLE:
        return None
    import io
    try:
        img = Image.open(io.BytesIO(raw))
        img.load()
        return img
    except Exception:
        return None

def make_preview_image(entry, max_size=200):
    """Return (PhotoImage|None, description_str)."""
    raw = entry.raw_bytes
    img = _try_dxi_preview(raw)
    if img is None:
        img = _try_image_preview(raw)

    desc = f"{len(raw)} bytes"
    if img:
        desc = f"{img.width}×{img.height}  {img.mode}  ({len(raw)} bytes)"
        # Scale up small icons for visibility
        scale = max(1, min(max_size // img.width, max_size // img.height))
        if scale > 1:
            img = img.resize((img.width * scale, img.height * scale),
                             Image.NEAREST)
        elif img.width > max_size or img.height > max_size:
            img.thumbnail((max_size, max_size), Image.LANCZOS)
        if PIL_AVAILABLE:
            return ImageTk.PhotoImage(img), desc
    return None, desc

# ─────────────────────────────────────────────────────────────────────────────
# Header-Info editor dialog
# ─────────────────────────────────────────────────────────────────────────────

class HeaderInfoDialog(tk.Toplevel):
    def __init__(self, parent, hdr: HeaderFile):
        super().__init__(parent)
        self.hdr    = hdr
        self.result = False
        self.title("Header Info — #ifndef / #define / #include / typedef")
        self.resizable(True, True)
        self.geometry("700x500")
        self._build()
        self.grab_set()

    def _build(self):
        nb = ttk.Notebook(self)
        nb.pack(fill='both', expand=True, padx=6, pady=6)

        # ── Guard ──
        f1 = ttk.Frame(nb); nb.add(f1, text="#ifndef guard")
        ttk.Label(f1, text="Guard name:").pack(anchor='w', padx=8, pady=4)
        self._guard_var = tk.StringVar(value=self.hdr.guard or "")
        ttk.Entry(f1, textvariable=self._guard_var, width=40).pack(padx=8)

        # ── #define ──
        f2 = ttk.Frame(nb); nb.add(f2, text="#define")
        ttk.Label(f2, text="One '#define NAME  VALUE' per line (omit #define):").pack(anchor='w', padx=8, pady=4)
        self._def_text = tk.Text(f2, height=16, font=("Courier", 10))
        self._def_text.pack(fill='both', expand=True, padx=8, pady=4)
        for name, val, _ in self.hdr.defines:
            if name != self.hdr.guard:
                self._def_text.insert('end', f"{name}  {val}\n")

        # ── #include ──
        f3 = ttk.Frame(nb); nb.add(f3, text="#include")
        ttk.Label(f3, text="One filename per line (e.g.  stdint.h  or  ../types.h):").pack(anchor='w', padx=8, pady=4)
        self._inc_text = tk.Text(f3, height=16, font=("Courier", 10))
        self._inc_text.pack(fill='both', expand=True, padx=8, pady=4)
        for inc in self.hdr.includes:
            self._inc_text.insert('end', inc + "\n")

        # ── typedef struct ──
        f4 = ttk.Frame(nb); nb.add(f4, text="typedef struct")
        ttk.Label(f4, text="Raw typedef source (one per block, separated by blank line):").pack(anchor='w', padx=8, pady=4)
        self._td_text = tk.Text(f4, height=16, font=("Courier", 10))
        self._td_text.pack(fill='both', expand=True, padx=8, pady=4)
        for body, tname in self.hdr.typedefs:
            self._td_text.insert('end', f"typedef struct {{{body}}} {tname};\n\n")

        # ── Buttons ──
        bf = ttk.Frame(self); bf.pack(fill='x', padx=6, pady=4)
        ttk.Button(bf, text="Apply", command=self._apply).pack(side='right', padx=4)
        ttk.Button(bf, text="Cancel", command=self.destroy).pack(side='right')

    def _apply(self):
        self.hdr.guard   = self._guard_var.get().strip() or "HEADER_DATA_H"
        # parse defines
        self.hdr.defines = []
        for line in self._def_text.get('1.0','end').splitlines():
            line = line.strip()
            if not line: continue
            parts = line.split(None, 1)
            name = parts[0]; val = parts[1] if len(parts)>1 else ""
            self.hdr.defines.append((name, val, ""))
        # parse includes
        self.hdr.includes = [l.strip() for l in
                             self._inc_text.get('1.0','end').splitlines()
                             if l.strip()]
        # typedef — just store raw for now
        self.hdr.typedefs = []
        self.result = True
        self.destroy()

# ─────────────────────────────────────────────────────────────────────────────
# Entry editor dialog (for editing raw hex / metadata)
# ─────────────────────────────────────────────────────────────────────────────

class EntryEditDialog(tk.Toplevel):
    def __init__(self, parent, entry: HeaderFile.DataEntry):
        super().__init__(parent)
        self.entry  = entry
        self.result = False
        self.title(f"Edit: {entry.name}")
        self.geometry("680x520")
        self.resizable(True, True)
        self._build()
        self.grab_set()

    def _build(self):
        top = ttk.Frame(self); top.pack(fill='x', padx=8, pady=6)
        ttk.Label(top, text="Name:").grid(row=0, column=0, sticky='w')
        self._name_var = tk.StringVar(value=self.entry.name)
        ttk.Entry(top, textvariable=self._name_var, width=32).grid(row=0, column=1, padx=4)
        ttk.Label(top, text="C type:").grid(row=0, column=2, padx=(12,0))
        self._ctype_var = tk.StringVar(value=self.entry.ctype)
        ttk.Entry(top, textvariable=self._ctype_var, width=16).grid(row=0, column=3, padx=4)
        ttk.Label(top, text="Comment:").grid(row=1, column=0, sticky='w', pady=4)
        self._comment_var = tk.StringVar(value=self.entry.comment)
        ttk.Entry(top, textvariable=self._comment_var, width=60).grid(row=1, column=1, columnspan=3, padx=4)

        nb = ttk.Notebook(self); nb.pack(fill='both', expand=True, padx=8)
        # Hex editor
        fhex = ttk.Frame(nb); nb.add(fhex, text="Hex")
        ttk.Label(fhex, text="Raw bytes (space-separated hex, e.g. 0x44 0xFF …):").pack(anchor='w', padx=4, pady=2)
        self._hex_text = tk.Text(fhex, font=("Courier", 9), wrap='word')
        sb = ttk.Scrollbar(fhex, command=self._hex_text.yview)
        self._hex_text.configure(yscrollcommand=sb.set)
        sb.pack(side='right', fill='y')
        self._hex_text.pack(fill='both', expand=True, padx=4, pady=4)
        hex_str = ' '.join(f'0x{b:02X}' for b in self.entry.raw_bytes)
        self._hex_text.insert('1.0', hex_str)
        # File import
        fimp = ttk.Frame(nb); nb.add(fimp, text="Import from file")
        ttk.Label(fimp, text="Replace entry data by importing a file:").pack(anchor='w', padx=8, pady=8)
        self._import_path = tk.StringVar()
        ttk.Entry(fimp, textvariable=self._import_path, width=50).pack(padx=8)
        ttk.Button(fimp, text="Browse…", command=self._browse_import).pack(padx=8, pady=4)

        bf = ttk.Frame(self); bf.pack(fill='x', padx=8, pady=6)
        ttk.Button(bf, text="Save", command=self._save).pack(side='right', padx=4)
        ttk.Button(bf, text="Cancel", command=self.destroy).pack(side='right')

    def _browse_import(self):
        p = filedialog.askopenfilename(parent=self, title="Import file")
        if p: self._import_path.set(p)

    def _save(self):
        self.entry.name    = self._name_var.get().strip()
        self.entry.ctype   = self._ctype_var.get().strip() or "uint8_t"
        self.entry.comment = self._comment_var.get().strip()
        # Import from file takes priority
        imp = self._import_path.get().strip()
        if imp and os.path.isfile(imp):
            with open(imp, 'rb') as f:
                self.entry.raw_bytes = f.read()
        else:
            # Parse hex text
            tokens = self._hex_text.get('1.0','end').split()
            raw = bytearray()
            for tok in tokens:
                try:
                    raw.append(int(tok, 0) & 0xFF)
                except ValueError:
                    pass
            if raw:
                self.entry.raw_bytes = bytes(raw)
        self.result = True
        self.destroy()

# ─────────────────────────────────────────────────────────────────────────────
# Main application window
# ─────────────────────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self, initial_file=None):
        super().__init__()
        self.title("hdata_editor — DracolaxOS Header Data Editor")
        self.geometry("1060x640")
        self.minsize(700, 480)
        self.hdr  = HeaderFile()
        self._photo_ref = None   # keep PhotoImage alive
        self._build_ui()
        self._apply_theme()
        if initial_file and os.path.isfile(initial_file):
            self._do_open(initial_file)

    # ── Theme ────────────────────────────────────────────────────────────────
    def _apply_theme(self):
        style = ttk.Style(self)
        style.theme_use('clam')
        BG, FG, SEL = '#0D1117', '#C9D1D9', '#21262D'
        style.configure('.', background=BG, foreground=FG, fieldbackground='#161B22',
                        troughcolor=SEL, bordercolor='#30363D', lightcolor='#21262D',
                        darkcolor='#21262D', selectbackground='#2D1A4A',
                        selectforeground='#F0F0FF')
        self.configure(bg=BG)

    # ── UI ───────────────────────────────────────────────────────────────────
    def _build_ui(self):
        # ── Menu bar ──
        mb = tk.Menu(self, tearoff=0)
        self.config(menu=mb)

        fm = tk.Menu(mb, tearoff=0)
        fm.add_command(label="New",   accelerator="Ctrl+N", command=self._new)
        fm.add_command(label="Open…", accelerator="Ctrl+O", command=self._open)
        fm.add_command(label="Save",  accelerator="Ctrl+S", command=self._save)
        fm.add_command(label="Save As…",                    command=self._save_as)
        fm.add_separator()
        fm.add_command(label="Exit",  accelerator="Ctrl+Q", command=self.quit)
        mb.add_cascade(label="File", menu=fm)

        em = tk.Menu(mb, tearoff=0)
        em.add_command(label="Add entry…",    command=self._add_entry)
        em.add_command(label="Delete selected", command=self._delete_selected)
        em.add_separator()
        em.add_command(label="Import file into selected…", command=self._import_selected)
        em.add_command(label="Export selected to file…",  command=self._export_selected)
        mb.add_cascade(label="Edit", menu=em)

        hm = tk.Menu(mb, tearoff=0)
        hm.add_command(label="Edit header info…", command=self._edit_header_info)
        mb.add_cascade(label="Header-Info", menu=hm)

        sm = tk.Menu(mb, tearoff=0)
        sm.add_command(label="About", command=lambda: messagebox.showinfo(
            "About", "hdata_editor.py\nDracolaxOS header binary data editor\n"
                     "Drag-and-drop a file onto the entry list to add it."))
        mb.add_cascade(label="Settings", menu=sm)

        # ── Paned layout ──
        pane = ttk.PanedWindow(self, orient='horizontal')
        pane.pack(fill='both', expand=True, padx=4, pady=4)

        # ── Left panel ──
        left = ttk.Frame(pane, width=520)
        pane.add(left, weight=3)

        # Data entries section
        ttk.Label(left, text="Data entries (static const arrays)").pack(anchor='w', padx=4)
        entry_frame = ttk.Frame(left)
        entry_frame.pack(fill='both', expand=True, padx=4)
        cols = ("name", "type", "size", "comment")
        self._tree = ttk.Treeview(entry_frame, columns=cols, show='headings', selectmode='browse')
        for c, w, label in zip(cols, (220, 70, 80, 250), ("Name", "Type", "Size", "Comment")):
            self._tree.heading(c, text=label)
            self._tree.column(c, width=w, minwidth=40)
        vsb = ttk.Scrollbar(entry_frame, orient='vertical', command=self._tree.yview)
        self._tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side='right', fill='y')
        self._tree.pack(fill='both', expand=True)
        self._tree.bind('<<TreeviewSelect>>', self._on_select)
        self._tree.bind('<Double-1>', lambda e: self._edit_selected())
        self._tree.bind('<Button-3>', self._on_right_click)
        # Drag-and-drop
        try:
            self._tree.drop_target_register('DND_Files')
            self._tree.dnd_bind('<<Drop>>', self._on_drop)
        except Exception:
            pass

        # Aggregate / lookup tables section
        ttk.Separator(left, orient='horizontal').pack(fill='x', padx=4, pady=4)
        ttk.Label(left, text="Aggregates / lookup tables (struct arrays)").pack(anchor='w', padx=4)
        agg_frame = ttk.Frame(left)
        agg_frame.pack(fill='both', expand=True, padx=4)
        acols = ("name", "type", "lines")
        self._agg_tree = ttk.Treeview(agg_frame, columns=acols, show='headings', selectmode='browse', height=6)
        for c, w, label in zip(acols, (200, 120, 300), ("Name", "Type", "Preview")):
            self._agg_tree.heading(c, text=label)
            self._agg_tree.column(c, width=w, minwidth=40)
        self._agg_tree.pack(fill='both', expand=True)
        self._agg_tree.bind('<Double-1>', self._edit_aggregate)

        # Toolbar buttons
        tb = ttk.Frame(left)
        tb.pack(fill='x', padx=4, pady=2)
        ttk.Button(tb, text="+ Add",    command=self._add_entry).pack(side='left', padx=2)
        ttk.Button(tb, text="− Delete", command=self._delete_selected).pack(side='left', padx=2)
        ttk.Button(tb, text="↑ Up",     command=lambda: self._move(-1)).pack(side='left', padx=2)
        ttk.Button(tb, text="↓ Down",   command=lambda: self._move(+1)).pack(side='left', padx=2)
        ttk.Button(tb, text="Edit…",    command=self._edit_selected).pack(side='left', padx=2)

        # ── Right panel ──
        right = ttk.Frame(pane, width=340)
        pane.add(right, weight=2)
        ttk.Label(right, text="Preview").pack(anchor='w', padx=4)
        self._preview_canvas = tk.Canvas(right, bg='#0D1117', highlightthickness=1,
                                          highlightbackground='#30363D')
        self._preview_canvas.pack(fill='both', expand=True, padx=4, pady=4)
        self._preview_label = ttk.Label(right, text="", foreground='#8B949E')
        self._preview_label.pack(anchor='w', padx=4, pady=(0,4))

        # Status bar
        self._status = tk.StringVar(value="Ready.  No file open.")
        ttk.Label(self, textvariable=self._status, relief='sunken',
                  anchor='w').pack(side='bottom', fill='x', padx=2)

        # Keyboard shortcuts
        self.bind('<Control-n>', lambda e: self._new())
        self.bind('<Control-o>', lambda e: self._open())
        self.bind('<Control-s>', lambda e: self._save())
        self.bind('<Control-q>', lambda e: self.quit())

        # Context menu
        self._ctx_menu = tk.Menu(self, tearoff=0)
        self._ctx_menu.add_command(label="Edit…",             command=self._edit_selected)
        self._ctx_menu.add_command(label="Rename…",           command=self._rename_selected)
        self._ctx_menu.add_separator()
        self._ctx_menu.add_command(label="Import from file…", command=self._import_selected)
        self._ctx_menu.add_command(label="Export raw bytes…", command=self._export_selected)
        self._ctx_menu.add_separator()
        self._ctx_menu.add_command(label="Delete",            command=self._delete_selected)

    # ── Helpers ───────────────────────────────────────────────────────────────
    def _refresh_tree(self):
        self._tree.delete(*self._tree.get_children())
        for e in self.hdr.arrays:
            self._tree.insert('', 'end', iid=e.name, values=(
                e.name, e.ctype, f"{e.byte_count()} B", e.comment[:60]))
        self._agg_tree.delete(*self._agg_tree.get_children())
        for sa in self.hdr.struct_arrays:
            preview = sa.body_text[:80].replace('\n',' ')
            self._agg_tree.insert('', 'end', iid=sa.name, values=(sa.name, sa.ctype, preview))
        path = self.hdr.path or "untitled"
        self._status.set(f"{os.path.basename(path)}  —  {len(self.hdr.arrays)} entries")
        self.title(f"hdata_editor — {os.path.basename(path)}")

    def _selected_entry(self):
        sel = self._tree.selection()
        if not sel: return None
        return self.hdr.find_entry(sel[0])

    def _on_select(self, _event=None):
        e = self._selected_entry()
        self._preview_canvas.delete('all')
        self._preview_label.config(text="")
        if not e:
            return
        photo, desc = make_preview_image(e, max_size=280)
        self._photo_ref = photo
        if photo:
            cx = self._preview_canvas.winfo_width()  // 2 or 170
            cy = self._preview_canvas.winfo_height() // 2 or 170
            self._preview_canvas.create_image(cx, cy, image=photo, anchor='center')
        else:
            # Hex dump first 256 bytes
            lines = []
            for off in range(0, min(256, len(e.raw_bytes)), 16):
                chunk = e.raw_bytes[off:off+16]
                hex_s = ' '.join(f'{b:02X}' for b in chunk)
                asc_s = ''.join(chr(b) if 32<=b<127 else '.' for b in chunk)
                lines.append(f"{off:04X}  {hex_s:<47}  {asc_s}")
            self._preview_canvas.create_text(
                8, 8, text="\n".join(lines), anchor='nw',
                font=("Courier", 8), fill='#8B949E')
        self._preview_label.config(text=desc)

    def _on_right_click(self, event):
        item = self._tree.identify_row(event.y)
        if item:
            self._tree.selection_set(item)
            self._ctx_menu.tk_popup(event.x_root, event.y_root)

    def _on_drop(self, event):
        paths = self.tk.splitlist(event.data)
        for p in paths:
            name = os.path.splitext(os.path.basename(p))[0]
            name = re.sub(r'\W', '_', name)
            if self.hdr.find_entry(name):
                name += "_2"
            self.hdr.add_entry_from_file(name, p)
        self._refresh_tree()

    # ── Commands ──────────────────────────────────────────────────────────────
    def _new(self):
        self.hdr = HeaderFile()
        self.hdr.guard = "HEADER_DATA_H"
        self.hdr.includes = ["../../kernel/types.h"]
        self._refresh_tree()

    def _open(self):
        p = filedialog.askopenfilename(
            filetypes=[("C header", "*.h"), ("All", "*.*")])
        if p: self._do_open(p)

    def _do_open(self, p):
        try:
            self.hdr = HeaderFile()
            self.hdr.load(p)
            self._refresh_tree()
            self._status.set(f"Opened {p}  ({len(self.hdr.arrays)} entries)")
        except Exception as ex:
            messagebox.showerror("Open failed", str(ex))

    def _save(self):
        if not self.hdr.path:
            self._save_as(); return
        try:
            self.hdr.save()
            self._status.set(f"Saved {self.hdr.path}")
        except Exception as ex:
            messagebox.showerror("Save failed", str(ex))

    def _save_as(self):
        p = filedialog.asksaveasfilename(
            defaultextension=".h",
            filetypes=[("C header", "*.h"), ("All", "*.*")])
        if p:
            try:
                self.hdr.save(p)
                self._refresh_tree()
            except Exception as ex:
                messagebox.showerror("Save failed", str(ex))

    def _add_entry(self):
        p = filedialog.askopenfilename(title="Select file to embed")
        if not p: return
        name = simpledialog.askstring("Entry name",
            f"Variable name for this data\n(e.g. icon_data_terminal):",
            initialvalue=re.sub(r'\W','_', os.path.splitext(os.path.basename(p))[0]))
        if not name: return
        name = re.sub(r'\W', '_', name)
        if self.hdr.find_entry(name):
            messagebox.showerror("Duplicate", f"Entry '{name}' already exists.")
            return
        self.hdr.add_entry_from_file(name, p)
        self._refresh_tree()

    def _delete_selected(self):
        e = self._selected_entry()
        if not e: return
        if messagebox.askyesno("Delete", f"Delete '{e.name}'?"):
            self.hdr.delete_entry(e.name)
            self._refresh_tree()

    def _edit_selected(self):
        e = self._selected_entry()
        if not e: return
        dlg = EntryEditDialog(self, e)
        self.wait_window(dlg)
        if dlg.result:
            self._refresh_tree()
            self._on_select()

    def _rename_selected(self):
        e = self._selected_entry()
        if not e: return
        new_name = simpledialog.askstring("Rename", "New name:", initialvalue=e.name)
        if new_name:
            e.name = re.sub(r'\W', '_', new_name.strip())
            self._refresh_tree()

    def _import_selected(self):
        e = self._selected_entry()
        if not e: return
        p = filedialog.askopenfilename(title="Import file into entry")
        if p:
            try:
                self.hdr.update_entry(e.name, p)
                self._refresh_tree(); self._on_select()
            except Exception as ex:
                messagebox.showerror("Import failed", str(ex))

    def _export_selected(self):
        e = self._selected_entry()
        if not e: return
        p = filedialog.asksaveasfilename(
            title="Export raw bytes", initialfile=e.name + ".bin")
        if p:
            try:
                self.hdr.export_entry(e.name, p)
                messagebox.showinfo("Exported", f"Written {len(e.raw_bytes)} bytes to {p}")
            except Exception as ex:
                messagebox.showerror("Export failed", str(ex))

    def _edit_header_info(self):
        dlg = HeaderInfoDialog(self, self.hdr)
        self.wait_window(dlg)

    def _edit_aggregate(self, event=None):
        sel = self._agg_tree.selection()
        if not sel: return
        name = sel[0]
        for sa in self.hdr.struct_arrays:
            if sa.name == name:
                dlg = tk.Toplevel(self)
                dlg.title(f"Edit aggregate: {name}")
                dlg.geometry("700x400")
                txt = tk.Text(dlg, font=("Courier", 10))
                txt.pack(fill='both', expand=True, padx=6, pady=6)
                txt.insert('1.0', sa.body_text)
                def _apply(sa=sa, txt=txt, dlg=dlg):
                    sa.body_text = txt.get('1.0','end')
                    self._refresh_tree()
                    dlg.destroy()
                ttk.Button(dlg, text="Apply", command=_apply).pack(pady=4)
                break

    def _move(self, direction):
        e = self._selected_entry()
        if not e: return
        arr = self.hdr.arrays
        idx = next((i for i,x in enumerate(arr) if x.name==e.name), -1)
        if idx < 0: return
        new_idx = idx + direction
        if 0 <= new_idx < len(arr):
            arr[idx], arr[new_idx] = arr[new_idx], arr[idx]
            self._refresh_tree()
            self._tree.selection_set(e.name)

# ─────────────────────────────────────────────────────────────────────────────
# CLI interface
# ─────────────────────────────────────────────────────────────────────────────

def cli_main():
    parser = argparse.ArgumentParser(
        prog="hdata_editor.py",
        description="DracolaxOS Header Data Editor — CLI mode",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""
        Commands:
          (no command)            Open GUI for <target_h>
          list   <target_h>       List all data entry names and sizes
          add    <target_h> <name> <src>   Add new entry from file
          delete <target_h> <name>         Delete entry by name
          update <target_h> <name> <src>   Replace entry data from file
          export <target_h> <name> <out>   Export raw bytes to file
          info   <target_h>       Show guard, defines, includes, typedefs

        Examples:
          python hdata_editor.py gui/icons/icon_data.h
          python hdata_editor.py list   gui/icons/icon_data.h
          python hdata_editor.py add    gui/icons/icon_data.h icon_data_paint paint.dxi
          python hdata_editor.py delete gui/icons/icon_data.h icon_data_paint
          python hdata_editor.py update gui/icons/icon_data.h icon_data_terminal terminal_v2.dxi
          python hdata_editor.py export gui/icons/icon_data.h icon_data_terminal out.dxi
        """))

    parser.add_argument('command_or_file', nargs='?', default=None,
                        help="Command or .h file path for GUI mode")
    parser.add_argument('args', nargs='*')
    ns = parser.parse_args()

    commands = {'list','add','delete','update','export','info'}
    cmd = ns.command_or_file

    if cmd is None or (cmd not in commands and cmd.endswith('.h')):
        # GUI mode
        initial = cmd
        app = App(initial_file=initial)
        app.mainloop()
        return

    if cmd not in commands:
        parser.print_help()
        sys.exit(1)

    args = ns.args
    if not args:
        print(f"Error: '{cmd}' requires a .h file path", file=sys.stderr)
        sys.exit(1)

    hdr_path = args[0]
    hdr = HeaderFile()
    if os.path.isfile(hdr_path):
        hdr.load(hdr_path)

    if cmd == 'list':
        if not hdr.arrays:
            print("(no entries)")
        for e in hdr.arrays:
            print(f"  {e.name:<40} {e.ctype:<12} {e.byte_count():>8} bytes  {e.comment}")

    elif cmd == 'info':
        print(f"File  : {hdr_path}")
        print(f"Guard : {hdr.guard}")
        print(f"Defines ({len(hdr.defines)}):")
        for n,v,_ in hdr.defines: print(f"  #define {n}  {v}")
        print(f"Includes ({len(hdr.includes)}):")
        for i in hdr.includes: print(f"  #include \"{i}\"")
        print(f"Typedefs ({len(hdr.typedefs)}):")
        for _,tname in hdr.typedefs: print(f"  typedef struct ... {tname};")
        print(f"Entries  : {len(hdr.arrays)}")
        print(f"Aggregates: {len(hdr.struct_arrays)}")

    elif cmd == 'add':
        if len(args) < 3:
            print("Usage: add <target_h> <name> <src_file>", file=sys.stderr); sys.exit(1)
        name, src = args[1], args[2]
        if not os.path.isfile(src):
            print(f"Error: '{src}' not found", file=sys.stderr); sys.exit(1)
        hdr.add_entry_from_file(name, src)
        hdr.save(hdr_path)
        print(f"Added '{name}' ({hdr.find_entry(name).byte_count()} bytes) -> {hdr_path}")

    elif cmd == 'delete':
        if len(args) < 2:
            print("Usage: delete <target_h> <name>", file=sys.stderr); sys.exit(1)
        name = args[1]
        hdr.delete_entry(name)
        hdr.save(hdr_path)
        print(f"Deleted '{name}' from {hdr_path}")

    elif cmd == 'update':
        if len(args) < 3:
            print("Usage: update <target_h> <name> <src_file>", file=sys.stderr); sys.exit(1)
        name, src = args[1], args[2]
        hdr.update_entry(name, src)
        hdr.save(hdr_path)
        print(f"Updated '{name}' ({hdr.find_entry(name).byte_count()} bytes) -> {hdr_path}")

    elif cmd == 'export':
        if len(args) < 3:
            print("Usage: export <target_h> <name> <out_file>", file=sys.stderr); sys.exit(1)
        name, out = args[1], args[2]
        hdr.export_entry(name, out)
        e = hdr.find_entry(name)
        print(f"Exported '{name}' ({e.byte_count()} bytes) -> {out}")


if __name__ == '__main__':
    cli_main()
