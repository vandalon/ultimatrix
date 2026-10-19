"""Builds docs/previews/quickstart.gif: flash, scan the setup QR, Wi-Fi setup, pick a face.
Composed from existing renders (mock-ups, not footage). Needs the setup QR library built first:
  cc -shared -fPIC -o $QRLIB firmware/lib/core/qrcodegen.c
Then shrink: ffmpeg -i quickstart.gif -vf "scale=560:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96[p];[b][p]paletteuse=dither=bayer:bayer_scale=4" out.gif
"""
import ctypes, os
from PIL import Image, ImageDraw, ImageFont, ImageSequence
D='/Users/vandalon/ultimatrix/docs/previews/'
W,H=640,360; FPS=10
BG=(13,20,28); FG=(235,240,235); ACC=(190,230,90)
F='/System/Library/Fonts/Helvetica.ttc'
fb=ImageFont.truetype(F,22,index=1); fm=ImageFont.truetype(F,15); fs=ImageFont.truetype(F,13)
def frames(n):
    return [f.convert('RGB') for f in ImageSequence.Iterator(Image.open(D+n))]

# --- panel with frame, LED dot look
IN=256; FR=16; PS=IN+2*FR
cell=IN//64
mask=Image.new('L',(cell*8,cell*8),0)
_d=ImageDraw.Draw(mask); 
for i in range(8): _d.ellipse((i*cell+0.3,0.3,i*cell+cell-0.3,cell-0.3),fill=255)
tile=Image.new('L',(cell,cell),0); ImageDraw.Draw(tile).ellipse((0.4,0.4,cell-0.4,cell-0.4),fill=255)
def dots(im64):
    big=im64.resize((IN,IN),Image.NEAREST)
    m=Image.new('L',(IN,IN))
    for y in range(64):
        for x in range(64): m.paste(tile,(x*cell,y*cell))
    out=Image.new('RGB',(IN,IN),(6,6,8)); out.paste(big,(0,0),m); return out
def framed(inner):
    im=Image.new('RGB',(PS,PS),(30,32,36)); d=ImageDraw.Draw(im)
    d.rounded_rectangle((0,0,PS-1,PS-1),10,outline=(70,74,80),width=2)
    for cx,cy in ((8,8),(PS-9,8),(8,PS-9),(PS-9,PS-9)):
        d.ellipse((cx-3,cy-3,cx+3,cy+3),fill=(12,12,14),outline=(90,94,100))
    im.paste(inner.resize((IN,IN),Image.LANCZOS),(FR,FR)); return im
def panel(fr): return framed(fr)

# --- QR (same encoder settings as BootScreen: ECC low, version 1-6)
def qr_image(text):
    lib=ctypes.CDLL(os.environ['QRLIB'])
    q=ctypes.create_string_buffer(3918); t=ctypes.create_string_buffer(3918)
    assert lib.qrcodegen_encodeText(text.encode(),t,q,0,1,6,-1,True)
    n=lib.qrcodegen_getSize(q); sc=64//n; m=(64-n*sc)//2
    im=Image.new('RGB',(64,64),(255,255,255)); px=im.load()
    for y in range(n):
        for x in range(n):
            if lib.qrcodegen_getModule(q,x,y):
                for dy in range(sc):
                    for dx in range(sc): px[m+x*sc+dx,m+y*sc+dy]=(0,0,0)
    return im

def base(step,title):
    im=Image.new('RGB',(W,H),BG); d=ImageDraw.Draw(im)
    d.ellipse((20,16,46,42),fill=ACC); d.text((33,29),str(step),font=fb,fill=BG,anchor='mm')
    d.text((58,29),title,font=fb,fill=FG,anchor='lm')
    return im,d
def phone(img,h=286):
    s=img.resize((int(img.width*h/img.height),h),Image.LANCZOS)
    o=Image.new('RGB',(s.width+8,h+8),(70,74,80)); ImageDraw.Draw(o).rounded_rectangle((0,0,o.width-1,o.height-1),14,outline=(110,116,124),width=3,fill=(20,22,26)); o.paste(s,(4,4)); return o

out=[]
# 1: flash
for i in range(46):
    im,d=base(1,'Flash from your browser (Chrome or Edge)')
    x0,y0=60,80; d.rounded_rectangle((x0,y0,W-60,H-30),12,fill=(24,34,44),outline=(50,66,80))
    d.text((W//2,y0+45),'Ultimatrix web installer',font=fb,fill=FG,anchor='mm')
    d.text((W//2,y0+75),'Plug in the ESP32 over USB and choose a face',font=fm,fill=(150,165,175),anchor='mm')
    p=min(1,max(0,(i-14)/24))
    if i<14:
        d.rounded_rectangle((W//2-190,y0+110,W//2+20,y0+150),8,fill=(36,48,60),outline=(80,100,115)); d.text((W//2-175,y0+130),'Face: Tetris',font=fm,fill=FG,anchor='lm'); d.polygon([(W//2-8,y0+126),(W//2+4,y0+126),(W//2-2,y0+134)],fill=FG)
        d.rounded_rectangle((W//2+40,y0+110,W//2+170,y0+150),8,fill=ACC); d.text((W//2+105,y0+130),'Install',font=fb,fill=BG,anchor='mm')
    else:
        d.rounded_rectangle((120,y0+125,W-120,y0+139),7,fill=(40,54,66))
        d.rounded_rectangle((120,y0+125,120+int((W-240)*p),y0+139),7,fill=ACC)
        d.text((W//2,y0+165),('Installation complete' if p>=1 else 'Installing… %d%%'%int(p*100)),font=fm,fill=ACC if p>=1 else FG,anchor='mm')
    out.append(im)

# 2: scan the QR code on the panel
qrp=framed(dots(qr_image('WIFI:S:UM-SETUP;;')))
for i in range(36):
    im,d=base(2,'Scan the QR code on the panel')
    im.paste(qrp,(40,56)); x0=370
    d.rounded_rectangle((x0,66,x0+210,296),22,fill=(24,34,44),outline=(110,116,124),width=3)
    if i<14:
        d.rectangle((x0+35,100,x0+175,240),outline=ACC,width=3)
        y=105+int(i/13*130); d.line((x0+40,y,x0+170,y),fill=ACC,width=2)
        d.text((x0+105,265),'Camera: scanning...',font=fs,fill=(150,165,175),anchor='mm')
    else:
        d.text((x0+105,125),'Join Wi-Fi network',font=fm,fill=FG,anchor='mm')
        d.text((x0+105,155),'"UM-SETUP"?',font=fb,fill=FG,anchor='mm')
        d.rounded_rectangle((x0+50,195,x0+160,231),10,fill=ACC); d.text((x0+105,213),'Join',font=fb,fill=BG,anchor='mm')
    out.append(im)

# 3: Wi-Fi setup itself, drawn on the real setup page
wifi=Image.open(D+'wifi-setup-design.png').convert('RGB')
f17=ImageFont.truetype(F,17); f15b=ImageFont.truetype(F,15,index=1)
boot=frames('boot.gif'); home=frames('home.gif')
PW='correct horse'  # shown as dots
for i in range(70):
    pg=wifi.copy(); d=ImageDraw.Draw(pg)
    if i>=4: d.rectangle((40,248,140,270),fill=wifi.getpixel((200,235)))   # blank the original link first
    if 4<=i<10: d.rectangle((40,248,140,270),fill=(214,232,150))           # tapping "Home network"
    if i>=4: d.text((45,259),'Home network',font=f17,fill=(60,80,20),anchor='lm')
    if i>=10:                                                              # name filled in
        d.rounded_rectangle((46,318,344,358),10,fill=(255,255,255)); d.text((58,338),'Home network',font=f17,fill=(30,30,30),anchor='lm')
    if i>=14:                                                              # password typed
        n=min(len(PW),(i-14)); d.rounded_rectangle((46,378,344,418),10,fill=(255,255,255))
        d.text((58,398),'•'*n+('|' if i%4<2 and n<len(PW) else ''),font=f17,fill=(30,30,30),anchor='lm')
    if 32<=i<38: d.rounded_rectangle((45,421,104,467),12,fill=(70,90,40))   # Save pressed
    if i>=38:
        d.rounded_rectangle((20,40,370,300),18,fill=(255,255,255),outline=(200,210,190),width=3)
        d.text((195,150),'Saved.',font=ImageFont.truetype(F,34,index=1),fill=(30,40,30),anchor='mm')
        d.text((195,200),'The clock restarts and',font=f17,fill=(90,95,90),anchor='mm')
        d.text((195,224),'joins your Wi-Fi.',font=f17,fill=(90,95,90),anchor='mm')
    ph=phone(pg)
    sc=boot[(i*2)%len(boot)] if i<55 else home[((i-55)*2+30)%len(home)]
    im,dd=base(3,'Choose your Wi-Fi and enter the password')
    im.paste(ph,(100,54)); im.paste(framed(dots(sc.resize((64,64),Image.LANCZOS))),(320,56))
    out.append(im)

# 4: it runs
for n,l in zip(['home','tetris','mario','pacman','word','f1','football'],['Home Assistant','Tetris','Mario','Pac-Man','Words','Formula 1','Football']):
    fr=frames(n+'.gif')
    for i in range(16):
        im,d=base(4,'Done: your clock is running')
        im.paste(framed(dots(fr[(i*2+20)%len(fr)].resize((64,64),Image.LANCZOS))),(W//2-PS//2,50))
        d.text((W//2,50+PS+6),l+'  \u00b7  change faces any time from the settings page',font=fs,fill=(150,165,175),anchor='mt')
        out.append(im)
out[0].save(D+'quickstart.gif',save_all=True,append_images=out[1:],duration=1000//FPS,loop=0,optimize=True)
print(len(out))
