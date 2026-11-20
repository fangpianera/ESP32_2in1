import sys
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
# 视情况给裁剪框：x0 y0 x1 y1 比例(0~1)，默认取照片中部
x0, y0, x1, y1 = [float(v) for v in sys.argv[3:7]] if len(sys.argv) > 6 else (0.28, 0.38, 0.62, 0.68)
scale = int(sys.argv[7]) if len(sys.argv) > 7 else 5
im = Image.open(src).convert("RGB")
W, H = im.size
box = (int(W * x0), int(H * y0), int(W * x1), int(H * y1))
c = im.crop(box)
c = c.resize((c.width * scale, c.height * scale), Image.LANCZOS)
c.save(out)
print(out, c.size)
