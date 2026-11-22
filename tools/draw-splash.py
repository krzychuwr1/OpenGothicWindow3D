# Usage: python tools/draw-splash.py android/quest/assets/vr_splash.png preview.png  (needs Pillow, Windows fonts)
# Line-art splash for OpenGothic Window3D: a floating pointed-arch window (front and back frame for
# depth) with a minimal landscape inside, and a wordmark. Transparent background, 1024x1024.
import math, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

S = 4                      # supersampling
W = 1024*S
LINE  = (238, 228, 206, 255)   # warm off-white
DIM   = (238, 228, 206, 120)
AMBER = (230, 166, 70, 255)

def P(x, y): return (x*S, y*S)

def arch(cx, base, width, spring, radius_k=0.78, steps=64):
    """Closed outline of a pointed arch: vertical sides from `base` up to `spring`, two arcs to the apex."""
    hw = width/2
    r  = radius_k*width
    xl, xr = cx-hw, cx+hw
    # left arc centred right of the left side, right arc centred left of the right side
    cl = xl + r
    cr = xr - r
    apex_dx = cx - xl
    apex_y = spring - math.sqrt(r*r - (r-apex_dx)**2)
    pts = [(xl, base), (xl, spring)]
    a0 = math.pi; a1 = math.pi - math.acos((r-apex_dx)/r)
    for i in range(1, steps+1):
        a = a0 + (a1-a0)*i/steps
        pts.append((cl + r*math.cos(a), spring - r*math.sin(a) if False else spring + r*math.sin(a)*-1 if False else spring - abs(r*math.sin(a))))
    b0 = math.acos((r-apex_dx)/r); b1 = 0.0
    for i in range(0, steps+1):
        a = b0 + (b1-b0)*i/steps
        pts.append((cr + r*math.cos(a), spring - abs(r*math.sin(a))))
    pts.append((xr, base))
    return pts, apex_y

def poly(draw, pts, fill, w, closed=False):
    q = [P(*p) for p in pts]
    if closed: q = q + [q[0]]
    draw.line(q, fill=fill, width=int(w*S), joint="curve")
    for p in (q[0], q[-1]):
        r = w*S/2
        draw.ellipse([p[0]-r, p[1]-r, p[0]+r, p[1]+r], fill=fill)

def smooth(points, n=24):
    """Catmull-Rom through the points."""
    out = []
    pts = [points[0]] + points + [points[-1]]
    for i in range(1, len(pts)-2):
        p0, p1, p2, p3 = pts[i-1], pts[i], pts[i+1], pts[i+2]
        for k in range(n):
            t = k/n; t2 = t*t; t3 = t2*t
            out.append(tuple(0.5*((2*p1[j]) + (-p0[j]+p2[j])*t + (2*p0[j]-5*p1[j]+4*p2[j]-p3[j])*t2 + (-p0[j]+3*p1[j]-3*p2[j]+p3[j])*t3) for j in range(2)))
    out.append(points[-1])
    return out

art = Image.new("RGBA", (W, W), (0, 0, 0, 0))
d = ImageDraw.Draw(art)

cx, base, width, spring = 512, 620, 300, 400
front, apex = arch(cx, base, width, spring)
depth = (30, -24)
back = [(x+depth[0], y+depth[1]) for x, y in front]

# Back frame (thin, dim) and the edges joining it to the front: the window has depth
poly(d, back, DIM, 3, closed=True)
for i in (0, 1, len(front)//2, len(front)-2, len(front)-1):
    poly(d, [front[i], back[i]], DIM, 3)

# Landscape, clipped to the window opening
scene = Image.new("RGBA", (W, W), (0, 0, 0, 0))
s = ImageDraw.Draw(scene)
r = 34
ridge = smooth([(330, 520), (380, 470), (420, 500), (470, 440), (520, 492), (575, 455), (620, 500), (690, 470)])
# Low sun, setting behind the far ridge
sun = Image.new("RGBA", (W, W), (0, 0, 0, 0))
ImageDraw.Draw(sun).ellipse([P(445-r, 478-r), P(445+r, 478+r)], outline=AMBER, width=4*S)
above = Image.new("L", (W, W), 255)
ImageDraw.Draw(above).polygon([P(*p) for p in ridge] + [P(700, 720), P(320, 720)], fill=0)
sun.putalpha(ImageChops.multiply(sun.getchannel("A"), above))
scene.alpha_composite(sun)
poly(s, ridge, DIM, 3.5)
hill = smooth([(330, 590), (420, 568), (500, 548), (560, 520), (600, 512), (650, 530), (690, 545)])
poly(s, hill, LINE, 4.5)
# Castle on the hill: keep, two towers, battlements
def castle(x, y, k=1.0):
    L = []
    L.append([(x-40*k, y), (x-40*k, y-42*k), (x-48*k, y-42*k), (x-48*k, y-54*k), (x-40*k, y-54*k), (x-40*k, y-48*k), (x-32*k, y-48*k), (x-32*k, y-54*k), (x-24*k, y-54*k), (x-24*k, y-42*k), (x-24*k, y-30*k)])
    L.append([(x-24*k, y-30*k), (x+20*k, y-30*k)])
    L.append([(x-10*k, y-30*k), (x-10*k, y-70*k), (x-16*k, y-70*k), (x-16*k, y-80*k), (x-8*k, y-80*k), (x-8*k, y-74*k), (x, y-74*k), (x, y-80*k), (x+8*k, y-80*k), (x+8*k, y-70*k), (x+2*k, y-70*k), (x+2*k, y-30*k)])
    L.append([(x+20*k, y), (x+20*k, y-46*k), (x+14*k, y-46*k), (x+14*k, y-56*k), (x+30*k, y-56*k), (x+30*k, y-46*k), (x+24*k, y-46*k)])
    L.append([(x-3*k, y-90*k), (x-3*k, y-80*k)])
    return L
for seg in castle(600, 512, 0.95):
    poly(s, seg, LINE, 3.5)
s.polygon([P(595, 445), P(612, 451), P(595, 457)], fill=AMBER)                # pennant
s.rectangle([P(594, 480), P(600, 488)], fill=AMBER)                           # lit window
path = smooth([(470, 625), (500, 600), (470, 585), (520, 566), (560, 548), (590, 525)])
poly(s, path, AMBER, 3.5)
# Foreground pines at the left
for px, py, h in ((372, 600, 70), (400, 606, 52)):
    poly(s, [(px, py), (px, py-h)], LINE, 3.5)
    for k in range(4):
        yy = py - h*0.25 - k*h*0.18; ww = (h*0.32)*(1-k*0.2)
        poly(s, [(px-ww, yy), (px, yy-h*0.18), (px+ww, yy)], LINE, 3.5)

mask = Image.new("L", (W, W), 0)
ImageDraw.Draw(mask).polygon([P(*p) for p in front], fill=255)
mask = mask.filter(ImageFilter.GaussianBlur(1*S))
scene.putalpha(ImageChops.multiply(scene.getchannel("A"), mask))
art = Image.alpha_composite(art, scene)
d = ImageDraw.Draw(art)

# Front frame on top, bold, with a sill
poly(d, front, LINE, 7, closed=True)
poly(d, [(cx-width/2-22, base), (cx+width/2+22, base)], LINE, 7)
poly(d, [(cx-width/2-10, base+14), (cx+width/2+10, base+14)], DIM, 3)

# Wordmark
fonts = "C:/Windows/Fonts/"
def text_center(txt, y, size, fill, spacing, font="georgia.ttf"):
    f = ImageFont.truetype(fonts+font, size*S)
    widths = [d.textlength(c, font=f) for c in txt]
    total = sum(widths) + spacing*S*(len(txt)-1)
    x = (W-total)/2
    for c, w_ in zip(txt, widths):
        d.text((x, y*S), c, font=f, fill=fill)
        x += w_ + spacing*S
    return (W-total)/2/S, (W+total)/2/S
text_center("OPENGOTHIC", 676, 58, LINE, 10)
x0, x1 = text_center("WINDOW  3D", 752, 30, AMBER, 14)
ym = 770
poly(d, [(x0-90, ym), (x0-26, ym)], DIM, 2.5)
poly(d, [(x1+26, ym), (x1+90, ym)], DIM, 2.5)
for xx in (x0-20, x1+20):
    d.polygon([P(xx, ym-6), P(xx+6, ym), P(xx, ym+6), P(xx-6, ym)], fill=AMBER)

# Soft dark halo so the lines read over a bright room as well as over black
a = art.getchannel("A")
halo = a.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(10*S))
shadow = Image.new("RGBA", (W, W), (0, 0, 0, 0))
shadow.putalpha(halo.point(lambda v: int(v*0.55)))
out = Image.alpha_composite(shadow, art).resize((1024, 1024), Image.LANCZOS)
out.save(sys.argv[1], optimize=True)
prev = Image.new("RGBA", out.size, (40, 44, 52, 255)); prev.alpha_composite(out); prev.save(sys.argv[2])
