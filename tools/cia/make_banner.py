"""Makes banner.png (256x128) from icon.png for the .cia HOME Menu banner, and a short silent
banner.wav. Replace banner.png (and banner.wav) in the project folder to use your own."""
import os
import struct
import wave

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def font(size):
    for name in ("arialbd.ttf", "segoeuib.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)
        except OSError:
            pass
    return ImageFont.load_default()


def make_png(path):
    w, h = 256, 128
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):  # dark blue vertical gradient
        t = y / (h - 1)
        c = (int(18 + 20 * t), int(28 + 30 * t), int(60 + 70 * t), 255)
        for x in range(w):
            px[x, y] = c
    icon = Image.open(os.path.join(ROOT, "icon.png")).convert("RGBA").resize((96, 96), Image.NEAREST)
    img.alpha_composite(icon, (10, 16))
    d = ImageDraw.Draw(img)
    f1, f2 = font(22), font(13)
    d.text((116, 30), "Awakening", font=f1, fill=(255, 255, 255, 255))
    d.text((116, 56), "Save Editor", font=f1, fill=(255, 255, 255, 255))
    d.text((116, 88), "by Noble Zero", font=f2, fill=(190, 205, 235, 255))
    img.save(path)


def make_wav(path):
    rate = 22050
    with wave.open(path, "wb") as wv:
        wv.setnchannels(1)
        wv.setsampwidth(2)
        wv.setframerate(rate)
        wv.writeframes(struct.pack("<h", 0) * (rate // 2))  # half a second of silence


if __name__ == "__main__":
    png = os.path.join(ROOT, "banner.png")
    wav = os.path.join(ROOT, "banner.wav")
    if not os.path.exists(png):
        make_png(png)
        print("made", png)
    if not os.path.exists(wav):
        make_wav(wav)
        print("made", wav)
