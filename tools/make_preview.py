# Builds workshop/preview.png (512x512) from a real in-game capture: python tools/make_preview.py
import os
from PIL import Image, ImageDraw, ImageFont, ImageEnhance

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "workshop")
SRC = os.path.join(ROOT, "screenshots", "01_two_players_town.jpg")
OUT = os.path.join(ROOT, "preview.png")
S = 1024
F = r"C:\Windows\Fonts\\"

bg = Image.open(SRC).convert("RGB").crop((590, 140, 1220, 770)).resize((S, S), Image.LANCZOS)
bg = ImageEnhance.Brightness(bg).enhance(0.85)

# darken top and bottom so text pops
shade = Image.new("L", (S, S), 0)
d = ImageDraw.Draw(shade)
for y in range(S):
    a = 0
    if y < 520: a = int(200 * (1 - y / 520) ** 1.2)
    if y > 820: a = max(a, int(220 * ((y - 820) / 204)))
    d.line([(0, y), (S, y)], fill=a)
bg = Image.composite(Image.new("RGB", (S, S), (12, 8, 6)), bg, shade)
img = bg.convert("RGBA")

def text_center(draw, y, txt, font, fill, shadow=(0, 0, 0, 200), off=5, stroke=0, stroke_fill=None):
    w = draw.textlength(txt, font=font)
    x = (S - w) / 2
    if shadow:
        draw.text((x + off, y + off), txt, font=font, fill=shadow)
    draw.text((x, y), txt, font=font, fill=fill, stroke_width=stroke, stroke_fill=stroke_fill)

layer = Image.new("RGBA", (S, S))
d = ImageDraw.Draw(layer)

# small "KENSHI" title
f_k = ImageFont.truetype(F + "impact.ttf", 120)
text_center(d, 30, "KENSHI", f_k, (240, 226, 196, 255), off=6)

# big MULTIPLAYER band
band_y, band_h = 185, 200
band = Image.new("RGBA", (S, band_h), (150, 38, 22, 235))
bd = ImageDraw.Draw(band)
bd.rectangle([0, 0, S, 7], fill=(235, 180, 90, 255))
bd.rectangle([0, band_h - 8, S, band_h], fill=(235, 180, 90, 255))
layer.alpha_composite(band, (0, band_y))
size = 200
while True:
    f_m = ImageFont.truetype(F + "impact.ttf", size)
    if d.textlength("MULTIPLAYER", font=f_m) <= S - 70: break
    size -= 2
bbox = d.textbbox((0, 0), "MULTIPLAYER", font=f_m)
ty = band_y + (band_h - (bbox[3] - bbox[1])) / 2 - bbox[1]
text_center(d, ty, "MULTIPLAYER", f_m, (255, 255, 255, 255), shadow=(40, 5, 0, 220), off=6)

# subtitle
f_s = ImageFont.truetype(F + "impact.ttf", 66)
text_center(d, band_y + band_h + 22, "CO-OP  •  PVP  •  UP TO 8 PLAYERS", f_s,
            (245, 190, 95, 255), off=4, stroke=3, stroke_fill=(20, 10, 5, 255))

# bottom strip
f_b = ImageFont.truetype(F + "segoeuib.ttf", 48)
text_center(d, S - 100, "Join through Steam  ·  Nothing to install", f_b, (240, 230, 210, 255), off=3)

img.alpha_composite(layer)
out = img.convert("RGB").resize((512, 512), Image.LANCZOS)
out.save(OUT, optimize=True)
print("ok")
