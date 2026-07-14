import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from tkinterdnd2 import DND_FILES, TkinterDnD
import cairosvg
import os
import threading

class SVGConverterApp:
    def __init__(self, root):
        self.root = root
        self.root.title("LunaSVG Batch Converter 🎨")
        self.root.geometry("600x500")
        
        self.files_to_convert = []
        self.output_path = tk.StringVar(value=os.getcwd())

        self.setup_ui()

    def setup_ui(self):
        # --- Top Bar: Options ---
        top_bar = tk.Frame(self.root, pady=10)
        top_bar.pack(fill=tk.X)

        tk.Button(top_bar, text="📂 Set Output Path", command=self.set_output_path).pack(side=tk.LEFT, padx=10)
        tk.Label(top_bar, textvariable=self.output_path, fg="blue", wraplength=300).pack(side=tk.LEFT)
        tk.Button(top_bar, text="🧹 Clear List", command=self.clear_list, bg="#ff9999").pack(side=tk.RIGHT, padx=10)

        # --- List Canvas (Listbox with Scrollbar) ---
        list_frame = tk.Frame(self.root, padx=10, pady=5)
        list_frame.pack(fill=tk.BOTH, expand=True)

        tk.Label(list_frame, text="Drag & Drop SVG files below 👇", font=("Arial", 10, "italic")).pack()
        
        self.file_listbox = tk.Listbox(list_frame, selectmode=tk.MULTIPLE, relief="flat", bd=2)
        self.file_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        scrollbar = tk.Scrollbar(list_frame, orient="vertical", command=self.file_listbox.yview)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.file_listbox.config(yscrollcommand=scrollbar.set)

        # Register Drag and Drop
        self.file_listbox.drop_target_register(DND_FILES)
        self.file_listbox.dnd_bind('<<Drop>>', self.handle_drop)

        # --- Status Label ---
        self.status_label = tk.Label(self.root, text="Ready to convert", pady=5)
        self.status_label.pack()

        # --- Progress Bar ---
        self.progress = ttk.Progressbar(self.root, orient=tk.HORIZONTAL, length=400, mode='determinate')
        self.progress.pack(pady=10)

        # --- Start Button ---
        self.start_btn = tk.Button(self.root, text="🚀 START CONVERSION", font=("Arial", 12, "bold"), 
                                   bg="#4CAF50", fg="white", command=self.start_conversion_thread, pady=10)
        self.start_btn.pack(fill=tk.X, padx=20, pady=10)

    # --- Logic ---

    def handle_drop(self, event):
        # Handle curly braces often added by DnD in Linux/Windows paths
        files = self.root.tk.splitlist(event.data)
        for f in files:
            if f.lower().endswith(".svg") and f not in self.files_to_convert:
                self.files_to_convert.append(f)
                self.file_listbox.insert(tk.END, os.path.basename(f))
        self.status_label.config(text=f"Added {len(files)} files.")

    def set_output_path(self):
        path = filedialog.askdirectory()
        if path:
            self.output_path.set(path)

    def clear_list(self):
        self.files_to_convert.clear()
        self.file_listbox.delete(0, tk.END)
        self.progress['value'] = 0
        self.status_label.config(text="List cleared.")

    def start_conversion_thread(self):
        if not self.files_to_convert:
            messagebox.showwarning("Empty List", "Please add some SVG files first!")
            return
        
        self.start_btn.config(state=tk.DISABLED)
        thread = threading.Thread(target=self.run_conversion)
        thread.start()

    def run_conversion(self):
        total = len(self.files_to_convert)
        self.progress['maximum'] = total
        
        output_dir = self.output_path.get()
        
        for i, svg_path in enumerate(self.files_to_convert):
            try:
                filename = os.path.basename(svg_path).replace(".svg", ".png")
                target_path = os.path.join(output_dir, filename)
                
                # Update status
                self.status_label.config(text=f"Converting: {filename}...")
                
                # Conversion logic
                cairosvg.svg2png(url=svg_path, write_to=target_path)
                
                # Update progress
                self.progress['value'] = i + 1
            except Exception as e:
                print(f"Error converting {svg_path}: {e}")
        
        self.status_label.config(text="✨ All files converted successfully!")
        self.start_btn.config(state=tk.NORMAL)
        messagebox.showinfo("Done", f"Converted {total} files to {output_dir}")

if __name__ == "__main__":
    root = TkinterDnD.Tk()
    app = SVGConverterApp(root)
    root.mainloop()