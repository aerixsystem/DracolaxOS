#!/usr/bin/env python3
"""
Image to C header converter for DracolaxOS wallpapers and embedded assets.

Features:
- Open PNG, JPG, BMP, GIF, WEBP, TIFF, ICO, and any format supported by Pillow
- Convert to a C header with raw 32-bit ARGB pixels
- Save a header with width, height, and pixel array
- Simple GUI with file picker
- Optional drag and drop if tkinterdnd2 is installed

Install Pillow:
    pip install pillow

Optional drag and drop:
    pip install tkinterdnd2
"""

from __future__ import annotations

import os
import re
import sys
import subprocess
from dataclasses import dataclass
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox

try:
    from PIL import Image
except ImportError as exc:
    raise SystemExit("Pillow is required. Install it with: pip install pillow") from exc

# Optional drag and drop support
DND_AVAILABLE = False
DND_FILES = None
BASE_TK = tk.Tk

try:
    import tkinterdnd2  # type: ignore
    from tkinterdnd2 import DND_FILES as _DND_FILES  # type: ignore

    DND_AVAILABLE = True
    DND_FILES = _DND_FILES
    BASE_TK = tkinterdnd2.Tk  # type: ignore[attr-defined]
except Exception:
    DND_AVAILABLE = False
    DND_FILES = None
    BASE_TK = tk.Tk


SUPPORTED_EXTS = {
    ".png",
    ".jpg",
    ".jpeg",
    ".bmp",
    ".gif",
    ".webp",
    ".tif",
    ".tiff",
    ".ico",
}


@dataclass
class ConversionOptions:
    input_path: Path
    output_path: Path
    symbol_name: str
    force_square: bool = False
    target_width: int | None = None
    target_height: int | None = None


def sanitize_symbol_name(path: Path) -> str:
    base = path.stem.lower()
    base = re.sub(r"[^a-z0-9_]+", "_", base)
    base = re.sub(r"_+", "_", base).strip("_")
    if not base:
        base = "image_asset"
    if base[0].isdigit():
        base = f"img_{base}"
    return base


def parse_output_path(input_path: Path, output_text: str) -> Path:
    output_text = output_text.strip()
    if not output_text:
        return input_path.with_suffix(".h")

    out = Path(output_text).expanduser()
    if out.is_dir():
        return out / f"{input_path.stem}.h"
    if out.suffix.lower() != ".h":
        return out.with_suffix(".h")
    return out


def image_to_argb_header(opts: ConversionOptions) -> str:
    img = Image.open(opts.input_path).convert("RGBA")

    if opts.target_width and opts.target_height:
        img = img.resize(
            (opts.target_width, opts.target_height),
            Image.Resampling.LANCZOS,
        )
    elif opts.force_square:
        size = max(img.size)
        canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        x = (size - img.width) // 2
        y = (size - img.height) // 2
        canvas.paste(img, (x, y))
        img = canvas

    width, height = img.size
    pixels = list(img.getdata())

    values = []
    for r, g, b, a in pixels:
        values.append(f"0x{a:02X}{r:02X}{g:02X}{b:02X}")

    lines = []
    for i in range(0, len(values), 8):
        lines.append("    " + ", ".join(values[i:i + 8]) + ",")

    guard = f"{opts.symbol_name.upper()}_H"
    header = f"""\
/* Auto-generated from: {opts.input_path.name}
 * Width: {width}
 * Height: {height}
 * Format: 0xAARRGGBB, row-major, top-to-bottom
 */

#ifndef {guard}
#define {guard}

#include <stdint.h>

#define {opts.symbol_name.upper()}_W {width}
#define {opts.symbol_name.upper()}_H {height}

static const uint32_t {opts.symbol_name}_pixels[{width * height}] = {{
{os.linesep.join(lines)}
}};

#endif
"""
    return header


def convert_image(opts: ConversionOptions) -> Path:
    header_text = image_to_argb_header(opts)
    opts.output_path.parent.mkdir(parents=True, exist_ok=True)
    opts.output_path.write_text(header_text, encoding="utf-8")
    return opts.output_path


class App(BASE_TK):
    def __init__(self):
        super().__init__()
        self.title("Image to Header Converter")
        self.geometry("760x340")
        self.minsize(680, 300)

        self.input_path = tk.StringVar()
        self.output_path = tk.StringVar()
        self.symbol_name = tk.StringVar()
        self.status = tk.StringVar(value="Pick an image.")

        self._build_ui()
        self._wire_drag_drop()

    def _build_ui(self):
        pad = {"padx": 10, "pady": 8}

        title = tk.Label(self, text="Image to C Header", font=("TkDefaultFont", 16, "bold"))
        title.pack(anchor="w", **pad)

        input_frame = tk.Frame(self)
        input_frame.pack(fill="x", **pad)

        tk.Label(input_frame, text="Input image:").pack(anchor="w")
        row1 = tk.Frame(input_frame)
        row1.pack(fill="x")

        tk.Entry(row1, textvariable=self.input_path).pack(side="left", fill="x", expand=True)
        tk.Button(row1, text="Browse", command=self.browse_input).pack(side="left", padx=(8, 0))

        output_frame = tk.Frame(self)
        output_frame.pack(fill="x", **pad)

        tk.Label(output_frame, text="Output header:").pack(anchor="w")
        row2 = tk.Frame(output_frame)
        row2.pack(fill="x")

        tk.Entry(row2, textvariable=self.output_path).pack(side="left", fill="x", expand=True)
        tk.Button(row2, text="Choose", command=self.browse_output).pack(side="left", padx=(8, 0))

        name_frame = tk.Frame(self)
        name_frame.pack(fill="x", **pad)

        tk.Label(name_frame, text="Symbol name:").pack(anchor="w")
        tk.Entry(name_frame, textvariable=self.symbol_name).pack(fill="x")

        options_frame = tk.Frame(self)
        options_frame.pack(fill="x", **pad)

        self.force_square = tk.BooleanVar(value=False)
        tk.Checkbutton(
            options_frame,
            text="Pad to square canvas",
            variable=self.force_square,
        ).pack(anchor="w")

        self.resize_mode = tk.BooleanVar(value=False)
        tk.Checkbutton(
            options_frame,
            text="Resize to target size",
            variable=self.resize_mode,
            command=self._toggle_resize_fields,
        ).pack(anchor="w")

        resize_row = tk.Frame(options_frame)
        resize_row.pack(fill="x", pady=(4, 0))
        tk.Label(resize_row, text="Width:").pack(side="left")
        self.width_entry = tk.Entry(resize_row, width=8)
        self.width_entry.pack(side="left", padx=(4, 12))
        tk.Label(resize_row, text="Height:").pack(side="left")
        self.height_entry = tk.Entry(resize_row, width=8)
        self.height_entry.pack(side="left", padx=(4, 12))
        self.width_entry.configure(state="disabled")
        self.height_entry.configure(state="disabled")

        action_row = tk.Frame(self)
        action_row.pack(fill="x", **pad)

        tk.Button(action_row, text="Convert", command=self.convert).pack(side="left")
        tk.Button(action_row, text="Open output folder", command=self.open_output_folder).pack(side="left", padx=8)

        status = tk.Label(self, textvariable=self.status, anchor="w", justify="left")
        status.pack(fill="x", padx=10, pady=(14, 0))

        hint = (
            "Tip: you can also drag and drop an image into this window."
            if DND_AVAILABLE
            else "Tip: drag and drop needs tkinterdnd2. Browse still works normally."
        )
        tk.Label(self, text=hint, anchor="w", justify="left").pack(fill="x", padx=10, pady=(4, 0))

        self.bind("<Return>", lambda _e: self.convert())

    def _toggle_resize_fields(self):
        state = "normal" if self.resize_mode.get() else "disabled"
        self.width_entry.configure(state=state)
        self.height_entry.configure(state=state)

    def _wire_drag_drop(self):
        if not DND_AVAILABLE:
            return
        try:
            self.drop_target_register(DND_FILES)  # type: ignore[arg-type]
            self.dnd_bind("<<Drop>>", self.on_drop)  # type: ignore[attr-defined]
        except Exception:
            pass

    def on_drop(self, event):
        raw = event.data.strip()

        # Handle paths with braces from tkinterdnd2
        if raw.startswith("{") and raw.endswith("}"):
            raw = raw[1:-1]

        path = Path(raw)
        if path.exists():
            self.input_path.set(str(path))
            self.symbol_name.set(sanitize_symbol_name(path))
            self.status.set(f"Loaded: {path.name}")
            if not self.output_path.get():
                self.output_path.set(str(path.with_suffix(".h")))
        else:
            self.status.set("Dropped item was not a file.")

    def browse_input(self):
        path = filedialog.askopenfilename(
            title="Select an image",
            filetypes=[
                ("Image files", "*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff *.ico"),
                ("All files", "*.*"),
            ],
        )
        if path:
            p = Path(path)
            self.input_path.set(str(p))
            self.symbol_name.set(sanitize_symbol_name(p))
            if not self.output_path.get():
                self.output_path.set(str(p.with_suffix(".h")))

    def browse_output(self):
        path = filedialog.asksaveasfilename(
            title="Save header as",
            defaultextension=".h",
            filetypes=[("C header", "*.h"), ("All files", "*.*")],
        )
        if path:
            self.output_path.set(path)

    def _read_int(self, entry: tk.Entry) -> int | None:
        text = entry.get().strip()
        if not text:
            return None
        try:
            value = int(text)
            return value if value > 0 else None
        except ValueError:
            return None

    def convert(self):
        in_text = self.input_path.get().strip()
        if not in_text:
            messagebox.showerror("Missing input", "Pick an image first.")
            return

        input_path = Path(in_text).expanduser()
        if not input_path.exists():
            messagebox.showerror("Missing file", "The input file does not exist.")
            return

        if input_path.suffix.lower() not in SUPPORTED_EXTS:
            self.status.set("The file extension is unusual, but Pillow may still open it.")

        symbol = self.symbol_name.get().strip() or sanitize_symbol_name(input_path)
        output_path = parse_output_path(input_path, self.output_path.get())

        target_width = target_height = None
        if self.resize_mode.get():
            target_width = self._read_int(self.width_entry)
            target_height = self._read_int(self.height_entry)
            if not target_width or not target_height:
                messagebox.showerror("Invalid size", "Enter valid width and height values.")
                return

        opts = ConversionOptions(
            input_path=input_path,
            output_path=output_path,
            symbol_name=symbol,
            force_square=self.force_square.get(),
            target_width=target_width,
            target_height=target_height,
        )

        try:
            out = convert_image(opts)
        except Exception as exc:
            messagebox.showerror("Conversion failed", str(exc))
            return

        self.output_path.set(str(out))
        self.status.set(f"Saved header: {out}")

    def open_output_folder(self):
        raw = self.output_path.get().strip()
        if raw:
            p = Path(raw).expanduser()
            folder = p.parent if p.suffix.lower() == ".h" else p
        else:
            folder = Path.cwd()

        try:
            if sys.platform.startswith("win"):
                os.startfile(folder)  # type: ignore[attr-defined]
            elif sys.platform == "darwin":
                subprocess.Popen(["open", str(folder)])
            else:
                subprocess.Popen(["xdg-open", str(folder)])
        except Exception as exc:
            messagebox.showerror("Open folder failed", str(exc))


def cli_mode(argv: list[str]) -> int:
    if len(argv) < 2:
        print("Usage: python image_to_header.py input_image [output_header]")
        return 1

    input_path = Path(argv[1]).expanduser()
    if not input_path.exists():
        print(f"Input file not found: {input_path}")
        return 1

    output_path = parse_output_path(input_path, argv[2] if len(argv) > 2 else "")
    symbol = sanitize_symbol_name(input_path)

    opts = ConversionOptions(
        input_path=input_path,
        output_path=output_path,
        symbol_name=symbol,
    )

    try:
        out = convert_image(opts)
    except Exception as exc:
        print(f"Conversion failed: {exc}")
        return 1

    print(f"Saved: {out}")
    return 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] != "--gui":
        return cli_mode(sys.argv)

    app = App()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())