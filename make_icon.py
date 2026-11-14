# Draws the pigeon icon (white pigeon, dark outline so it shows on light taskbars) and saves pigeon.ico next to this script.
import os
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # draw large, downscale for smooth edges
mask = Image.new('L', (S, S), 0)
d = ImageDraw.Draw(mask)
d.ellipse([230, 390, 800, 760], fill=255)                                  # body
d.ellipse([190, 370, 520, 700], fill=255)                                  # puffed chest
d.ellipse([150, 170, 350, 370], fill=255)                                  # small head
d.polygon([(175, 320), (340, 260), (450, 430), (230, 500)], fill=255)      # neck
d.polygon([(158, 255), (70, 290), (165, 305)], fill=255)                   # beak
d.polygon([(680, 500), (995, 610), (975, 700), (660, 720)], fill=255)      # tail
d.polygon([(420, 730), (400, 880), (432, 880), (465, 735)], fill=255)      # legs
d.polygon([(530, 740), (520, 880), (552, 880), (575, 740)], fill=255)

edge = mask.filter(ImageFilter.MaxFilter(49))                              # outline ring around the shape
im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
im.paste((40, 40, 40, 255), (0, 0), edge)
im.paste((255, 255, 255, 255), (0, 0), mask)
o = ImageDraw.Draw(im)
o.arc([400, 430, 800, 700], 195, 330, fill=(150, 150, 150, 255), width=34)  # wing line
o.ellipse([215, 235, 265, 285], fill=(40, 40, 40, 255))                     # eye

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pigeon.ico')
im.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
if os.environ.get('PREVIEW_DIR'): im.resize((256, 256), Image.LANCZOS).save(os.path.join(os.environ['PREVIEW_DIR'], 'icon_preview.png'))
print(out)
