#!/bin/sh
# usage: shots.sh <seconds> [env assignments...]  -> contact sheet in $S/sheet.png
T="$(cd "$(dirname "$0")" && pwd)"
S=/tmp/claude-1000/-home-salah-Games-ios-3d-pixel-racing/cec4c3b8-9f3d-49e8-9233-a9d06ff2ebb0/scratchpad/shots
rm -rf $S; mkdir -p $S
secs=$1; shift
cd "$(dirname "$0")/../build"
env "$@" PIXELRACING_DEBUG=1 PIXELRACING_SHOTS=$S timeout $secs ./pixel_racing > $S/run.log 2>&1
grep -v -e SmdLoad -e "loops/s" $S/run.log | tail -20
cd $S && python3 - <<'PY'
import glob
fs=sorted(glob.glob("frame_*.ppm"))[-12:]
imgs=[]
for f in fs:
    d=open(f,'rb').read(); parts=d.split(b'\n',3); w,h=map(int,parts[1].split()); px=parts[3]
    w2,h2=w//3,h//3
    out=bytearray(w2*h2*3)
    for y in range(h2):
        row=px[(3*y)*w*3:(3*y+1)*w*3]
        for x in range(w2):
            out[(y*w2+x)*3:(y*w2+x)*3+3]=row[3*x*3:3*x*3+3]
    imgs.append((w2,h2,out))
cols=6
rows=(len(imgs)+cols-1)//cols
w2,h2=imgs[0][0],imgs[0][1]
W=w2*cols; H=h2*rows
sheet=bytearray(W*H*3)
for k,(w,h,o) in enumerate(imgs):
    ox=(k%cols)*w; oy=(k//cols)*h
    for y in range(h): sheet[((oy+y)*W+ox)*3:((oy+y)*W+ox+w)*3]=o[y*w*3:(y+1)*w*3]
open('sheet.ppm','wb').write(b'P6\n%d %d\n255\n'%(W,H)+bytes(sheet))
PY
python3 "$T/ppm2png.py" $S/sheet.ppm
ls $S | grep -c frame_.*ppm
