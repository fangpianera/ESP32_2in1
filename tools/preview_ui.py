"""466x466 中心文字块排版预览（Host 侧，非板级证据）。

用途：字体/字号/偏移改完后先看比例关系，避免反复烧板。
绘制的环只是示意（真实环由 LVGL lv_draw_arc 在板端绘制），
文字字号、颜色、相对位置与 firmware/main/ui_eva.c 保持一致。

用法：python tools/preview_ui.py [out.png]
"""
import sys
from PIL import Image, ImageDraw, ImageFont

W = H = 466
CX = CY = 233

C_GREEN = "#A6FF00"
C_LILAC = "#C0A0E8"
C_REMAIN = "#B79BE0"
C_PURPLE = "#60269E"
C_DIM = "#2E1A44"
C_WX_TOP = "#FF3333"
C_WX_MID = "#FFFFFF"
C_WX_BOT = "#2E7BFF"

F_CJK20 = r"C:\Windows\Fonts\NotoSansSC-VF.ttf"
F_CJK28 = r"C:\Windows\Fonts\simhei.ttf"
F_TECH = r"D:\豆包ESP32项目\EVA_Nav\tools\fonts\ChakraPetch-SemiBold.ttf"


def font(path, px):
    return ImageFont.truetype(path, px)


def draw_arc_band(d, r_out, r_in, a0, a1, color, steps=360):
    import math
    a0, a1 = (a0 + 360) % 360, (a1 + 360) % 360
    span = (a1 - a0) % 360 or 360
    for i in range(steps):
        t0 = a0 + span * i / steps
        t1 = a0 + span * (i + 1) / steps
        x0, y0 = CX + r_out * math.cos(math.radians(t0)), CY + r_out * math.sin(math.radians(t0))
        x1, y1 = CX + r_out * math.cos(math.radians(t1)), CY + r_out * math.sin(math.radians(t1))
        x2, y2 = CX + r_in * math.cos(math.radians(t1)), CY + r_in * math.sin(math.radians(t1))
        x3, y3 = CX + r_in * math.cos(math.radians(t0)), CY + r_in * math.sin(math.radians(t0))
        d.polygon([(x0, y0), (x1, y1), (x2, y2), (x3, y3)], fill=color)


def text_c(d, cx, cy, s, f, color):
    d.text((cx, cy), s, font=f, fill=color, anchor="mm")


def main(out=r"D:\豆包ESP32项目\EVA_Nav\tests\board\preview_ui11.png"):
    img = Image.new("RGB", (W, H), "black")
    d = ImageDraw.Draw(img)

    # 环1（200..216）：左=电量 95% 紫，右=天气 红白蓝渐变（示意）
    draw_arc_band(d, 216, 200, 90, 270, C_PURPLE)
    draw_arc_band(d, 216, 200, 270, 90, C_WX_MID)
    draw_arc_band(d, 216, 200, 300, 360, C_WX_TOP)
    draw_arc_band(d, 216, 200, 0, 60, C_WX_BOT)

    # 环2（169..185）：已走段淡紫 + 剩余紫
    draw_arc_band(d, 185, 169, -90, 30, C_LILAC)
    draw_arc_band(d, 185, 169, 30, 90, C_PURPLE)

    f56 = font(F_TECH, 56)
    f28 = font(F_CJK28, 28)
    f20 = font(F_CJK20, 20)
    f15 = font(F_TECH, 15)

    text_c(d, CX - 12, CY - 72, "0.0", f56, C_GREEN)
    text_c(d, CX + 58, CY - 44, "m/s", f15, C_GREEN)
    text_c(d, CX, CY - 18, "龙凤新天地购物广场", f20, C_LILAC)
    text_c(d, CX, CY + 22, "右转 后 73 米", f28, C_GREEN)
    text_c(d, CX, CY + 62, "剩 1.8 km", f20, C_REMAIN)
    text_c(d, CX, CY + 96, "MODE: WALK", f15, C_PURPLE)

    img.save(out)
    print(out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else r"D:\豆包ESP32项目\EVA_Nav\tests\board\preview_ui11.png")
