import struct
import os
import io
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from PIL import Image, ImageTk

# --- SVG Support via CairoSVG ---
try:
    import cairosvg
    SVG_SUPPORT = True
except ImportError:
    SVG_SUPPORT = False
    print("⚠️ Warning: 'cairosvg' not found. SVG support disabled.")
    print("Run: pip install cairosvg")

# --- Tkinter native drag and drop wrapper ---
try:
    from tkinterdnd2 import TkinterDnD, DND_FILES
except ImportError:
    print("❌ Error: tkinterdnd2 is not installed.")
    print("Run: pip install tkinterdnd2")
    exit(1)

# --- DXI Specification Constants ---
MAGIC = b"DRCO"
HEADER_FORMAT = "<4sHHBB6x"  # Magic(4), W(2), H(2), BPP(1), Comp(1), Padding(6)
HEADER_SIZE = 16

def load_universal_icon(filepath):
    """Loads PNG, JPG, ICO, ICNS, and SVG into a standardized RGBA Pillow Image."""
    ext = os.path.splitext(filepath)[1].lower()
    
    if ext == '.svg':
        if not SVG_SUPPORT:
            raise Exception("SVG conversion requires 'cairosvg' library. Please install it.")
        # Rasterize vector to PNG in memory
        png_bytes = cairosvg.svg2png(url=filepath)
        img = Image.open(io.BytesIO(png_bytes))
        return img.convert("RGBA")
    
    # Pillow natively handles .ico, .icns, .png, .jpg, etc.
    img = Image.open(filepath)
    return img.convert("RGBA")

class DxiStudio(TkinterDnD.Tk):
    def __init__(self):
        super().__init__()
        self.title("DracolaxOS Icon Studio (.dxi)")
        self.geometry("900x700")
        self.minsize(700, 600)
        
        # Setup Notebook (Tabs)
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        # Create Tabs
        self.tab_convert = ttk.Frame(self.notebook)
        self.tab_preview = ttk.Frame(self.notebook)
        
        self.notebook.add(self.tab_convert, text=" 🔄 Convert to .dxi ")
        self.notebook.add(self.tab_preview, text=" 🔍 Inspect .dxi ")
        
        self._setup_convert_tab()
        self._setup_preview_tab()

    # ==========================================
    # TAB 1: CONVERTER (Improved UI + Multi-file)
    # ==========================================
    def _setup_convert_tab(self):
        # Main container with two columns
        main_frame = ttk.Frame(self.tab_convert)
        main_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        # Left: File list and controls
        left_frame = ttk.LabelFrame(main_frame, text="Files to Convert")
        left_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 10))
        
        # Listbox with scrollbar
        list_frame = ttk.Frame(left_frame)
        list_frame.pack(fill=tk.BOTH, expand=True, pady=5)
        self.convert_listbox = tk.Listbox(list_frame, selectmode=tk.EXTENDED, height=12)
        scrollbar = ttk.Scrollbar(list_frame, orient=tk.VERTICAL, command=self.convert_listbox.yview)
        self.convert_listbox.config(yscrollcommand=scrollbar.set)
        self.convert_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        # Control buttons
        btn_frame = ttk.Frame(left_frame)
        btn_frame.pack(fill=tk.X, pady=5)
        ttk.Button(btn_frame, text="➕ Add Files", command=self.add_files_to_convert).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_frame, text="🗑️ Clear All", command=self.clear_convert_list).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_frame, text="🔁 Convert All", command=self.convert_all_files).pack(side=tk.LEFT, padx=2)
        
        # Progress bar
        self.convert_progress = ttk.Progressbar(left_frame, orient=tk.HORIZONTAL, mode='determinate')
        self.convert_progress.pack(fill=tk.X, pady=5)
        self.status_label = ttk.Label(left_frame, text="Ready")
        self.status_label.pack()
        
        # Right: Drag & drop area (visual hint)
        right_frame = ttk.LabelFrame(main_frame, text="Drag & Drop Icons Here")
        right_frame.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True)
        
        drop_label = tk.Label(right_frame, text="📥 Drop any image file here\n(.png, .jpg, .ico, .icns, .svg)\nThey will be added to the list.\n\nIf you drop a .dxi file, you'll be\nswitched to the preview tab.",
                              bg="#f0f0f0", relief="ridge", bd=2)
        drop_label.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        # Register drag-and-drop on the whole tab
        self.tab_convert.drop_target_register(DND_FILES)
        self.tab_convert.dnd_bind('<<Drop>>', self.handle_convert_drop)
    
    def add_files_to_convert(self):
        """Open file dialog to add images to conversion list."""
        files = filedialog.askopenfilenames(
            title="Select Images to Convert",
            filetypes=[
                ("All supported", "*.png *.jpg *.jpeg *.ico *.icns *.svg"),
                ("PNG files", "*.png"),
                ("JPEG files", "*.jpg *.jpeg"),
                ("Windows ICO", "*.ico"),
                ("Mac ICNS", "*.icns"),
                ("SVG files", "*.svg")
            ]
        )
        for f in files:
            if f.lower().endswith('.dxi'):
                messagebox.showinfo("Notice", f"{os.path.basename(f)} is already a .dxi file.\nSwitch to Preview tab to view it.")
                continue
            if f not in self.convert_listbox.get(0, tk.END):
                self.convert_listbox.insert(tk.END, f)
    
    def clear_convert_list(self):
        self.convert_listbox.delete(0, tk.END)
    
    def handle_convert_drop(self, event):
        files = self.split_dnd_files(event.data)
        for path in files:
            if not os.path.isfile(path):
                continue
            if path.lower().endswith('.dxi'):
                # Switch to preview tab and load this file
                self.notebook.select(self.tab_preview)
                # Pass to preview handler
                self.preview_drop_file(path)
                return  # only handle one .dxi at a time? We'll just load the first one.
            else:
                # Add to conversion list
                if path not in self.convert_listbox.get(0, tk.END):
                    self.convert_listbox.insert(tk.END, path)
        self.status_label.config(text=f"Added {len(files)} file(s) to list.")
    
    def convert_all_files(self):
        files = list(self.convert_listbox.get(0, tk.END))
        if not files:
            messagebox.showinfo("No files", "No files in list.")
            return
        
        total = len(files)
        self.convert_progress['maximum'] = total
        self.convert_progress['value'] = 0
        
        success = 0
        for i, input_path in enumerate(files):
            self.status_label.config(text=f"Converting: {os.path.basename(input_path)}")
            self.update_idletasks()
            
            output_path = os.path.splitext(input_path)[0] + ".dxi"
            try:
                img = load_universal_icon(input_path)
                width, height = img.size
                
                header = struct.pack(HEADER_FORMAT, MAGIC, width, height, 32, 0)
                
                # Convert RGBA -> BGRA for storage
                r, g, b, a = img.split()
                img_bgra = Image.merge("RGBA", (b, g, r, a))
                
                with open(output_path, "wb") as f:
                    f.write(header)
                    f.write(img_bgra.tobytes())
                
                success += 1
            except Exception as e:
                messagebox.showerror("Conversion Failed", f"Failed: {input_path}\n{str(e)}")
            
            self.convert_progress['value'] = i + 1
            self.update_idletasks()
        
        self.status_label.config(text=f"Done: {success}/{total} converted.")
        messagebox.showinfo("Conversion Complete", f"{success} of {total} files converted successfully.")
        self.clear_convert_list()
    
    # ==========================================
    # TAB 2: PREVIEW & INSPECTOR (Gallery)
    # ==========================================
    def _setup_preview_tab(self):
        # Main container with two columns
        main_frame = ttk.Frame(self.tab_preview)
        main_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        # Left sidebar: gallery list + info
        left_panel = ttk.Frame(main_frame, width=250)
        left_panel.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 10))
        left_panel.pack_propagate(False)
        
        # Gallery listbox
        ttk.Label(left_panel, text="Loaded .dxi Files", font=("Helvetica", 10, "bold")).pack(pady=(0,5))
        list_frame = ttk.Frame(left_panel)
        list_frame.pack(fill=tk.BOTH, expand=True)
        self.preview_listbox = tk.Listbox(list_frame, selectmode=tk.SINGLE, height=10)
        scrollbar = ttk.Scrollbar(list_frame, orient=tk.VERTICAL, command=self.preview_listbox.yview)
        self.preview_listbox.config(yscrollcommand=scrollbar.set)
        self.preview_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.preview_listbox.bind('<<ListboxSelect>>', self.on_preview_select)
        
        # Buttons to manage gallery
        btn_frame = ttk.Frame(left_panel)
        btn_frame.pack(fill=tk.X, pady=5)
        ttk.Button(btn_frame, text="➕ Add .dxi Files", command=self.add_dxi_files).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_frame, text="🗑️ Remove Selected", command=self.remove_selected_preview).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_frame, text="🧹 Clear All", command=self.clear_preview_list).pack(side=tk.LEFT, padx=2)
        
        # Info panel
        info_frame = ttk.LabelFrame(left_panel, text="File Information")
        info_frame.pack(fill=tk.BOTH, expand=True, pady=(10,0))
        self.info_text = tk.Text(info_frame, height=12, wrap=tk.WORD, state=tk.DISABLED)
        scroll_info = ttk.Scrollbar(info_frame, orient=tk.VERTICAL, command=self.info_text.yview)
        self.info_text.config(yscrollcommand=scroll_info.set)
        self.info_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scroll_info.pack(side=tk.RIGHT, fill=tk.Y)
        
        # Right panel: canvas for viewing
        canvas_frame = ttk.Frame(main_frame)
        canvas_frame.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True)
        
        self.canvas = tk.Canvas(canvas_frame, bg="gray", highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        
        # Viewport variables
        self.scale = 1.0
        self.offset_x = 0
        self.offset_y = 0
        self.drag_data = {"x": 0, "y": 0}
        self.original_img = None
        self.tk_img = None
        self.img_id = self.canvas.create_image(0, 0, anchor="center")
        
        # Bindings
        self.canvas.drop_target_register(DND_FILES)
        self.canvas.dnd_bind('<<Drop>>', self.handle_preview_drop)
        self.canvas.bind("<Configure>", self.on_canvas_configure)
        
        # Mouse bindings for pan & zoom
        self.canvas.bind("<ButtonPress-1>", self.on_pan_start)
        self.canvas.bind("<B1-Motion>", self.on_pan_motion)
        self.canvas.bind("<MouseWheel>", self.on_zoom)
        self.canvas.bind("<Button-4>", self.on_zoom)
        self.canvas.bind("<Button-5>", self.on_zoom)
        
        # Initial checkerboard
        self.canvas.after(100, self.draw_checkerboard)  # delay to ensure canvas size
    
    def add_dxi_files(self):
        """Add .dxi files to gallery via file dialog."""
        files = filedialog.askopenfilenames(
            title="Select .dxi Files",
            filetypes=[("DXI files", "*.dxi"), ("All files", "*.*")]
        )
        for f in files:
            if f not in self.preview_listbox.get(0, tk.END):
                self.preview_listbox.insert(tk.END, f)
        if self.preview_listbox.size() > 0:
            self.preview_listbox.selection_set(0)
            self.on_preview_select()
    
    def remove_selected_preview(self):
        sel = self.preview_listbox.curselection()
        if sel:
            self.preview_listbox.delete(sel[0])
            if self.preview_listbox.size() > 0:
                self.preview_listbox.selection_set(0)
                self.on_preview_select()
            else:
                self.original_img = None
                self.update_image()
                self.clear_info()
    
    def clear_preview_list(self):
        self.preview_listbox.delete(0, tk.END)
        self.original_img = None
        self.update_image()
        self.clear_info()
    
    def clear_info(self):
        self.info_text.config(state=tk.NORMAL)
        self.info_text.delete(1.0, tk.END)
        self.info_text.config(state=tk.DISABLED)
    
    def on_preview_select(self, event=None):
        sel = self.preview_listbox.curselection()
        if sel:
            file_path = self.preview_listbox.get(sel[0])
            self.load_dxi_file(file_path)
    
    def load_dxi_file(self, file_path):
        """Load a .dxi file and display it."""
        try:
            with open(file_path, "rb") as f:
                header_data = f.read(HEADER_SIZE)
                if len(header_data) < HEADER_SIZE:
                    raise ValueError("File too small.")
                
                magic, w, h, bpp, comp = struct.unpack(HEADER_FORMAT, header_data)
                if magic != MAGIC:
                    raise ValueError("Invalid magic – not a .dxi file.")
                
                raw_data = f.read()
                file_size = os.path.getsize(file_path)
                
                # The file contains BGRA data. We read it as RGBA (wrong interpretation)
                img = Image.frombytes("RGBA", (w, h), raw_data)
                # Now img has channels: R=stored B, G=stored G, B=stored R, A=stored A
                # To recover original RGBA, we swap R and B
                r, g, b, a = img.split()
                # Correct RGBA is (b, g, r, a)
                self.original_img = Image.merge("RGBA", (b, g, r, a))
                
                # Check alpha presence
                alpha_pixels = list(a.getdata())
                has_alpha = min(alpha_pixels) < 255
                
                # Update info panel
                info = (
                    f"File: {os.path.basename(file_path)}\n"
                    f"Path: {file_path}\n"
                    f"Dimensions: {w} × {h} px\n"
                    f"Bit depth: {bpp} bpp\n"
                    f"Compression: {'None' if comp == 0 else 'Unknown'}\n"
                    f"File size: {file_size} bytes\n"
                    f"Has transparency: {'Yes' if has_alpha else 'No'}\n"
                    f"Total pixels: {w*h}\n"
                    f"Data size: {len(raw_data)} bytes"
                )
                self.info_text.config(state=tk.NORMAL)
                self.info_text.delete(1.0, tk.END)
                self.info_text.insert(tk.END, info)
                self.info_text.config(state=tk.DISABLED)
                
                # Auto-scale to fit canvas
                self.auto_scale_and_center()
                self.update_image()
                
        except Exception as e:
            messagebox.showerror("Preview Error", f"Failed to load {file_path}\n{str(e)}")
    
    def auto_scale_and_center(self):
        """Calculate best scale to fit image in canvas and center it."""
        if not self.original_img:
            return
        
        # Get canvas size
        canvas_width = self.canvas.winfo_width()
        canvas_height = self.canvas.winfo_height()
        
        if canvas_width <= 1 or canvas_height <= 1:
            # Canvas not yet drawn; will be called again on configure
            return
        
        img_width = self.original_img.width
        img_height = self.original_img.height
        
        # Determine scale to fit with 20px margin
        margin = 20
        scale_x = (canvas_width - margin) / img_width if img_width > 0 else 1
        scale_y = (canvas_height - margin) / img_height if img_height > 0 else 1
        self.scale = min(scale_x, scale_y)
        if self.scale < 0.1:
            self.scale = 0.1
        
        # Center the image
        self.offset_x = canvas_width // 2
        self.offset_y = canvas_height // 2
    
    def on_canvas_configure(self, event):
        """Handle canvas resize: redraw checkerboard and reposition image."""
        self.draw_checkerboard()
        if self.original_img:
            # Re-center and re-scale when canvas size changes
            self.auto_scale_and_center()
            self.update_image()
    
    def draw_checkerboard(self, event=None):
        self.canvas.delete("checker")
        width = self.canvas.winfo_width()
        height = self.canvas.winfo_height()
        if width <= 1 or height <= 1:
            return
        sq_size = 20
        for y in range(0, height, sq_size):
            for x in range(0, width, sq_size):
                color = "#404040" if ((x // sq_size) + (y // sq_size)) % 2 == 0 else "#202020"
                self.canvas.create_rectangle(x, y, x + sq_size, y + sq_size,
                                             fill=color, outline="", tags="checker")
        self.canvas.tag_lower("checker")
    
    def update_image(self):
        if not self.original_img:
            self.canvas.delete(self.img_id)
            return
        
        new_w = int(self.original_img.width * self.scale)
        new_h = int(self.original_img.height * self.scale)
        if new_w < 1 or new_h < 1:
            return
        
        resized = self.original_img.resize((new_w, new_h), Image.NEAREST)
        self.tk_img = ImageTk.PhotoImage(resized)
        self.canvas.coords(self.img_id, self.offset_x, self.offset_y)
        self.canvas.itemconfig(self.img_id, image=self.tk_img)
        # Ensure the image is above the checkerboard
        self.canvas.tag_raise(self.img_id)
    
    def on_pan_start(self, event):
        self.drag_data["x"] = event.x
        self.drag_data["y"] = event.y
    
    def on_pan_motion(self, event):
        if not self.original_img:
            return
        dx = event.x - self.drag_data["x"]
        dy = event.y - self.drag_data["y"]
        self.offset_x += dx
        self.offset_y += dy
        self.drag_data["x"] = event.x
        self.drag_data["y"] = event.y
        self.canvas.coords(self.img_id, self.offset_x, self.offset_y)
    
    def on_zoom(self, event):
        if not self.original_img:
            return
        zoom_in = False
        if event.num == 4 or (hasattr(event, 'delta') and event.delta > 0):
            zoom_in = True
        elif event.num == 5 or (hasattr(event, 'delta') and event.delta < 0):
            zoom_in = False
        else:
            return
        
        factor = 1.1 if zoom_in else 0.9
        self.scale *= factor
        if self.scale < 0.1:
            self.scale = 0.1
        self.update_image()
    
    def preview_drop_file(self, file_path):
        """Called when a .dxi is dropped on convert tab; adds to preview gallery and selects it."""
        # Add to list if not already there
        if file_path not in self.preview_listbox.get(0, tk.END):
            self.preview_listbox.insert(tk.END, file_path)
        # Select it
        idx = self.preview_listbox.get(0, tk.END).index(file_path)
        self.preview_listbox.selection_clear(0, tk.END)
        self.preview_listbox.selection_set(idx)
        self.on_preview_select()
    
    def handle_preview_drop(self, event):
        files = self.split_dnd_files(event.data)
        for path in files:
            if not os.path.isfile(path):
                continue
            if path.lower().endswith('.dxi'):
                if path not in self.preview_listbox.get(0, tk.END):
                    self.preview_listbox.insert(tk.END, path)
        # Select the first one dropped
        if files and files[0].lower().endswith('.dxi'):
            idx = self.preview_listbox.get(0, tk.END).index(files[0])
            self.preview_listbox.selection_clear(0, tk.END)
            self.preview_listbox.selection_set(idx)
            self.on_preview_select()
    
    # ==========================================
    # Utility: split DND file list
    # ==========================================
    def split_dnd_files(self, data_string):
        """Safely split Tkinter DND file list (handles spaces and braces)."""
        import re
        # Match either {anything} or non-space sequences
        matches = re.findall(r'\{([^}]+)\}|(\S+)', data_string)
        files = []
        for match in matches:
            if match[0]:
                files.append(match[0])
            elif match[1]:
                files.append(match[1])
        if not files:
            files = data_string.split()
        return [f for f in files if os.path.isfile(f)]

if __name__ == "__main__":
    app = DxiStudio()
    app.mainloop()