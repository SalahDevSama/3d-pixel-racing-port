"""Extract the armv6 slice from a fat Mach-O: extract_slice.py <fat> <out>"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
magic, n = struct.unpack('>II', d[:8])
if magic != 0xcafebabe:
    open(sys.argv[2], 'wb').write(d)
    sys.exit(0)
for i in range(n):
    ct, cs, off, size, al = struct.unpack('>5I', d[8 + 20 * i:28 + 20 * i])
    if ct == 12 and cs == 6:
        open(sys.argv[2], 'wb').write(d[off:off + size])
        sys.exit(0)
sys.exit('no armv6 slice found')
