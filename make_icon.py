# Draws the grimnotif envelope icon and saves grimnotif.ico (multi-size) next to this script.
import os
from PIL import Image, ImageDraw

S = 1024  # draw large, downscale for smooth edges
im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
m, top, bot = 64, 224, 800
d.rounded_rectangle([m, top, S - m, bot], radius=90, fill=(234, 67, 53, 255))          # envelope body
d.polygon([(m + 30, top + 40), (S // 2, 560), (S - m - 30, top + 40)], fill=(255, 255, 255, 255))  # flap
d.line([(m + 40, top + 50), (S // 2, 560), (S - m - 40, top + 50)], fill=(197, 34, 31, 255), width=40, joint='curve')

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'grimnotif.ico')
im.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
print(out)
