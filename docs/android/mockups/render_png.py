"""Render the 3DS Live Android mockups to PNG with Pillow.

Headless Edge is blocked by the sandbox (mojo channel), so the previews are
drawn directly from the same tokens and geometry as the HTML mockups in this
folder. Design px are multiplied by SCALE for crisp output.
"""

from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw, ImageFont

S = 2  # supersampling factor
W, H = 412, 915
OUT = pathlib.Path(__file__).resolve().parent.parent / "images"
FONT_DIR = pathlib.Path(r"C:\Windows\Fonts")

INK = "#1C2030"
MUTED = "#73798C"
LINE = "#E7E9F2"
CANVAS = "#F7F8FC"
SURFACE = "#FFFFFF"
SURFACE_HIGH = "#F1F3F8"
PRIMARY = "#238E82"
PRIMARY_SOFT = "#E7F1ED"
SECONDARY = "#5268F2"
SUCCESS = "#2CAF7A"
WARNING = "#E19B39"
DANGER = "#D65757"
AMBER = "#E8A33D"
GREEN = "#35C07A"

_font_cache: dict[tuple[int, bool], ImageFont.FreeTypeFont] = {}


def A(color: str, alpha: float) -> tuple[int, int, int, int]:
    """Hex color plus 0..1 alpha as an RGBA tuple for ImageDraw."""
    value = color.lstrip("#")
    r, g, b = (int(value[i:i + 2], 16) for i in (0, 2, 4))
    return (r, g, b, round(alpha * 255))


def font(size: float, bold: bool = False) -> ImageFont.FreeTypeFont:
    key = (round(size * S), bold)
    if key not in _font_cache:
        path = FONT_DIR / ("msyhbd.ttc" if bold else "msyh.ttc")
        _font_cache[key] = ImageFont.truetype(str(path), key[0])
    return _font_cache[key]


def px(v: float) -> int:
    return round(v * S)


class Canvas:
    def __init__(self, bg: str, width: int = W, height: int = H) -> None:
        self.img = Image.new("RGB", (width * S, height * S), bg)
        self.d = ImageDraw.Draw(self.img, "RGBA")

    def card(self, x, y, w, h, r=16, fill=SURFACE, outline=LINE, width=1):
        self.d.rounded_rectangle(
            [px(x), px(y), px(x + w), px(y + h)], radius=px(r), fill=fill,
            outline=outline, width=max(1, px(width)),
        )

    def rect(self, x, y, w, h, fill, r=0, outline=None, width=1):
        if r:
            self.d.rounded_rectangle(
                [px(x), px(y), px(x + w), px(y + h)], radius=px(r), fill=fill,
                outline=outline, width=max(1, px(width)),
            )
        else:
            self.d.rectangle([px(x), px(y), px(x + w), px(y + h)], fill=fill,
                             outline=outline, width=max(1, px(width)))

    def text(self, x, y, value, size, color, bold=False, anchor="la"):
        self.d.text((px(x), px(y)), value, font=font(size, bold), fill=color, anchor=anchor)

    def paragraph(self, x, y, lines, size, color, line_height, bold=False):
        for i, line in enumerate(lines):
            self.text(x, y + i * line_height, line, size, color, bold)

    def circle(self, cx, cy, r, fill=None, outline=None, width=1):
        self.d.ellipse([px(cx - r), px(cy - r), px(cx + r), px(cy + r)],
                       fill=fill, outline=outline, width=max(1, px(width)))

    def dot(self, cx, cy, r, color, ring=None):
        if ring:
            self.circle(cx, cy, r + ring[0], fill=ring[1])
        self.circle(cx, cy, r, fill=color)

    def line(self, x1, y1, x2, y2, color, width=1.7, dash=None):
        if not dash:
            self.d.line([px(x1), px(y1), px(x2), px(y2)], fill=color, width=max(1, px(width)))
            return
        import math

        total = math.hypot(x2 - x1, y2 - y1)
        if total == 0:
            return
        ux, uy = (x2 - x1) / total, (y2 - y1) / total
        pos = 0.0
        while pos < total:
            end = min(pos + dash, total)
            self.d.line([px(x1 + ux * pos), px(y1 + uy * pos),
                         px(x1 + ux * end), px(y1 + uy * end)], fill=color, width=max(1, px(width)))
            pos += dash * 2

    def vgradient(self, x, y, w, h, top, bottom):
        """Vertical gradient overlay; top/bottom are (r,g,b,a)."""
        base = self.img.crop((px(x), px(y), px(x + w), px(y + h)))
        grad = Image.new("RGBA", base.size)
        gd = ImageDraw.Draw(grad)
        for row in range(base.size[1]):
            t = row / max(1, base.size[1] - 1)
            color = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(4))
            gd.line([(0, row), (base.size[0], row)], fill=color)
        self.img.paste(Image.alpha_composite(base.convert("RGBA"), grad).convert("RGB"),
                       (px(x), px(y)))

    def dim(self, alpha=0.32):
        self.d.rectangle([0, 0, W * S, H * S], fill=(0, 0, 0, round(alpha * 255)))

    def save(self, name):
        self.img.save(OUT / name, "PNG")
        print(f"wrote {name}")


# ---------------------------------------------------------------------------
# icons

def icon_gear(c, cx, cy, color, r=8.5):
    for dy, knob in ((-r * 0.45, -r * 0.28), (r * 0.42, r * 0.3)):
        c.line(cx - r, cy + dy, cx + r, cy + dy, color, 1.7)
        c.circle(cx + knob, cy + dy, r * 0.34, fill=color)


def icon_moon(c, cx, cy, color, r=8.5):
    c.circle(cx, cy, r, outline=color, width=1.6)
    c.d.pieslice([px(cx - r), px(cy - r), px(cx + r), px(cy + r)], 90, 270, fill=color)


def icon_chevron(c, cx, cy, color, r=5):
    c.line(cx - r * 0.5, cy - r, cx + r * 0.5, cy, color, 1.7)
    c.line(cx + r * 0.5, cy, cx - r * 0.5, cy + r, color, 1.7)


def icon_back(c, cx, cy, color, r=7):
    c.line(cx + r * 0.5, cy - r, cx - r * 0.5, cy, color, 1.9)
    c.line(cx - r * 0.5, cy, cx + r * 0.5, cy + r, color, 1.9)


def icon_lock(c, cx, cy, color, r=7):
    c.rect(cx - r * 0.7, cy - r * 0.05, r * 1.4, r * 0.95, fill=None, r=2.2, outline=color, width=1.6)
    c.d.arc([px(cx - r * 0.5), px(cy - r * 1.15), px(cx + r * 0.5), px(cy + r * 0.25)],
            185, 355, fill=color, width=px(1.6))


def icon_camera(c, cx, cy, color, r=8):
    c.rect(cx - r, cy - r * 0.6, r * 2, r * 1.35, fill=None, r=2.6, outline=color, width=1.6)
    c.circle(cx, cy + r * 0.08, r * 0.34, outline=color, width=1.6)


def icon_more(c, cx, cy, color, r=8):
    for dx in (-r * 0.6, 0, r * 0.6):
        c.circle(cx + dx, cy, 1.4, fill=color)


def icon_stop(c, cx, cy, color, r=15):
    c.rect(cx - r / 2, cy - r / 2, r, r, fill=color, r=4)


def icon_record(c, cx, cy, color, r=6):
    c.circle(cx, cy, r * 0.62, fill=color)


def icon_shot(c, cx, cy, color, r=7.5):
    c.rect(cx - r, cy - r, r * 2, r * 2, fill=None, r=3, outline=color, width=1.6)
    c.line(cx - r * 0.45, cy + r * 0.45, cx - r * 0.1, cy, color, 1.5)
    c.line(cx - r * 0.1, cy, cx + r * 0.15, cy + r * 0.2, color, 1.5)
    c.line(cx + r * 0.15, cy + r * 0.2, cx + r * 0.5, cy - r * 0.15, color, 1.5)


def icon_layout(c, cx, cy, color, r=8):
    c.rect(cx - r, cy - r * 0.8, r * 2, r * 0.66, fill=None, r=2, outline=color, width=1.5)
    c.rect(cx - r, cy + r * 0.1, r * 2, r * 0.55, fill=None, r=2, outline=color, width=1.5)


def icon_wifi_small(c, cx, cy, color, r=7):
    for rr in (r * 0.45, r * 0.8):
        c.d.arc([px(cx - rr), px(cy - rr * 0.8), px(cx + rr), px(cy + rr * 1.2)], 220, 320,
                fill=color, width=px(1.5))
    c.circle(cx, cy + r * 0.45, 1.1, fill=color)


def icon_signal(c, cx, cy, color, r=8):
    import math

    for i, rr in enumerate((r * 0.42, r * 0.72, r)):
        c.d.arc([px(cx - rr), px(cy - rr), px(cx + rr), px(cy + rr)], 225, 315,
                fill=color, width=px(1.6))
    c.circle(cx, cy + r * 0.55, 1.2, fill=color)


def icon_pulse(c, cx, cy, color, r=8):
    c.line(cx - r, cy, cx - r * 0.4, cy, color, 1.7)
    c.line(cx - r * 0.4, cy, cx - r * 0.15, cy - r * 0.6, color, 1.7)
    c.line(cx - r * 0.15, cy - r * 0.6, cx + r * 0.15, cy + r * 0.6, color, 1.7)
    c.line(cx + r * 0.15, cy + r * 0.6, cx + r * 0.4, cy, color, 1.7)
    c.line(cx + r * 0.4, cy, cx + r, cy, color, 1.7)


def icon_battery(c, cx, cy, color):
    c.rect(cx - 7, cy - 4, 12, 8, fill=None, r=2, outline=color, width=1.3)
    c.rect(cx + 5.4, cy - 1.8, 1.6, 3.6, fill=color, r=0.6)


def statusbar(c: Canvas, dark=False):
    color = "#EAF5F2" if dark else INK
    c.text(18, 13, "21:14", 11, color, bold=True, anchor="lm")
    icon_signal(c, 366, 13, color, r=5.4)
    icon_battery(c, 388, 13, color)


# ---------------------------------------------------------------------------
# screens

def connect() -> Canvas:
    c = Canvas(CANVAS)
    statusbar(c)

    # top bar
    c.rect(16, 40, 28, 28, fill=SECONDARY, r=8)
    c.text(30, 54, "3DS", 8, "#FFFFFF", bold=True, anchor="mm")
    c.text(54, 54, "3DS Live", 17, INK, bold=True, anchor="lm")
    icon_moon(c, 332, 54, INK)
    icon_gear(c, 376, 54, INK)

    # status card
    c.card(16, 86, 380, 172)
    c.dot(32, 114, 5, AMBER, ring=(4, A(AMBER, 0.18)))
    c.text(50, 114, "正在寻找掌机", 17, INK, bold=True, anchor="lm")
    c.paragraph(32, 136, ["在掌机打开 3DS Live 后按 R + SELECT，", "两端会自动配对。"], 14, MUTED, 21)
    c.rect(32, 194, 197, 48, fill=PRIMARY, r=14)
    c.text(130, 218, "停止搜索", 14, "#FFFFFF", bold=True, anchor="mm")
    c.rect(239, 194, 157, 48, fill=None, r=14, outline=LINE, width=1)
    c.text(317, 218, "手动 IP…", 14, INK, bold=True, anchor="mm")

    # discovered
    c.text(16, 284, "发现的掌机", 17, INK, bold=True, anchor="lm")
    c.text(396, 284, "2 台", 12, MUTED, anchor="rm")

    for i, (name, meta) in enumerate([
        ("New 3DS · 192.168.1.23", "刚刚 · session 8F2A"),
        ("New 3DS XL · 192.168.1.31", "2 分钟前 · session 3C19"),
    ]):
        y = 302 + i * 82
        c.card(16, y, 380, 72)
        c.rect(32, y + 16, 40, 40, fill=PRIMARY_SOFT, r=12)
        c.text(52, y + 36, "3DS", 9, PRIMARY, bold=True, anchor="mm")
        c.text(84, y + 27, name, 15, INK, bold=True, anchor="lm")
        c.text(84, y + 49, meta, 12, MUTED, anchor="lm")
        icon_chevron(c, 378, y + 36, MUTED)

    # recent
    c.text(16, 500, "最近使用", 17, INK, bold=True, anchor="lm")
    c.card(16, 518, 380, 72)
    c.rect(32, 534, 40, 40, fill=PRIMARY_SOFT, r=12)
    c.text(52, 554, "3DS", 9, PRIMARY, bold=True, anchor="mm")
    c.text(84, 545, "192.168.1.23", 14, INK, anchor="lm")
    c.text(84, 567, "昨天 21:40 · 均衡 · Q70", 12, MUTED, anchor="lm")
    icon_chevron(c, 378, 554, MUTED)

    # guide
    c.card(16, 606, 380, 150)
    c.text(32, 628, "首次使用？3 步开始投屏", 14, INK, bold=True, anchor="lm")
    for i, line in enumerate(["在掌机打开 3DS Live", "在就绪画面按 R + SELECT", "回到此页等待自动连接"]):
        y = 656 + i * 28
        c.circle(42, y + 8, 10, fill=SURFACE_HIGH)
        c.text(42, y + 8, str(i + 1), 10, INK, bold=True, anchor="mm")
        c.text(58, y + 8, line, 12, MUTED, anchor="lm")

    c.card(16, 772, 380, 68, r=16)
    icon_wifi_small(c, 36, 793, PRIMARY)
    c.text(52, 793, "手机与掌机需在同一 Wi-Fi", 12, INK, bold=True, anchor="lm")
    c.text(52, 815, "5 GHz 或 Wi-Fi 6 更稳定，避免访客网络与 AP 隔离。", 11, MUTED, anchor="lm")

    # bottom nav
    c.rect(0, 851, 412, 64, fill=SURFACE)
    c.line(0, 851, 412, 851, LINE, 1)
    nav_items(c, "connect")
    return c


def nav_items(c: Canvas, active: str):
    items = [("connect", "连接", icon_signal), ("diag", "诊断", icon_pulse),
             ("settings", "设置", icon_gear)]
    for i, (key, label, draw) in enumerate(items):
        cx = 412 / 6 + i * 412 / 3
        color = PRIMARY if key == active else MUTED
        draw(c, cx, 872, color, r=7.5)
        c.text(cx, 894, label, 11, color, bold=True, anchor="mm")


def live(stage_dy=0, dock=True) -> Canvas:
    c = Canvas("#000000")
    # stage
    top_h = 388 * 240 / 400
    bottom_h = 388 * 240 / 320
    total = top_h + 8 + bottom_h
    y0 = (H - total) / 2 + stage_dy
    c.rect(12, y0, 388, top_h, fill="#0F1115", outline="#1B1E24", width=1)
    c.text(206, y0 + top_h / 2, "上屏 · 400×240", 12, A("#FFFFFF", 0.34), anchor="mm")
    by = y0 + top_h + 8
    c.rect(12, by, 388, bottom_h, fill="#0F1115", outline="#1B1E24", width=1)
    c.text(206, by + bottom_h / 2, "下屏 · 320×240", 12, A("#FFFFFF", 0.34), anchor="mm")

    # HUD
    c.vgradient(0, 0, 412, 116, (11, 15, 20, 235), (11, 15, 20, 0))
    icon_back(c, 32, 46, "#FFFFFF")
    c.rect(56, 30, 174, 32, fill=A("#FFFFFF", 0.14), r=16)
    c.dot(72, 46, 3, GREEN, ring=(3, A(GREEN, 0.22)))
    c.text(82, 46, "29.8 · Q70 · 1.8 MB/s", 12, "#FFFFFF", bold=True, anchor="lm")
    icon_lock(c, 292, 46, "#FFFFFF")
    icon_camera(c, 332, 46, "#FFFFFF")
    icon_more(c, 372, 46, "#FFFFFF")

    if not dock:
        return c
    # recording badge
    c.rect(300, 84, 96, 26, fill=A("#E05A5A", 0.18), r=13)
    c.dot(316, 97, 3, "#E05A5A")
    c.text(326, 97, "00:42", 11, "#FFB4AB", bold=True, anchor="lm")

    # dock
    c.vgradient(0, 787, 412, 128, (11, 15, 20, 0), (11, 15, 20, 242))
    chips = [("高清", False), ("均衡", True), ("低带宽", False)]
    x = 12
    for label, on in chips:
        w = 28 + c.d.textlength(label, font=font(12, True)) / S
        if on:
            c.rect(x, 803, w, 34, fill=A("#78D7C7", 0.14), r=17, outline=A("#78D7C7", 0.55), width=1)
        else:
            c.rect(x, 803, w, 34, fill=A("#FFFFFF", 0.10), r=17)
        c.text(x + w / 2, 820, label, 12, "#78D7C7" if on else A("#FFFFFF", 0.78),
               bold=True, anchor="mm")
        x += w + 8

    icon_record(c, 32, 871, "#FF8A80", r=12)
    icon_shot(c, 72, 871, "#FFFFFF")
    icon_layout(c, 112, 871, "#FFFFFF")
    c.rect(352, 849, 44, 44, fill=A("#E05A5A", 0.92), r=12)
    icon_stop(c, 374, 871, "#FFFFFF")
    return c


def quality() -> Canvas:
    base = live(stage_dy=-96, dock=False)
    c = Canvas("#000000")
    c.img = base.img.copy()
    c.d = ImageDraw.Draw(c.img, "RGBA")
    # dim the video only (sheet area is drawn over it)
    c.dim(0.42)
    sheet_y = 311
    c.d.rounded_rectangle([0, px(sheet_y), px(W), px(H)], radius=px(24), fill="#10211D")
    c.rect(0, sheet_y, 412, 1, fill="#29413A")

    c.rect(188, sheet_y + 10, 36, 4, fill=A("#FFFFFF", 0.22), r=2)
    c.text(16, 337, "掌机推流", 17, "#EAF5F2", bold=True, anchor="lm")
    c.text(396, 337, "重置", 13, "#78D7C7", bold=True, anchor="rm")

    presets = [("高清", ["30 FPS", "Q80", "2.0 MB/s"], False),
               ("均衡", ["30 FPS", "Q70", "2.0 MB/s"], True),
               ("低带宽", ["24 FPS", "Q40", "1.0 MB/s"], False)]
    pw = 121.3
    for i, (name, meta, on) in enumerate(presets):
        x = 16 + i * (pw + 8)
        if on:
            c.rect(x, 356, pw, 92, fill="#1B2D27", r=16, outline="#78D7C7", width=1.5)
        else:
            c.rect(x, 356, pw, 92, fill="#172C27", r=16)
        c.text(x + 10, 380, name, 14, "#EAF5F2", bold=True, anchor="lm")
        for j, line in enumerate(meta):
            c.text(x + 10, 402 + j * 17, line, 11, "#9CB0AA", anchor="lm")
        if on:
            c.circle(x + pw - 16, 368, 8, fill="#78D7C7")
            c.line(x + pw - 19, 368, x + pw - 17, 371, "#0A2B20", 1.6)
            c.line(x + pw - 17, 371, x + pw - 13, 365, "#0A2B20", 1.6)

    def label(y, text):
        c.text(16, y, text, 12, "#9CB0AA", bold=True, anchor="lm")

    def seg(y, options, active, small=False):
        c.rect(16, y, 380, 44, fill="#172C27", r=14)
        cell = (380 - 6 - 3 * (len(options) - 1)) / len(options)
        for i, option in enumerate(options):
            x = 19 + i * (cell + 3)
            if i == active:
                c.rect(x, y + 3, cell, 38, fill="#2A413A", r=12)
            c.text(x + cell / 2, y + 22, option, 11 if small else 12,
                   "#EAF5F2" if i == active else "#9CB0AA", bold=True, anchor="mm")

    label(480, "目标帧率")
    seg(496, ["15", "20", "24", "30"], 3)
    label(560, "JPEG 质量")
    seg(576, ["自动", "Q40", "Q60", "Q70", "Q80"], 3, small=True)
    label(640, "带宽上限")
    c.text(396, 640, "1.80 MB/s", 12, "#EAF5F2", bold=True, anchor="rm")
    c.rect(16, 668, 380, 4, fill="#29413A", r=2)
    c.rect(16, 668, 247, 4, fill="#78D7C7", r=2)
    c.circle(16 + 247, 670, 9, fill="#EAF5F2")
    label(706, "带宽模式")
    seg(722, ["自适应上限", "固定上限"], 0)

    c.text(16, 796, "掌机确认", 12, "#9CB0AA", anchor="lm")
    c.text(66, 796, "Q70 · 2.00 MB/s · 29.8 fps", 12, "#78D7C7", bold=True, anchor="lm")
    c.rect(16, 816, 380, 48, fill="#78D7C7", r=14)
    c.text(206, 840, "应用", 14, "#0A2B20", bold=True, anchor="mm")
    _ = sheet_y
    return c


def diagnostics() -> Canvas:
    c = Canvas(CANVAS)
    statusbar(c)
    c.text(16, 54, "诊断", 24, INK, bold=True, anchor="lm")
    c.text(396, 54, "导出 JSON", 13, PRIMARY, bold=True, anchor="rm")
    c.text(16, 92, "采样窗口 4.0 s · 已对齐两端状态报文", 12, MUTED, anchor="lm")

    tiles = [("双屏收齐", "29.8", "FPS"), ("上屏呈现", "29.7", "FPS"), ("掌机上限", "2.00", "MB/s"),
             ("掌机提交", "1.92", "MB/s"), ("本机实收", "1.78", "MB/s"), ("实际质量", "Q70", "")]
    tw = (380 - 16) / 3
    for i, (key, value, unit) in enumerate(tiles):
        x = 16 + (i % 3) * (tw + 8)
        y = 110 + (i // 3) * 92
        c.card(x, y, tw, 84)
        c.text(x + 12, y + 22, key, 11, MUTED, bold=True, anchor="lm")
        c.text(x + 12, y + 54, value, 20, INK, bold=True, anchor="lm")
        if unit:
            vw = c.d.textlength(value, font=font(20, True)) / S
            c.text(x + 14 + vw, y + 57, unit, 11, MUTED, anchor="lm")

    # chart
    c.card(16, 300, 380, 176, r=16)
    c.text(30, 320, "近 60 秒", 14, INK, bold=True, anchor="lm")
    c.line(258, 320, 270, 320, PRIMARY, 2)
    c.text(274, 320, "收齐 FPS", 11, MUTED, anchor="lm")
    c.line(330, 320, 342, 320, SECONDARY, 1.5, dash=3)
    c.text(346, 320, "带宽", 11, MUTED, anchor="lm")

    chart_x, chart_y, chart_w, chart_h = 30, 344, 352, 78

    def points(series, lo=18, hi=32):
        return [(chart_x + x / 60 * chart_w, chart_y + chart_h - (y - lo) / (hi - lo) * chart_h)
                for x, y in series]

    fps = [(0, 26), (6, 29), (12, 30), (18, 29), (24, 22), (30, 26), (36, 29), (42, 30), (48, 30), (54, 29), (60, 30)]
    bw = [(0, 24), (6, 27), (12, 28), (18, 27), (24, 20), (30, 24), (36, 27), (42, 28), (48, 28), (54, 27), (60, 28)]
    c.d.line([px(chart_x), px(chart_y + chart_h), px(chart_x + chart_w), px(chart_y + chart_h)], fill=LINE, width=1)
    
    # dashed bandwidth polyline drawn segment by segment
    pts = points(bw)
    for a, b in zip(pts, pts[1:]):
        c.line(a[0], a[1], b[0], b[1], SECONDARY, 1.5, dash=3)
    pts = points(fps)
    for a, b in zip(pts, pts[1:]):
        c.line(a[0], a[1], b[0], b[1], PRIMARY, 2)
    for label, x, anchor in (("60 秒前", 30, "la"), ("30", 206, "ma"), ("现在", 382, "ra")):
        c.text(x, 452, label, 11, MUTED, anchor=anchor)

    # rows
    c.card(16, 490, 380, 214, r=16)
    rows = [("缺片帧", "12"), ("修复成功 / 超时", "9 / 3"), ("行复用节省", "38%"),
            ("LZ4 下屏节省", "6%"), ("音频已收 / 空洞", "60.0 s / 0.4 s")]
    for i, (key, value) in enumerate(rows):
        y = 512 + i * 42
        if i:
            c.line(30, y - 21, 382, y - 21, LINE, 1)
        c.text(30, y, key, 13, MUTED, anchor="lm")
        c.text(382, y, value, 13, INK, bold=True, anchor="rm")

    c.paragraph(16, 714, ["窗口以掌机状态报文对齐：两端 MB/s 同计音视频与 IPv4/UDP 头。"], 11, MUTED, 17)

    c.card(16, 748, 380, 88, r=16)
    c.text(30, 768, "最近事件", 13, INK, bold=True, anchor="lm")
    c.text(30, 793, "21:13  自适应降档 2.00 → 1.80 MB/s", 11, MUTED, anchor="lm")
    c.text(30, 815, "21:12  参考帧重建 · 行复用恢复", 11, MUTED, anchor="lm")

    c.rect(0, 851, 412, 64, fill=SURFACE)
    c.line(0, 851, 412, 851, LINE, 1)
    nav_items(c, "diag")
    return c


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    connect().save("connect.png")
    live().save("live.png")
    quality().save("quality.png")
    diagnostics().save("diagnostics.png")


if __name__ == "__main__":
    main()
