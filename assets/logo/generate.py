"""为 Dream Minecraft Launcher (DML) 生成图标与横版标志。

风格：圆角应用方块 + 粗体字母组合 + 渐变（对齐 PCL2 一类启动器图标），
配色沿用项目主题（深色底 + Minecraft 草绿）。

输出：
  dml-logo-1024.png / -512 / -256   方形图标（圆角外透明）
  dml.ico                           多尺寸 Windows 图标
  dml-lockup.png                    图标 + 文字横版标志（深色底，通用）
"""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
SS = 4
S = 1024 * SS
RADIUS = int(S * 0.235)

FONT_BLACK = r"C:\Windows\Fonts\seguibl.ttf"
FONT_MED = r"C:\Windows\Fonts\segoeuib.ttf"

GREEN = (74, 222, 128)
GREEN_LO = (30, 128, 78)
WHITE = (248, 255, 251)
BG_A = (32, 42, 52)
BG_B = (8, 10, 13)


def diag_grad(size, c0, c1):
    w, h = size
    n = w + h
    strip = Image.new("RGB", (1, n))
    for i in range(n):
        t = i / (n - 1)
        strip.putpixel((0, i), tuple(int(c0[j] + (c1[j] - c0[j]) * t) for j in range(3)))
    px = strip.load()
    out = Image.new("RGB", (w, h))
    op = out.load()
    for y in range(h):
        for x in range(w):
            op[x, y] = px[0, min(n - 1, int((x + y) * n / (w + h)))]
    return out


def vgrad(size, stops):
    w, h = size
    col = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        for i in range(len(stops) - 1):
            p0, c0 = stops[i]
            p1, c1 = stops[i + 1]
            if p0 <= t <= p1:
                k = (t - p0) / max(1e-6, p1 - p0)
                col.putpixel((0, y), tuple(int(c0[j] + (c1[j] - c0[j]) * k) for j in range(3)))
                break
    return col.resize((w, h), Image.BILINEAR)


def rounded_mask(size, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size[0] - 1, size[1] - 1], radius=radius, fill=255)
    return m


def measure(font, text, tracking):
    total = 0
    for ch in text:
        w = font.getbbox(ch)[2] - font.getbbox(ch)[0]
        total += w + tracking * font.size
    return total - tracking * font.size if text else 0


def fit_font(font_path, text, target_w, tracking=0.0):
    lo, hi, best = 10, int(target_w), 10
    while lo <= hi:
        mid = (lo + hi) // 2
        f = ImageFont.truetype(font_path, mid)
        if measure(f, text, tracking) <= target_w:
            best, lo = mid, mid + 1
        else:
            hi = mid - 1
    return ImageFont.truetype(font_path, best)


def draw_tracked(draw, xy, text, font, tracking, fill=255):
    x, y = xy
    for ch in text:
        bb = font.getbbox(ch)
        draw.text((x - bb[0], y), ch, font=font, fill=fill)
        x += (bb[2] - bb[0]) + tracking * font.size


def glyph_mask(text, tracking, target_w):
    font = fit_font(FONT_BLACK, text, target_w, tracking)
    tmp = Image.new("L", (S, S), 0)
    w = measure(font, text, tracking)
    asc, desc = font.getmetrics()
    draw_tracked(ImageDraw.Draw(tmp), ((S - w) / 2, (S - (asc + desc)) / 2), text, font, tracking)
    bb = tmp.getbbox()
    canvas = Image.new("L", (S, S), 0)
    canvas.paste(tmp.crop(bb), (int((S - (bb[2] - bb[0])) / 2),
                                int((S - (bb[3] - bb[1])) / 2 - S * 0.010)))
    return canvas, font


def render_icon():
    base = diag_grad((S, S), BG_A, BG_B)

    # 左上冷光
    sheen = Image.new("L", (S, S), 0)
    r = int(S * 0.75)
    ImageDraw.Draw(sheen).ellipse([-r // 2, -r // 2, r, r], fill=255)
    sheen = sheen.filter(ImageFilter.GaussianBlur(S * 0.12)).point(lambda v: int(v * 0.10))
    base = Image.composite(Image.new("RGB", (S, S), (120, 190, 220)), base, sheen)

    # 极淡网格（方块暗示）
    grid = Image.new("L", (S, S), 0)
    gd = ImageDraw.Draw(grid)
    step = S / 8.0
    for i in range(1, 8):
        gd.line([(step * i, 0), (step * i, S)], fill=9, width=SS)
        gd.line([(0, step * i), (S, step * i)], fill=9, width=SS)
    base = Image.composite(Image.new("RGB", (S, S), (255, 255, 255)), base, grid)

    text_mask, font = glyph_mask("DML", tracking=-0.05, target_w=int(S * 0.76))

    # 轻投影
    sh = Image.new("L", (S, S), 0)
    sh.paste(text_mask, (0, int(S * 0.010)))
    sh = sh.filter(ImageFilter.GaussianBlur(S * 0.007)).point(lambda v: int(v * 0.55))
    base = Image.composite(Image.new("RGB", (S, S), (3, 14, 8)), base, sh)

    fill = vgrad((S, S), [(0.0, WHITE), (0.35, (170, 243, 201)), (0.58, GREEN), (1.0, GREEN_LO)])
    base = Image.composite(fill, base, text_mask)

    # 外框描边
    border = Image.new("L", (S, S), 0)
    ImageDraw.Draw(border).rounded_rectangle(
        [SS * 2, SS * 2, S - SS * 2 - 1, S - SS * 2 - 1], radius=RADIUS - SS * 2,
        outline=26, width=int(1.5 * SS))
    base = Image.composite(Image.new("RGB", (S, S), (255, 255, 255)), base, border)

    # 圆角外透明
    icon = base.convert("RGBA")
    icon.putalpha(rounded_mask((S, S), RADIUS))
    icon = icon.resize((1024, 1024), Image.LANCZOS)
    icon.save(os.path.join(OUT, "dml-logo-1024.png"))
    for size in (512, 256):
        icon.resize((size, size), Image.LANCZOS).save(os.path.join(OUT, f"dml-logo-{size}.png"))
    icon.save(os.path.join(OUT, "dml.ico"),
              sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("icon ok, font", font.size)


def make_lockup():
    f1 = ImageFont.truetype(FONT_BLACK, 106)
    f2 = ImageFont.truetype(FONT_MED, 37)
    text_w = max(measure(f1, "DREAM", 0.015), measure(f2, "MINECRAFT LAUNCHER", 0.235))
    icon_size = 300
    left, gap, right = 70, 62, 78
    W = int(left + icon_size + gap + text_w + right)
    H = 440
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))

    # 深色圆角底板，保证在浅色/深色背景下都能看清
    plate = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    pd = ImageDraw.Draw(plate)
    pd.rounded_rectangle([0, 0, W - 1, H - 1], radius=64, fill=(18, 21, 26, 255))
    g = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(g).ellipse([-260, -300, 760, 520], fill=(74, 222, 128, 26))
    g = g.filter(ImageFilter.GaussianBlur(90))
    plate.alpha_composite(g)
    canvas.alpha_composite(plate)

    icon = Image.open(os.path.join(OUT, "dml-logo-1024.png")).convert("RGBA").resize(
        (icon_size, icon_size), Image.LANCZOS)
    canvas.alpha_composite(icon, (left, (H - icon_size) // 2))

    d = ImageDraw.Draw(canvas)
    x = left + icon_size + gap
    draw_tracked(d, (x, 112), "DREAM", f1, 0.015, fill=(244, 249, 254, 255))
    draw_tracked(d, (x + 4, 252), "MINECRAFT LAUNCHER", f2, 0.235, fill=(122, 214, 158, 255))

    canvas.save(os.path.join(OUT, "dml-lockup.png"))
    print("lockup ok", W, "x", H)


if __name__ == "__main__":
    render_icon()
    make_lockup()
