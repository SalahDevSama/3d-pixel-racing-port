"""Minimal Mach-O (32-bit ARM) loader used by the recompiler."""
import struct

class MachO:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        self.segments = []   # (name, vmaddr, vmsize, fileoff, filesize)
        self.sections = {}   # name -> (addr, size, offset)
        self.entry = None
        self._parse()
        self.mem = {}
        self.stubs = {}      # stub addr -> symbol
        self.binds = {}      # data addr -> symbol
        self._parse_imports()

    def _parse(self):
        d = self.data
        magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags = struct.unpack('<7I', d[:28])
        off = 28
        for _ in range(ncmds):
            cmd, size = struct.unpack('<II', d[off:off+8])
            if cmd == 1:  # LC_SEGMENT
                segname = d[off+8:off+24].rstrip(b'\0').decode()
                vmaddr, vmsize, fileoff, filesize, maxp, initp, nsects, fl = struct.unpack('<8I', d[off+24:off+56])
                self.segments.append((segname, vmaddr, vmsize, fileoff, filesize))
                so = off + 56
                for i in range(nsects):
                    sectname = d[so:so+16].rstrip(b'\0').decode()
                    addr, ssize, soff = struct.unpack('<3I', d[so+32:so+44])
                    self.sections[sectname] = (addr, ssize, soff)
                    so += 68
            elif cmd == 5:  # LC_UNIXTHREAD
                # flavor, count, then r0..r15
                regs = struct.unpack('<17I', d[off+16:off+16+68])
                self.entry = regs[15]
            off += size

    def _parse_imports(self):
        """Stub/pointer -> symbol name, from the indirect symbol table and dyld bind opcodes."""
        d = self.data
        magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags = struct.unpack('<7I', d[:28])
        off = 28
        symtab = dysymtab = dyldinfo = None
        sects = []   # (sectname, addr, size, flags, reserved1, reserved2)
        segs = []
        for _ in range(ncmds):
            cmd, size = struct.unpack('<II', d[off:off+8])
            if cmd == 1:
                vmaddr = struct.unpack('<I', d[off+24:off+28])[0]
                segs.append(vmaddr)
                nsects = struct.unpack('<I', d[off+48:off+52])[0]
                so = off + 56
                for i in range(nsects):
                    name = d[so:so+16].rstrip(b'\0').decode()
                    addr, ssize, soff, align, reloff, nreloc, fl, r1, r2 = struct.unpack('<9I', d[so+32:so+68])
                    sects.append((name, addr, ssize, fl, r1, r2))
                    so += 68
            elif cmd == 2:
                symtab = struct.unpack('<4I', d[off+8:off+24])
            elif cmd == 0xb:
                dysymtab = struct.unpack('<18I', d[off+8:off+80])
            elif cmd in (0x22, 0x80000022):
                dyldinfo = struct.unpack('<10I', d[off+8:off+48])
            off += size
        symoff, nsyms, stroff, strsize = symtab

        def symname(i):
            n_strx = struct.unpack('<I', d[symoff + 12 * i:symoff + 12 * i + 4])[0]
            e = d.index(b'\0', stroff + n_strx)
            return d[stroff + n_strx:e].decode()

        indirectsymoff, nindirect = dysymtab[12], dysymtab[13]
        for name, addr, size, fl, r1, r2 in sects:
            typ = fl & 0xff
            if typ == 8:      # S_SYMBOL_STUBS
                n = size // r2
                for k in range(n):
                    idx = struct.unpack('<I', d[indirectsymoff + 4 * (r1 + k):][:4])[0]
                    if idx & 0xc0000000 == 0:
                        self.stubs[addr + k * r2] = symname(idx)
            elif typ in (6, 7):   # non-lazy / lazy symbol pointers
                for k in range(size // 4):
                    idx = struct.unpack('<I', d[indirectsymoff + 4 * (r1 + k):][:4])[0]
                    if idx & 0xc0000000 == 0:
                        self.binds[addr + 4 * k] = symname(idx)
        if dyldinfo:
            # bind, weak bind and lazy bind opcode streams
            for o, n in ((dyldinfo[2], dyldinfo[3]), (dyldinfo[6], dyldinfo[7])):
                self._run_bind(d[o:o + n], segs)

    def _run_bind(self, ops, segs):
        def uleb(i):
            v = sh = 0
            while True:
                b = ops[i]; i += 1
                v |= (b & 0x7f) << sh; sh += 7
                if b < 0x80:
                    return v, i
        i = 0
        sym = None
        addr = 0
        while i < len(ops):
            b = ops[i]; i += 1
            op, imm = b & 0xf0, b & 0x0f
            if op == 0x00:          # DONE (lazy streams continue)
                continue
            elif op in (0x10, 0x30):    # SET_DYLIB_ORDINAL_IMM / SPECIAL_IMM
                pass
            elif op == 0x20:        # SET_DYLIB_ORDINAL_ULEB
                _, i = uleb(i)
            elif op == 0x40:        # SET_SYMBOL_TRAILING_FLAGS_IMM
                e = ops.index(b'\0', i)
                sym = ops[i:e].decode()
                i = e + 1
            elif op == 0x50:        # SET_TYPE_IMM
                pass
            elif op == 0x60:        # SET_ADDEND_SLEB
                while ops[i] & 0x80:
                    i += 1
                i += 1
            elif op == 0x70:        # SET_SEGMENT_AND_OFFSET_ULEB
                o, i = uleb(i)
                addr = segs[imm] + o
            elif op == 0x80:        # ADD_ADDR_ULEB
                o, i = uleb(i)
                addr = (addr + o) & 0xffffffff
            elif op == 0x90:        # DO_BIND
                self.binds[addr] = sym
                addr += 4
            elif op == 0xa0:        # DO_BIND_ADD_ADDR_ULEB
                self.binds[addr] = sym
                o, i = uleb(i)
                addr = (addr + 4 + o) & 0xffffffff
            elif op == 0xb0:        # DO_BIND_ADD_ADDR_IMM_SCALED
                self.binds[addr] = sym
                addr += 4 + imm * 4
            elif op == 0xc0:        # DO_BIND_ULEB_TIMES_SKIPPING_ULEB
                cnt, i = uleb(i)
                skip, i = uleb(i)
                for _ in range(cnt):
                    self.binds[addr] = sym
                    addr += 4 + skip
            else:
                raise ValueError('bind opcode %x' % op)

    def seg_for(self, addr):
        for s in self.segments:
            if s[1] <= addr < s[1] + s[4] and s[4]:
                return s
        return None

    def read(self, addr, n):
        s = self.seg_for(addr)
        if not s:
            return None
        o = s[3] + addr - s[1]
        return self.data[o:o+n]

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<I', b)[0] if b and len(b) == 4 else None

    def cstr(self, addr, maxlen=200):
        s = self.seg_for(addr)
        if not s:
            return None
        o = s[3] + addr - s[1]
        e = self.data.find(b'\0', o, o + maxlen)
        if e < 0:
            return None
        try:
            return self.data[o:e].decode('latin1')
        except Exception:
            return None

    def section_of(self, addr):
        for n, (a, sz, o) in self.sections.items():
            if a <= addr < a + sz:
                return n
        return None
