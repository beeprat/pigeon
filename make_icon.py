# Draws the pigeon icon (minimal white pigeon silhouette, dark outline so it shows on light taskbars) and saves pigeon.ico next to this script.
import os
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # draw large, downscale for smooth edges
mask = Image.new('L', (S, S), 0)
d = ImageDraw.Draw(mask)
d.ellipse([230, 380, 760, 760], fill=255)                                  # plump body
d.ellipse([200, 230, 400, 430], fill=255)                                  # small head
d.polygon([(230, 360), (390, 340), (420, 520), (250, 540)], fill=255)     # neck into chest
d.polygon([(210, 320), (110, 350), (205, 362)], fill=255)                  # thin beak
d.polygon([(700, 470), (990, 610), (950, 680), (700, 690)], fill=255)     # tail, angled down
edge = mask.filter(ImageFilter.MaxFilter(49))                              # outline ring around the shape
im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
im.paste((40, 40, 40, 255), (0, 0), edge)
im.paste((255, 255, 255, 255), (0, 0), mask)
o = ImageDraw.Draw(im)
o.ellipse([262, 295, 307, 340], fill=(40, 40, 40, 255))                     # eye

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pigeon.ico')
im.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
dark = Image.new('RGBA', (S, S), (0, 0, 0, 0))                           # light look: dark pigeon, white eye
dark.paste((40, 40, 40, 255), (0, 0), mask)
ImageDraw.Draw(dark).ellipse([262, 295, 307, 340], fill=(255, 255, 255, 255))
dark.save(out.replace('pigeon.ico', 'pigeon_dark.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48)])
if os.environ.get('PREVIEW_DIR'): im.resize((256, 256), Image.LANCZOS).save(os.path.join(os.environ['PREVIEW_DIR'], 'icon_preview.png'))
print(out)
