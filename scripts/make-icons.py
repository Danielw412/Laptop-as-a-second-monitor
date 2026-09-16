"""Generates the application and tray icons in the warm neutral palette. Run once; the .ico files are committed."""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent / "host" / "app" / "resources"
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
DARK = (0x66, 0x49, 0x30, 255)
MID = (0x99, 0x7E, 0x67, 255)
LIGHT = (0xFF, 0xDB, 0xBB, 255)
PAPER = (0xFF, 0xF8, 0xF2, 255)
DOTS = {
    "": None,
    "-ready": (0x3F, 0x9E, 0x6B, 255),
    "-connected": (0x2F, 0x7A, 0xD6, 255),
    "-warning": (0xD9, 0x8B, 0x2B, 255),
    "-error": (0xC9, 0x4B, 0x3E, 255),
    "-off": (0xA0, 0x98, 0x90, 255),
}


def frame(size: int, dot) -> Image.Image:
    scale = 8
    s = size * scale
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    r = s * 0.22
    d.rounded_rectangle((0, 0, s - 1, s - 1), radius=r, fill=DARK)
    # Monitor body and screen.
    x0, y0, x1, y1 = s * 0.16, s * 0.22, s * 0.84, s * 0.64
    d.rounded_rectangle((x0, y0, x1, y1), radius=s * 0.06, fill=LIGHT)
    d.rounded_rectangle((x0 + s * 0.05, y0 + s * 0.05, x1 - s * 0.05, y1 - s * 0.05), radius=s * 0.03, fill=PAPER)
    # Stand.
    d.rectangle((s * 0.46, y1, s * 0.54, s * 0.72), fill=LIGHT)
    d.rounded_rectangle((s * 0.32, s * 0.72, s * 0.68, s * 0.78), radius=s * 0.02, fill=LIGHT)
    # A small second "screen" suggesting the remote laptop.
    d.rounded_rectangle((s * 0.58, s * 0.40, x1 - s * 0.02, y1 + s * 0.02), radius=s * 0.03, fill=MID)
    if dot:
        cx, cy, rr = s * 0.80, s * 0.80, s * 0.14
        d.ellipse((cx - rr - s * 0.03, cy - rr - s * 0.03, cx + rr + s * 0.03, cy + rr + s * 0.03), fill=DARK)
        d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), fill=dot)
    return im.resize((size, size), Image.LANCZOS)


def main() -> None:
    ROOT.mkdir(parents=True, exist_ok=True)
    for suffix, dot in DOTS.items():
        frames = [frame(size, dot) for size in SIZES]
        path = ROOT / f"LaptopMonitor{suffix}.ico"
        frames[-1].save(path, format="ICO", sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
        print("wrote", path)


if __name__ == "__main__":
    main()
