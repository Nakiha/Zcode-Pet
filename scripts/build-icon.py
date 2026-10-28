"""Rebuild the original paw/status icon at Windows tray and Explorer sizes."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
scale = 16
image = Image.new("RGBA", (64 * scale, 64 * scale))
draw = ImageDraw.Draw(image)
purple, cream = "#7963b6", "#fff8fc"
draw.rounded_rectangle((2*scale, 2*scale, 62*scale, 62*scale), 18*scale, fill=purple)
for x, y, angle in ((17, 26, 28), (27, 18, 9), (39, 18, -9), (49, 26, -28)):
    toe = Image.new("RGBA", image.size)
    ImageDraw.Draw(toe).ellipse(((x-5.3)*scale, (y-7)*scale, (x+5.3)*scale, (y+7)*scale), fill=cream)
    image.alpha_composite(toe.rotate(angle, center=(x*scale, y*scale), resample=Image.Resampling.BICUBIC))

def curve(a, b, c, d):
    return [tuple(((1-t)**3*a[k]+3*(1-t)**2*t*b[k]+3*(1-t)*t*t*c[k]+t**3*d[k])*scale for k in (0,1)) for t in (i/40 for i in range(41))]

pad = []
for points in (((19,42),(19,36),(26,30),(33,30)), ((33,30),(40,30),(47,36),(47,42)),
               ((47,42),(47,49),(41,51),(37,48)), ((37,48),(34,46),(32,46),(29,48)),
               ((29,48),(25,51),(19,49),(19,42))):
    pad.extend(curve(*points))
draw = ImageDraw.Draw(image)
draw.polygon(pad, fill=cream)
stroke = [(27*scale,39*scale),(31*scale,43*scale),(39*scale,35*scale)]
draw.line(stroke, fill=purple, width=round(3.2*scale), joint="curve")
for x,y in stroke:
    r=1.6*scale
    draw.ellipse((x-r,y-r,x+r,y+r),fill=purple)
image = image.resize((256,256), Image.Resampling.LANCZOS)
image.save(root / "assets/app-icon.png")
image.save(root / "assets/app-icon.ico", sizes=[(n,n) for n in (16,20,24,32,40,48,64,256)])
