"""Find code computing a PC-relative address within [lo,hi): xref.py lo hi"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recomp import Program, bits
p = Program(sys.argv[3] if len(sys.argv) > 3 else 'gen/armv6.macho')
p.explore()
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
m = p.m
for a in sorted(p.code):
    w = m.u32(a)
    # ldr rX, [pc, #imm]  (literal)
    if (w & 0x0f7f0000) == 0x051f0000:
        imm = w & 0xfff
        lit_addr = a + 8 + (imm if w & (1 << 23) else -imm)
        L = m.u32(lit_addr)
        rd = bits(w, 15, 12)
        # look ahead for use of rd with pc: add rY, pc, rd  or ldr rY, [pc, rd]
        for k in range(1, 12):
            b = a + 4 * k
            if b not in p.code: break
            ww = m.u32(b)
            if ((ww & 0x0fff0ff0) == 0x008f0000 or (ww & 0x0fff0ff0) == 0x079f0000) and (ww & 15) == rd:
                t = (L + b + 8) & 0xffffffff
                kind = 'add' if (ww & 0x0fff0ff0) == 0x008f0000 else 'ldr'
                if kind == 'ldr':
                    t2 = m.u32(t)
                    if t2 is not None and lo <= t2 < hi:
                        print('%x: ptr via [%x] -> %x' % (b, t, t2))
                elif lo <= t < hi:
                    print('%x: addr %x' % (b, t))
                break
