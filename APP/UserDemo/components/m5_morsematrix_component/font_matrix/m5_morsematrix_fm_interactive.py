"""Interactive font matrix generator with real-time preview.

This GUI tool allows you to:
- Preview individual characters in real-time
- Adjust rendering parameters (scale, threshold, padding, etc.)
- Compare different fonts
- Export to C++ files when satisfied

Requirements:
    pip install Pillow tkinter
"""

from __future__ import annotations

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import math
from pathlib import Path
from typing import Optional

try:
    from PIL import Image, ImageDraw, ImageFont, ImageTk, ImageFilter, ImageOps
except ImportError:
    raise SystemExit("Pillow is required. Install it with 'pip install Pillow'")

# Import the generation logic from the main toolbox
import sys
sys.path.insert(0, str(Path(__file__).parent))


class FontMatrixPreview(tk.Tk):
    """Interactive font matrix generator with live preview."""
    
    def __init__(self):
        super().__init__()
        
        self.title("M5 MorseMatrix Font Generator - Interactive Preview")
        self.geometry("1200x800")
        
        # Parameters
        self.width = tk.IntVar(value=8)                          # Output matrix width
        self.height = tk.IntVar(value=8)                         # Output matrix height
        self.scale_factor = tk.IntVar(value=16)                  # High-res rendering scale
        self.threshold = tk.IntVar(value=128)                    # Fixed threshold (UNUSED - kept for UI compatibility)
        self.padding = tk.IntVar(value=1)                        # Padding around glyph bounding box
        self.pixel_threshold = tk.IntVar(value=20)               # Threshold for detecting glyph bounds
        self.adaptive_percentile = tk.IntVar(value=33)           # Percentile for adaptive thresholding
        self.min_adaptive_threshold = tk.IntVar(value=80)        # Minimum threshold value
        self.edge_enhance = tk.DoubleVar(value=0.0)              # Edge enhancement strength
        self.morphology_iterations = tk.IntVar(value=0)          # Morphological filter iterations
        self.resample_method = tk.StringVar(value="LANCZOS")     # Resampling algorithm
        
        self.current_char = tk.StringVar(value="A")
        self.font_path = tk.StringVar(value="")
        self.font_size_multiplier = tk.DoubleVar(value=1.0)
        
        self.font_obj: Optional[ImageFont.FreeTypeFont] = None
        self.preview_bitmap = None
        
        self._setup_ui()
        self._load_default_font()
        self._update_preview()
    
    def _setup_ui(self):
        """Setup the user interface."""
        
        # Main container
        main_frame = ttk.Frame(self)
        main_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        # Left panel: Controls
        left_panel = ttk.Frame(main_frame, width=400)
        left_panel.pack(side=tk.LEFT, fill=tk.BOTH, padx=(0, 10))
        
        # Right panel: Preview
        right_panel = ttk.Frame(main_frame)
        right_panel.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True)
        
        self._setup_controls(left_panel)
        self._setup_preview(right_panel)
    
    def _setup_controls(self, parent):
        """Setup control panel."""
        
        # Character input
        char_frame = ttk.LabelFrame(parent, text="Character", padding=10)
        char_frame.pack(fill=tk.X, pady=(0, 10))
        
        ttk.Label(char_frame, text="Character:").grid(row=0, column=0, sticky=tk.W)
        char_entry = ttk.Entry(char_frame, textvariable=self.current_char, width=10, font=('Arial', 16))
        char_entry.grid(row=0, column=1, sticky=tk.EW, padx=(10, 0))
        char_entry.bind('<KeyRelease>', lambda e: self._update_preview())
        
        # Quick character buttons
        quick_frame = ttk.Frame(char_frame)
        quick_frame.grid(row=1, column=0, columnspan=2, pady=(10, 0))
        
        quick_chars = ['M', '5', 'S', 'T', 'A', 'C', 'K', 'O']
        for i, ch in enumerate(quick_chars):
            btn = ttk.Button(quick_frame, text=ch, width=3,
                           command=lambda c=ch: self._set_char(c))
            btn.grid(row=0, column=i, padx=2)
        
        # Font selection
        font_frame = ttk.LabelFrame(parent, text="Font", padding=10)
        font_frame.pack(fill=tk.X, pady=(0, 10))
        
        ttk.Button(font_frame, text="Select Font File",
                  command=self._select_font).pack(fill=tk.X, pady=(0, 5))
        
        ttk.Label(font_frame, textvariable=self.font_path,
                 wraplength=350, foreground='blue').pack(fill=tk.X)
        
        ttk.Label(font_frame, text="Font Size Multiplier:").pack(anchor=tk.W, pady=(10, 0))
        ttk.Scale(font_frame, from_=0.5, to=16.0, variable=self.font_size_multiplier,
                 orient=tk.HORIZONTAL, command=lambda v: self._update_preview()).pack(fill=tk.X)
        ttk.Label(font_frame, textvariable=self.font_size_multiplier).pack(anchor=tk.E)
        
        # Matrix dimensions
        dim_frame = ttk.LabelFrame(parent, text="Matrix Dimensions", padding=10)
        dim_frame.pack(fill=tk.X, pady=(0, 10))
        
        ttk.Label(dim_frame, text="Width:").grid(row=0, column=0, sticky=tk.W)
        ttk.Spinbox(dim_frame, from_=4, to=32, textvariable=self.width,
                   command=self._update_preview, width=10).grid(row=0, column=1, sticky=tk.EW, padx=(10, 0))
        
        ttk.Label(dim_frame, text="Height:").grid(row=1, column=0, sticky=tk.W, pady=(5, 0))
        ttk.Spinbox(dim_frame, from_=4, to=32, textvariable=self.height,
                   command=self._update_preview, width=10).grid(row=1, column=1, sticky=tk.EW, padx=(10, 0), pady=(5, 0))
        
        # Rendering parameters
        render_frame = ttk.LabelFrame(parent, text="Rendering Parameters", padding=10)
        render_frame.pack(fill=tk.X, pady=(0, 10))
        
        self._add_slider(render_frame, "Scale Factor:", self.scale_factor, 4, 32, 0)
        self._add_slider(render_frame, "Padding:", self.padding, 0, 5, 1)
        self._add_slider(render_frame, "Pixel Threshold:", self.pixel_threshold, 0, 100, 2)
        self._add_slider(render_frame, "Threshold:", self.threshold, 0, 255, 3)
        self._add_slider(render_frame, "Adaptive %ile:", self.adaptive_percentile, 0, 100, 4)
        self._add_slider(render_frame, "Min Adaptive:", self.min_adaptive_threshold, 0, 255, 5)
        self._add_slider(render_frame, "Edge Enhance:", self.edge_enhance, 0.0, 3.0, 6)
        self._add_slider(render_frame, "Morphology:", self.morphology_iterations, 0, 5, 7)
        
        # Resampling method
        ttk.Label(render_frame, text="Resample Method:").grid(row=8, column=0, sticky=tk.W, pady=(5, 0))
        method_combo = ttk.Combobox(render_frame, textvariable=self.resample_method,
                                   values=["NEAREST", "BOX", "BILINEAR", "HAMMING", "BICUBIC", "LANCZOS"],
                                   state='readonly', width=10)
        method_combo.grid(row=8, column=1, sticky=tk.EW, padx=(10, 0), pady=(5, 0))
        method_combo.bind('<<ComboboxSelected>>', lambda e: self._update_preview())
        
        # Action buttons
        action_frame = ttk.Frame(parent)
        action_frame.pack(fill=tk.X, pady=(10, 0))
        
        ttk.Button(action_frame, text="Update Preview",
                  command=self._update_preview).pack(fill=tk.X, pady=(0, 5))
        
        ttk.Button(action_frame, text="Generate Font File",
                  command=self._generate_font_file).pack(fill=tk.X, pady=(0, 5))
        
        ttk.Button(action_frame, text="Copy C Array",
                  command=self._copy_c_array).pack(fill=tk.X)
    
    def _add_slider(self, parent, label, variable, from_, to, row):
        """Add a labeled slider."""
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky=tk.W, pady=(5, 0))
        
        slider_frame = ttk.Frame(parent)
        slider_frame.grid(row=row, column=1, sticky=tk.EW, padx=(10, 0), pady=(5, 0))
        
        ttk.Scale(slider_frame, from_=from_, to=to, variable=variable,
                 orient=tk.HORIZONTAL,
                 command=lambda v: self._update_preview()).pack(side=tk.LEFT, fill=tk.X, expand=True)
        
        ttk.Label(slider_frame, textvariable=variable, width=5).pack(side=tk.RIGHT)
    
    def _setup_preview(self, parent):
        """Setup preview panel."""
        
        # Preview canvas
        preview_frame = ttk.LabelFrame(parent, text="Preview", padding=10)
        preview_frame.pack(fill=tk.BOTH, expand=True)
        
        # Left: High-res original render
        left_frame = ttk.Frame(preview_frame)
        left_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 5))
        
        ttk.Label(left_frame, text="Original Render", font=('Arial', 10, 'bold')).pack(anchor=tk.W)
        
        self.original_canvas = tk.Canvas(left_frame, bg='white', width=250, height=400)
        self.original_canvas.pack(fill=tk.BOTH, expand=True, pady=(5, 0))
        
        # Middle: Grayscale preview (same scale as matrix)
        middle_frame = ttk.Frame(preview_frame)
        middle_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 5))
        
        ttk.Label(middle_frame, text="Grayscale (8x8)", font=('Arial', 10, 'bold')).pack(anchor=tk.W)
        
        self.hires_canvas = tk.Canvas(middle_frame, bg='#f5f5f5', width=250, height=400)
        self.hires_canvas.pack(fill=tk.BOTH, expand=True, pady=(5, 0))
        
        # Right: Matrix preview
        right_frame = ttk.Frame(preview_frame)
        right_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        
        ttk.Label(right_frame, text="Matrix (1-bit)", font=('Arial', 10, 'bold')).pack(anchor=tk.W)
        
        self.matrix_canvas = tk.Canvas(right_frame, bg='#f0f0f0', width=250, height=400)
        self.matrix_canvas.pack(fill=tk.BOTH, expand=True, pady=(5, 0))
        
        # Hex dump (below all previews)
        hex_frame = ttk.Frame(parent)
        hex_frame.pack(fill=tk.X, pady=(10, 0))
        
        ttk.Label(hex_frame, text="Hex Data:", font=('Arial', 10, 'bold')).pack(anchor=tk.W)
        
        self.hex_text = tk.Text(hex_frame, height=8, font=('Courier', 9))
        self.hex_text.pack(fill=tk.X, pady=(5, 0))
        
        hex_scroll = ttk.Scrollbar(hex_frame, command=self.hex_text.yview)
        hex_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.hex_text.config(yscrollcommand=hex_scroll.set)
    
    def _set_char(self, char):
        """Set current character and update preview."""
        self.current_char.set(char)
        self._update_preview()
    
    def _load_default_font(self):
        """Load default font."""
        fonts_dir = Path(Path.home().drive or "C:") / "Windows" / "Fonts"
        
        # Try common monospace fonts
        for font_name in ["consola.ttf", "cour.ttf", "arial.ttf"]:
            font_file = fonts_dir / font_name
            if font_file.exists():
                self.font_path.set(str(font_file))
                self._load_font()
                return
    
    def _select_font(self):
        """Open font file dialog."""
        fonts_dir = Path(Path.home().drive or "C:") / "Windows" / "Fonts"
        
        filename = filedialog.askopenfilename(
            title="Select Font File",
            initialdir=str(fonts_dir) if fonts_dir.exists() else None,
            filetypes=[
                ("TrueType Fonts", "*.ttf"),
                ("OpenType Fonts", "*.otf"),
                ("TrueType Collections", "*.ttc"),
                ("All Files", "*.*")
            ]
        )
        
        if filename:
            self.font_path.set(filename)
            self._load_font()
            self._update_preview()
    
    def _load_font(self):
        """Load font from current path."""
        if not self.font_path.get():
            return
        
        try:
            font_size = int(self.height.get() * self.font_size_multiplier.get())
            self.font_obj = ImageFont.truetype(self.font_path.get(), size=font_size)
        except Exception as e:
            messagebox.showerror("Font Error", f"Failed to load font: {e}")
    
    def _render_glyph(self):
        """Render current glyph with current parameters."""
        if not self.font_obj:
            return None
        
        char = self.current_char.get()
        if not char:
            return None
        
        char = char[0]  # Take first character only
        
        width = self.width.get()
        height = self.height.get()
        scale_factor = self.scale_factor.get()
        # Note: self.threshold is unused in current implementation (adaptive threshold is used instead)
        padding = self.padding.get()
        pixel_threshold = self.pixel_threshold.get()  # Used for bounding box detection only
        
        # 1. Render at high resolution
        hires_size = max(width, height) * scale_factor
        hires_canvas = Image.new("L", (hires_size, hires_size), color=0)
        hires_draw = ImageDraw.Draw(hires_canvas)
        
        # Get bounding box
        bbox = hires_draw.textbbox((hires_size // 4, hires_size // 4), char, font=self.font_obj)
        
        if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            return None
        
        # Center the character
        center_x = hires_size // 2
        center_y = hires_size // 2
        char_width = bbox[2] - bbox[0]
        char_height = bbox[3] - bbox[1]
        
        draw_x = center_x - char_width // 2 - bbox[0]
        draw_y = center_y - char_height // 2 - bbox[1]
        hires_draw.text((draw_x, draw_y), char, fill=255, font=self.font_obj)
        
        # 2. Find tight bounding box
        pixels = hires_canvas.load()
        min_x, min_y = hires_size, hires_size
        max_x, max_y = -1, -1
        
        for y in range(hires_size):
            for x in range(hires_size):
                if pixels[x, y] > pixel_threshold:
                    min_x = min(min_x, x)
                    min_y = min(min_y, y)
                    max_x = max(max_x, x)
                    max_y = max(max_y, y)
        
        if max_x < 0:
            return None
        
        # 3. Crop with padding
        crop_x1 = max(0, min_x - padding)
        crop_y1 = max(0, min_y - padding)
        crop_x2 = min(hires_size, max_x + 1 + padding)
        crop_y2 = min(hires_size, max_y + 1 + padding)
        
        char_image = hires_canvas.crop((crop_x1, crop_y1, crop_x2, crop_y2))
        
        # Store high-res for preview
        self.hires_image = char_image.copy()
        
        # 4. Apply edge enhancement if requested
        edge_enhance = self.edge_enhance.get()
        if edge_enhance > 0:
            # Sharpen the image to preserve edges
            from PIL import ImageFilter
            char_image = char_image.filter(ImageFilter.UnsharpMask(radius=2, percent=int(edge_enhance * 100), threshold=3))
        
        # 5. Scale to target size with selected resampling method
        resample_map = {
            "NEAREST": Image.Resampling.NEAREST,
            "BOX": Image.Resampling.BOX,
            "BILINEAR": Image.Resampling.BILINEAR,
            "HAMMING": Image.Resampling.HAMMING,
            "BICUBIC": Image.Resampling.BICUBIC,
            "LANCZOS": Image.Resampling.LANCZOS,
        }
        resample_method = resample_map.get(self.resample_method.get(), Image.Resampling.LANCZOS)
        char_image = char_image.resize((width, height), resample_method)
        
        # 6. Apply morphological operations for shape simplification
        morph_iterations = self.morphology_iterations.get()
        if morph_iterations > 0:
            # Convert to binary first
            temp_pixels = char_image.load()
            binary = Image.new('L', (width, height), color=0)
            binary_pixels = binary.load()
            
            # Quick threshold
            for y in range(height):
                for x in range(width):
                    binary_pixels[x, y] = 255 if temp_pixels[x, y] > 128 else 0
            
            # Apply erosion then dilation (opening) to remove noise
            for _ in range(morph_iterations):
                binary = binary.filter(ImageFilter.MinFilter(3))  # Erosion
            for _ in range(morph_iterations):
                binary = binary.filter(ImageFilter.MaxFilter(3))  # Dilation
            
            char_image = binary
        
        # 7. Convert to bitmap with adaptive threshold
        stride = math.ceil(width / 8)
        bitmap = []
        final_pixels = char_image.load()
        
        # Calculate adaptive threshold
        pixel_values = [final_pixels[x, y] for y in range(height) for x in range(width) if final_pixels[x, y] > 0]
        if pixel_values:
            pixel_values.sort()
            percentile = self.adaptive_percentile.get()
            # Fix: Correctly calculate percentile index
            if percentile >= 100:
                percentile_idx = len(pixel_values) - 1
            else:
                percentile_idx = min(int(len(pixel_values) * percentile / 100.0), len(pixel_values) - 1)
            adaptive_threshold = pixel_values[percentile_idx]
            adaptive_threshold = max(adaptive_threshold, self.min_adaptive_threshold.get())
        else:
            # Fix: Use min_adaptive_threshold instead of threshold when no pixels
            adaptive_threshold = self.min_adaptive_threshold.get()
        
        for y in range(height):
            for byte_index in range(stride):
                value = 0
                for bit in range(8):
                    x = byte_index * 8 + bit
                    if x >= width:
                        continue
                    if final_pixels[x, y] >= adaptive_threshold:
                        value |= 1 << (7 - bit)
                bitmap.append(value)
        
        return {
            'char': char,
            'width': width,
            'height': height,
            'stride': stride,
            'bitmap': bitmap,
            'grayscale': char_image,
            'adaptive_threshold': adaptive_threshold
        }
    
    def _update_preview(self):
        """Update preview displays."""
        # Reload font if size changed
        self._load_font()
        
        glyph = self._render_glyph()
        if not glyph:
            return
        
        self.preview_bitmap = glyph
        
        # Update all three previews
        self._draw_original_preview()
        self._draw_hires_preview()
        self._draw_matrix_preview()
        
        # Update hex dump
        self._update_hex_dump()
    
    def _draw_original_preview(self):
        """Draw original high-resolution render."""
        if not hasattr(self, 'hires_image'):
            return
        
        canvas_width = self.original_canvas.winfo_width()
        canvas_height = self.original_canvas.winfo_height()
        
        if canvas_width <= 1:
            canvas_width = 250
            canvas_height = 400
        
        # Scale image to fit canvas
        img_width, img_height = self.hires_image.size
        scale = min(canvas_width / img_width, canvas_height / img_height) * 0.9
        
        new_width = int(img_width * scale)
        new_height = int(img_height * scale)
        
        display_img = self.hires_image.resize((new_width, new_height), Image.Resampling.NEAREST)
        
        # Convert to PhotoImage
        self.original_photo = ImageTk.PhotoImage(display_img)
        
        # Draw on canvas
        self.original_canvas.delete('all')
        x = (canvas_width - new_width) // 2
        y = (canvas_height - new_height) // 2
        self.original_canvas.create_image(x, y, anchor=tk.NW, image=self.original_photo)
    
    def _draw_hires_preview(self):
        """Draw high-resolution preview - shows grayscale version at same scale as matrix."""
        if not self.preview_bitmap or 'grayscale' not in self.preview_bitmap:
            return
        
        canvas_width = self.hires_canvas.winfo_width()
        canvas_height = self.hires_canvas.winfo_height()
        
        if canvas_width <= 1:
            canvas_width = 400
            canvas_height = 400
        
        # Get the grayscale image (same size as matrix)
        gray_img = self.preview_bitmap['grayscale']
        img_width, img_height = gray_img.size
        
        # Calculate same cell size as matrix preview
        cell_size = min(canvas_width // img_width, canvas_height // img_height) - 2
        cell_size = max(cell_size, 10)
        
        # Scale up using NEAREST to show pixels clearly
        display_width = img_width * cell_size
        display_height = img_height * cell_size
        
        display_img = gray_img.resize((display_width, display_height), Image.Resampling.NEAREST)
        
        # Convert to PhotoImage
        self.hires_photo = ImageTk.PhotoImage(display_img)
        
        # Draw on canvas (centered)
        self.hires_canvas.delete('all')
        x = (canvas_width - display_width) // 2
        y = (canvas_height - display_height) // 2
        self.hires_canvas.create_image(x, y, anchor=tk.NW, image=self.hires_photo)
        
        # Draw grid lines to show pixel boundaries
        for i in range(img_width + 1):
            x_pos = x + i * cell_size
            self.hires_canvas.create_line(x_pos, y, x_pos, y + display_height, fill='#cccccc', width=1)
        
        for i in range(img_height + 1):
            y_pos = y + i * cell_size
            self.hires_canvas.create_line(x, y_pos, x + display_width, y_pos, fill='#cccccc', width=1)
    
    def _draw_matrix_preview(self):
        """Draw 1-bit matrix preview."""
        if not self.preview_bitmap:
            return
        
        self.matrix_canvas.delete('all')
        
        canvas_width = self.matrix_canvas.winfo_width()
        canvas_height = self.matrix_canvas.winfo_height()
        
        if canvas_width <= 1:
            canvas_width = 400
            canvas_height = 400
        
        width = self.preview_bitmap['width']
        height = self.preview_bitmap['height']
        
        cell_size = min(canvas_width // width, canvas_height // height) - 2
        cell_size = max(cell_size, 10)
        
        offset_x = (canvas_width - width * (cell_size + 2)) // 2
        offset_y = (canvas_height - height * (cell_size + 2)) // 2
        
        bitmap = self.preview_bitmap['bitmap']
        stride = self.preview_bitmap['stride']
        
        for y in range(height):
            for x in range(width):
                byte_idx = y * stride + x // 8
                bit_idx = 7 - (x % 8)
                
                is_on = (bitmap[byte_idx] & (1 << bit_idx)) != 0
                
                x1 = offset_x + x * (cell_size + 2)
                y1 = offset_y + y * (cell_size + 2)
                x2 = x1 + cell_size
                y2 = y1 + cell_size
                
                color = '#000000' if is_on else '#ffffff'
                outline = '#cccccc'
                
                self.matrix_canvas.create_rectangle(x1, y1, x2, y2,
                                                   fill=color, outline=outline)
    
    def _update_hex_dump(self):
        """Update hex dump text."""
        if not self.preview_bitmap:
            return
        
        self.hex_text.delete('1.0', tk.END)
        
        bitmap = self.preview_bitmap['bitmap']
        stride = self.preview_bitmap['stride']
        height = self.preview_bitmap['height']
        
        # Show as 2D array
        output = f"// Character: '{self.preview_bitmap['char']}'\n"
        output += f"// Size: {self.preview_bitmap['width']}x{self.preview_bitmap['height']}, Stride: {stride}\n"
        output += f"// Adaptive Threshold: {self.preview_bitmap['adaptive_threshold']}\n\n"
        
        output += "uint8_t bitmap[" + str(height) + "][" + str(stride) + "] = {\n"
        
        for y in range(height):
            row_bytes = bitmap[y * stride : (y + 1) * stride]
            hex_str = ", ".join(f"0x{b:02X}" for b in row_bytes)
            
            # Add visual representation
            bits = ''.join('X' if (b & (1 << (7 - j))) else '.' 
                          for b in row_bytes for j in range(8))[:self.preview_bitmap['width']]
            
            output += f"    {{ {hex_str} }},  // {bits}\n"
        
        output += "};\n"
        
        self.hex_text.insert('1.0', output)
    
    def _copy_c_array(self):
        """Copy C array to clipboard."""
        if not self.preview_bitmap:
            messagebox.showwarning("No Data", "Please generate a preview first.")
            return
        
        text = self.hex_text.get('1.0', tk.END)
        self.clipboard_clear()
        self.clipboard_append(text)
        messagebox.showinfo("Copied", "C array copied to clipboard!")
    
    def _generate_font_file(self):
        """Generate full font file using current parameters."""
        # Ask for character list file
        char_file = filedialog.askopenfilename(
            title="Select Character List File",
            initialdir=str(Path(__file__).parent),
            filetypes=[("Text Files", "*.txt"), ("All Files", "*.*")]
        )
        
        if not char_file:
            return
        
        # Ask for output file
        output_file = filedialog.asksaveasfilename(
            title="Save Font File",
            initialdir=str(Path(__file__).parent),
            defaultextension=".cpp",
            filetypes=[("C++ Source", "*.cpp"), ("All Files", "*.*")]
        )
        
        if not output_file:
            return
        
        # Generate using m5_morsematrix_fm_toolbox.py with current parameters
        try:
            import subprocess
            
            output_path = Path(output_file)
            header_path = output_path.with_suffix('.h')
            
            # Calculate font size from font_size_multiplier
            font_size = int(self.height.get() * self.font_size_multiplier.get())
            
            cmd = [
                'python',
                str(Path(__file__).parent / 'm5_morsematrix_fm_toolbox.py'),
                '--width', str(self.width.get()),
                '--height', str(self.height.get()),
                '--font-size', str(font_size),
                '--scale-factor', str(self.scale_factor.get()),
                '--padding', str(self.padding.get()),
                '--pixel-threshold', str(self.pixel_threshold.get()),
                '--adaptive-percentile', str(self.adaptive_percentile.get()),
                '--min-adaptive-threshold', str(self.min_adaptive_threshold.get()),
                '--threshold', str(self.threshold.get()),
                '--ascii-font', self.font_path.get(),
                '--ascii-file', char_file,
                '--output-cpp', str(output_path),
                '--output-h', str(header_path),
            ]
            
            result = subprocess.run(cmd, capture_output=True, text=True)
            
            if result.returncode == 0:
                messagebox.showinfo("Success", 
                                  f"Font files generated:\n{output_path}\n{header_path}")
            else:
                messagebox.showerror("Error", f"Generation failed:\n{result.stderr}")
        
        except Exception as e:
            messagebox.showerror("Error", f"Failed to generate font file:\n{e}")


def main():
    """Run the interactive font generator."""
    app = FontMatrixPreview()
    app.mainloop()


if __name__ == "__main__":
    main()
