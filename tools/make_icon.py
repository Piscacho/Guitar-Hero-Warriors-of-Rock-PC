# Genera project/gh6.ico (icono propio del ejecutable y del launcher): pua naranja con un rayo sobre fondo oscuro.
# Uso: python tools/make_icon.py   (requiere Pillow)
import math, os
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # se dibuja grande y se reduce

def pick_polygon(cx, cy, w, h, n=60):
    # Pua: triangulo de lados abombados (ancho arriba, punta abajo); las esquinas se redondean despues
    a = (cx - w / 2, cy - h * 0.42); b = (cx + w / 2, cy - h * 0.42); c = (cx, cy + h / 2)
    def edge(p, q, bulge):
        mx, my = (p[0] + q[0]) / 2, (p[1] + q[1]) / 2
        nx, ny = -(q[1] - p[1]), q[0] - p[0]; L = math.hypot(nx, ny); nx, ny = nx / L, ny / L
        ctrl = (mx - nx * bulge, my - ny * bulge)
        return [((1 - t) ** 2 * p[0] + 2 * (1 - t) * t * ctrl[0] + t * t * q[0], (1 - t) ** 2 * p[1] + 2 * (1 - t) * t * ctrl[1] + t * t * q[1]) for t in [i / n for i in range(n)]]
    return edge(a, b, 90) + edge(b, c, 110) + edge(c, a, 110)

def rounded(mask, r):
    return mask.filter(ImageFilter.GaussianBlur(r)).point(lambda v: 255 if v > 140 else 0)

img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
# fondo: cuadrado redondeado oscuro con borde naranja
d.rounded_rectangle([24, 24, S - 24, S - 24], radius=200, fill=(24, 18, 26, 255), outline=(232, 85, 30, 255), width=28)
# brillo de fuego en la parte baja
glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
gd = ImageDraw.Draw(glow)
gd.ellipse([150, 560, S - 150, S + 200], fill=(232, 85, 30, 120))
glow = glow.filter(ImageFilter.GaussianBlur(90))
mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([52, 52, S - 52, S - 52], radius=180, fill=255)
img.paste(Image.alpha_composite(img, glow), (0, 0), mask)
d = ImageDraw.Draw(img)
# pua con degradado vertical (amarillo -> naranja -> rojo)
pick = Image.new('RGBA', (S, S), (0, 0, 0, 0))
grad = Image.new('RGBA', (S, S))
for y in range(S):
    f = y / S
    if f < 0.5:
        a = f / 0.5; c = (int(255), int(196 - 70 * a), int(64 - 40 * a))
    else:
        a = (f - 0.5) / 0.5; c = (int(255 - 40 * a), int(126 - 80 * a), int(24))
    ImageDraw.Draw(grad).line([(0, y), (S, y)], fill=c + (255,))
pmask = Image.new('L', (S, S), 0)
ImageDraw.Draw(pmask).polygon(pick_polygon(S / 2, S / 2 + 10, 640, 700), fill=255)
pmask = rounded(pmask, 45)
pick.paste(grad, (0, 0), pmask)
# sombra de la pua
shadow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
shadow.paste((0, 0, 0, 170), (0, 0), pmask)
shadow = shadow.filter(ImageFilter.GaussianBlur(24))
img = Image.alpha_composite(img, shadow.transform((S, S), Image.AFFINE, (1, 0, -14, 0, 1, -22)))
img = Image.alpha_composite(img, pick)
# borde de la pua
edge = pmask.filter(ImageFilter.FIND_EDGES).filter(ImageFilter.MaxFilter(13))
img.paste((255, 236, 200, 255), (0, 0), edge)
# rayo blanco
bolt = [(560, 250), (380, 560), (500, 560), (430, 800), (660, 470), (530, 470), (610, 250)]
bl = Image.new('RGBA', (S, S), (0, 0, 0, 0))
ImageDraw.Draw(bl).polygon(bolt, fill=(40, 16, 10, 200))
bl = bl.filter(ImageFilter.GaussianBlur(10))
img = Image.alpha_composite(img, bl)
ImageDraw.Draw(img).polygon([(x - 6, y - 8) for x, y in bolt], fill=(255, 255, 255, 255))

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'project', 'gh6.ico')
sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)]
img.resize((256, 256), Image.LANCZOS).save(out, sizes=sizes)
img.resize((256, 256), Image.LANCZOS).save(os.path.join(os.path.dirname(out), 'gh6_icon_preview.png'))
print('icono:', os.path.normpath(out))
