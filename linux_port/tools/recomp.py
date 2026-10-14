#!/usr/bin/env python3
"""Static recompiler: ARMv6 (ARM mode + VFPv2) Mach-O -> C.

Usage: recomp.py <armv6 macho> <outdir>
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO

NORETURN = {'___assert_rtn', '_exit', '_abort', '__Unwind_SjLj_Resume', '___cxa_throw',
            '___stack_chk_fail'}

# guest functions wrapped by host code: the recompiled body is emitted as fo_<addr>
# and the runtime provides f_<addr> (which normally calls fo_<addr>).
HOOKS = {0x386dc, 0x50e8, 0x2dd6c, 0x6ec8, 0x4df4, 0x1bbd8, 0x50c8, 0x2d934}

# single instructions replaced by host code (address -> C statement)
PATCHES = {
    # Per-car physics (0x3a2d8): the car's direction of travel is normalised into a stack
    # vector only when it moves (0x3a888), but the rolling friction (0x3bc1c) reads that
    # vector anyway.  For a car at rest it is uninitialised stack: harmless on the iPhone,
    # huge here, so cars starting on flat ground (China, Holland... and those attract
    # demos) could never move.  Clear it on the "not moving" path (original instruction:
    # ldr r0, [pc, #0xf5c]).
    0x3a90c: 'ST32(R[13] + 0x118u, 0); ST32(R[13] + 0x11cu, 0); ST32(R[13] + 0x120u, 0); '
             'R[0] = LD32(0x3b870u);',
}

COND = ['ZF', '!ZF', 'CF', '!CF', 'NF', '!NF', 'VF', '!VF',
        '(CF&&!ZF)', '(!CF||ZF)', '(NF==VF)', '(NF!=VF)', '(!ZF&&NF==VF)', '(ZF||NF!=VF)', None, None]


class DecodeError(Exception):
    pass


def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xffffffff if n else v


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


class Ins:
    """Decoded instruction.

    code   : C statements (unconditional part). May assign NPC for pc writes.
    cf     : None | 'b' | 'bl' | 'ret' | 'jump' (pc written, value in NPC) | 'trap'
    target : branch target for b/bl
    """
    def __init__(self, addr, word):
        self.addr = addr
        self.word = word
        self.cond = word >> 28
        self.code = ''
        self.cf = None
        self.target = None
        self.pclit = None      # pc-relative literal address read by this insn
        self.ldr_pc_from_sp = False
        self.mov_lr_pc = False
        self.text = ''


def reg(n, addr):
    """C expression reading register n (pc reads as addr+8)."""
    if n == 15:
        return '0x%xu' % ((addr + 8) & 0xffffffff)
    return 'R[%d]' % n


def shift_imm_expr(v, typ, amt, want_carry):
    """Return (value_expr, carry_expr) for immediate shift."""
    if typ == 0:  # LSL
        if amt == 0:
            return v, 'CF'
        return '(%s<<%d)' % (v, amt), '((%s>>%d)&1)' % (v, 32 - amt)
    if typ == 1:  # LSR
        if amt == 0:
            return '0u', '(%s>>31)' % v
        return '(%s>>%d)' % (v, amt), '((%s>>%d)&1)' % (v, amt - 1)
    if typ == 2:  # ASR
        if amt == 0:
            return '(uint32_t)((int32_t)%s>>31)' % v, '(%s>>31)' % v
        return '(uint32_t)((int32_t)%s>>%d)' % (v, amt), '((%s>>%d)&1)' % (v, amt - 1)
    # ROR / RRX
    if amt == 0:
        return '((CF<<31)|(%s>>1))' % v, '(%s&1)' % v
    return 'ROR32(%s,%d)' % (v, amt), '((%s>>%d)&1)' % (v, amt - 1)


def shift_reg_expr(v, typ, s):
    names = ['LSL', 'LSR', 'ASR', 'ROR']
    return 'SH_%s(%s,%s)' % (names[typ], v, s), 'SHC_%s(%s,%s)' % (names[typ], v, s)


def operand2(w, addr):
    """Data-processing operand 2. Returns (value_expr, carry_expr)."""
    if w & (1 << 25):
        imm8 = w & 0xff
        rot = bits(w, 11, 8) * 2
        val = ror(imm8, rot)
        carry = str(val >> 31) if rot else 'CF'
        return '0x%xu' % val, carry
    rm = w & 15
    typ = bits(w, 6, 5)
    if w & 0x10:
        rs = bits(w, 11, 8)
        v = reg(rm, addr + 4) if rm == 15 else reg(rm, addr)
        return shift_reg_expr(v, typ, '(%s&0xff)' % reg(rs, addr))
    return shift_imm_expr(reg(rm, addr), typ, bits(w, 11, 7), True)


DP_NAMES = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc',
            'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn']


def decode_dp(ins, w):
    a = ins.addr
    op = bits(w, 24, 21)
    s = (w >> 20) & 1
    rn = bits(w, 19, 16)
    rd = bits(w, 15, 12)
    o2, c2 = operand2(w, a)
    n = reg(rn, a)
    logical = op in (0, 1, 8, 9, 12, 13, 14, 15)
    arith = {2: ('%s', '~%s', '1'), 3: (None, None, None), 4: ('%s', '%s', '0'), 5: ('%s', '%s', 'CF'),
             6: ('%s', '~%s', 'CF'), 7: (None, None, None), 10: ('%s', '~%s', '1'), 11: ('%s', '%s', '0')}
    lines = []
    if logical:
        expr = {0: '%s & %s' % (n, 'op2'), 1: '%s ^ op2' % n, 8: '%s & op2' % n, 9: '%s ^ op2' % n,
                12: '%s | op2' % n, 13: 'op2', 14: '%s & ~op2' % n, 15: '~op2'}[op]
        lines.append('uint32_t op2 = %s;' % o2)
        if s and c2 != 'CF':
            # carry must be computed from the original operand before any write
            lines.insert(0, 'uint32_t sc = %s;' % c2)
        lines.append('uint32_t res = %s;' % expr)
        if s:
            lines.append('NF = res>>31; ZF = (res==0);')
            if c2 != 'CF':
                lines.append('CF = sc;')
    else:
        lines.append('uint32_t op2 = %s;' % o2)
        if op in (3, 7):  # rsb, rsc: op2 - rn
            cin = '1' if op == 3 else 'CF'
            if s:
                lines.append('uint32_t res = ADDC(op2, ~%s, %s);' % (n, cin))
            else:
                lines.append('uint32_t res = op2 + ~%s + %s;' % (n, cin))
        else:
            x, y, cin = arith[op]
            if s:
                lines.append('uint32_t res = ADDC(%s, %s, %s);' % (x % n, y % 'op2', cin))
            else:
                lines.append('uint32_t res = %s + %s + %s;' % (x % n, y % 'op2', cin))
    if op in (8, 9, 10, 11):
        if not s:
            raise DecodeError('dp test without S')
        ins.code = '{ %s }' % ' '.join(lines)
        return
    if rd == 15:
        if s:
            raise DecodeError('dp S to pc')
        if rn == 15 and (w & (1 << 25)) and op in (2, 4):   # add/sub pc, pc, #imm: static branch
            imm = ror(w & 0xff, bits(w, 11, 8) * 2)
            ins.cf = 'b'
            ins.target = (ins.addr + 8 + (imm if op == 4 else -imm)) & 0xffffffff
            ins.code = ''
            return
        lines.append('NPC = res;')
        ins.code = '{ %s }' % ' '.join(lines)
        ins.cf = 'jump'
        ins.dp_pc = (op, rn, w)
        if op == 13 and not (w & (1 << 25)) and (w & 0xff0) == 0 and (w & 15) == 14:
            ins.cf = 'ret'   # mov pc, lr
        return
    lines.append('R[%d] = res;' % rd)
    ins.code = '{ %s }' % ' '.join(lines)
    if op == 13 and rd == 14 and rn == 0 and not (w & (1 << 25)) and (w & 0xfff) == 15:
        ins.mov_lr_pc = True


def decode_ldst(ins, w):
    """LDR/STR/LDRB/STRB."""
    a = ins.addr
    I = (w >> 25) & 1
    P = (w >> 24) & 1
    U = (w >> 23) & 1
    B = (w >> 22) & 1
    W = (w >> 21) & 1
    L = (w >> 20) & 1
    rn = bits(w, 19, 16)
    rd = bits(w, 15, 12)
    if I:
        if w & 0x10:
            raise DecodeError('ldst reg-shift-reg')
        off, _ = shift_imm_expr(reg(w & 15, a), bits(w, 6, 5), bits(w, 11, 7), False)
    else:
        off = '%du' % (w & 0xfff)
    sign = '+' if U else '-'
    base = reg(rn, a)
    if rn == 15 and not I and P:
        ea = '0x%xu' % (((a + 8) + (w & 0xfff if U else -(w & 0xfff))) & 0xffffffff)
        ins.pclit = int(ea[2:-1], 16)
    else:
        ea = '(%s %s %s)' % (base, sign, off)
    lines = []
    if P:
        lines.append('uint32_t ea = %s;' % ea)
        wb = 'R[%d] = ea;' % rn if W else ''
    else:
        if W:
            raise DecodeError('ldrt/strt')
        lines.append('uint32_t ea = %s;' % base)
        wb = 'R[%d] = %s %s %s;' % (rn, base, sign, off)
    if rn == 15 and (W or not P):
        raise DecodeError('pc writeback')
    if L:
        val = 'LD8(ea)' if B else 'LD32(ea)'
        if rd == 15:
            lines.append('NPC = %s;' % val)
            if wb:
                lines.append(wb)
            ins.cf = 'jump'
            if rn == 13 and not P and U:
                ins.cf = 'ret'
            ins.ldr_pc = (rn, P, U, I, w)
        else:
            lines.append('uint32_t v = %s;' % val)
            if wb and rd != rn:
                lines.append(wb)
            lines.append('R[%d] = v;' % rd)
    else:
        src = reg(rd, a + 4) if rd == 15 else 'R[%d]' % rd
        lines.append(('ST8(ea, %s);' if B else 'ST32(ea, %s);') % src)
        if wb:
            lines.append(wb)
    ins.code = '{ %s }' % ' '.join(lines)


def decode_extra_ldst(ins, w):
    a = ins.addr
    P = (w >> 24) & 1
    U = (w >> 23) & 1
    I = (w >> 22) & 1
    W = (w >> 21) & 1
    L = (w >> 20) & 1
    rn = bits(w, 19, 16)
    rd = bits(w, 15, 12)
    sh = bits(w, 6, 5)
    if I:
        off = '%du' % (((w >> 4) & 0xf0) | (w & 15))
    else:
        off = reg(w & 15, a)
    sign = '+' if U else '-'
    base = reg(rn, a)
    lines = []
    if P:
        if rn == 15 and I:
            v = ((a + 8) + ((((w >> 4) & 0xf0) | (w & 15)) * (1 if U else -1))) & 0xffffffff
            lines.append('uint32_t ea = 0x%xu;' % v)
            ins.pclit = v
        else:
            lines.append('uint32_t ea = %s %s %s;' % (base, sign, off))
        wb = 'R[%d] = ea;' % rn if W else ''
    else:
        lines.append('uint32_t ea = %s;' % base)
        wb = 'R[%d] = %s %s %s;' % (rn, base, sign, off)
    if rd == 15:
        raise DecodeError('extra ldst pc')
    if L:
        fn = {1: 'LD16', 2: 'LDS8', 3: 'LDS16'}[sh]
        lines.append('uint32_t v = %s(ea);' % fn)
        if wb and rd != rn:
            lines.append(wb)
        lines.append('R[%d] = v;' % rd)
    else:
        if sh == 1:
            lines.append('ST16(ea, R[%d]);' % rd)
            if wb:
                lines.append(wb)
        elif sh == 2:  # LDRD
            if rd & 1:
                raise DecodeError('ldrd odd')
            lines.append('uint32_t v0 = LD32(ea), v1 = LD32(ea+4);')
            if wb:
                lines.append(wb)
            lines.append('R[%d] = v0; R[%d] = v1;' % (rd, rd + 1))
        else:  # STRD
            if rd & 1:
                raise DecodeError('strd odd')
            lines.append('ST32(ea, R[%d]); ST32(ea+4, R[%d]);' % (rd, rd + 1))
            if wb:
                lines.append(wb)
    ins.code = '{ %s }' % ' '.join(lines)


def decode_ldm(ins, w):
    P = (w >> 24) & 1
    U = (w >> 23) & 1
    S = (w >> 22) & 1
    W = (w >> 21) & 1
    L = (w >> 20) & 1
    rn = bits(w, 19, 16)
    rl = [i for i in range(16) if w & (1 << i)]
    n = len(rl)
    if rn == 15:
        raise DecodeError('ldm pc base')
    if S and not (L and 15 in rl):
        raise DecodeError('ldm/stm user regs')
    lines = ['uint32_t base = R[%d];' % rn]
    if U:
        start = 'base + 4' if P else 'base'
        newb = 'base + %d' % (4 * n)
    else:
        start = 'base - %d' % (4 * n) if P else 'base - %d' % (4 * n - 4)
        newb = 'base - %d' % (4 * n)
    lines.append('uint32_t ea = %s;' % start)
    if L:
        for i, r in enumerate(rl):
            if r == 15:
                lines.append('NPC = LD32(ea+%d);' % (4 * i))
            else:
                lines.append('uint32_t v%d = LD32(ea+%d);' % (r, 4 * i))
        if W and rn not in rl:
            lines.append('R[%d] = %s;' % (rn, newb))
        for r in rl:
            if r != 15:
                lines.append('R[%d] = v%d;' % (r, r))
        if 15 in rl:
            ins.cf = 'jump'
            if rn == 13:
                ins.cf = 'ret'
    else:
        for i, r in enumerate(rl):
            src = '0x%xu' % (ins.addr + 12) if r == 15 else 'R[%d]' % r
            lines.append('ST32(ea+%d, %s);' % (4 * i, src))
        if W:
            lines.append('R[%d] = %s;' % (rn, newb))
    ins.code = '{ %s }' % ' '.join(lines)


def decode_mul(ins, w):
    op = bits(w, 27, 21)
    S = (w >> 20) & 1
    rd = bits(w, 19, 16)
    rn = bits(w, 15, 12)
    rs = bits(w, 11, 8)
    rm = w & 15
    if bits(w, 27, 22) == 0:  # MUL / MLA
        A = (w >> 21) & 1
        e = 'R[%d] * R[%d]' % (rm, rs)
        if A:
            e += ' + R[%d]' % rn
        c = 'uint32_t res = %s; R[%d] = res;' % (e, rd)
        if S:
            c += ' NF = res>>31; ZF = (res==0);'
        ins.code = '{ %s }' % c
        return
    if bits(w, 27, 23) == 1:  # long multiply
        U = (w >> 22) & 1   # 1 = signed
        A = (w >> 21) & 1
        hi, lo = rd, rn
        if U:
            e = '(uint64_t)((int64_t)(int32_t)R[%d] * (int64_t)(int32_t)R[%d])' % (rm, rs)
        else:
            e = '((uint64_t)R[%d] * (uint64_t)R[%d])' % (rm, rs)
        if A:
            e += ' + (((uint64_t)R[%d]<<32)|R[%d])' % (hi, lo)
        c = 'uint64_t res = %s; R[%d] = (uint32_t)res; R[%d] = (uint32_t)(res>>32);' % (e, lo, hi)
        if S:
            c += ' NF = (uint32_t)(res>>63); ZF = (res==0);'
        ins.code = '{ %s }' % c
        return
    if bits(w, 27, 20) == 0x04:  # UMAAL
        c = 'uint64_t res = (uint64_t)R[%d]*R[%d] + R[%d] + R[%d]; R[%d]=(uint32_t)res; R[%d]=(uint32_t)(res>>32);' % (
            rm, rs, rn, rd, rn, rd)
        ins.code = '{ %s }' % c
        return
    if bits(w, 27, 23) == 2 and bits(w, 21, 20) == 0:  # SWP
        B = (w >> 22) & 1
        if B:
            c = 'uint32_t t = LD8(R[%d]); ST8(R[%d], R[%d]); R[%d] = t;' % (rd, rd, rm, rn)
        else:
            c = 'uint32_t t = LD32(R[%d]); ST32(R[%d], R[%d]); R[%d] = t;' % (rd, rd, rm, rn)
        ins.code = '{ %s }' % c
        return
    raise DecodeError('mul-like %08x' % w)


def decode_misc(ins, w):
    """bits27-23 = 00010, S=0 (op=0)."""
    op = bits(w, 22, 21)
    op2 = bits(w, 7, 4)
    if op2 == 0:
        if op & 1 == 0:  # MRS
            rd = bits(w, 15, 12)
            ins.code = 'R[%d] = (NF<<31)|(ZF<<30)|(CF<<29)|(VF<<28)|0x10;' % rd
            return
        # MSR reg
        mask = bits(w, 19, 16)
        rm = w & 15
        if mask & 8:
            ins.code = '{ uint32_t v = R[%d]; NF=v>>31; ZF=(v>>30)&1; CF=(v>>29)&1; VF=(v>>28)&1; }' % rm
        else:
            ins.code = ';'
        return
    if op2 == 1 and op == 1:  # BX
        rm = w & 15
        if rm == 14:
            ins.cf = 'ret'
            ins.code = 'NPC = R[14];'
        else:
            ins.cf = 'jump'
            ins.code = 'NPC = R[%d];' % rm
            ins.bx = rm
        return
    if op2 == 1 and op == 3:  # CLZ
        ins.code = 'R[%d] = CLZ32(R[%d]);' % (bits(w, 15, 12), w & 15)
        return
    if op2 == 3 and op == 1:  # BLX reg
        rm = w & 15
        ins.cf = 'blxr'
        ins.code = 'NPC = R[%d]; R[14] = 0x%xu;' % (rm, ins.addr + 4)
        return
    if op2 & 9 == 8:  # signed halfword multiplies
        rd = bits(w, 19, 16)
        rn = bits(w, 15, 12)
        rs = bits(w, 11, 8)
        rm = w & 15
        x = (w >> 5) & 1
        y = (w >> 6) & 1
        hm = '(int32_t)(int16_t)(R[%d]%s)' % (rm, '>>16' if x else '')
        hs = '(int32_t)(int16_t)(R[%d]%s)' % (rs, '>>16' if y else '')
        if op == 0:  # SMLAxy
            ins.code = 'R[%d] = (uint32_t)(%s * %s) + R[%d];' % (rd, hm, hs, rn)
        elif op == 3:  # SMULxy
            ins.code = 'R[%d] = (uint32_t)(%s * %s);' % (rd, hm, hs)
        elif op == 1:
            prod = '(uint32_t)(((int64_t)(int32_t)R[%d] * %s) >> 16)' % (rm, hs)
            if x:  # SMULWy
                ins.code = 'R[%d] = %s;' % (rd, prod)
            else:  # SMLAWy
                ins.code = 'R[%d] = %s + R[%d];' % (rd, prod, rn)
        else:  # SMLALxy
            ins.code = '{ int64_t acc = (int64_t)(((uint64_t)R[%d]<<32)|R[%d]); acc += (int64_t)%s * %s; R[%d]=(uint32_t)acc; R[%d]=(uint32_t)((uint64_t)acc>>32); }' % (
                rd, rn, hm, hs, rn, rd)
        return
    raise DecodeError('misc %08x' % w)


def decode_media(ins, w):
    """op=011, bit4=1."""
    a = ins.addr
    b2720 = bits(w, 27, 20)
    b74 = bits(w, 7, 4)
    rd = bits(w, 15, 12)
    rn = bits(w, 19, 16)
    rm = w & 15
    if bits(w, 27, 23) == 0b01101 and b74 == 0b0111:
        # extend
        rot = bits(w, 11, 10) * 8
        src = 'ROR32(R[%d],%d)' % (rm, rot) if rot else 'R[%d]' % rm
        k = bits(w, 22, 20)
        ext = {0b010: '(uint32_t)(int32_t)(int8_t)(%s)', 0b011: '(uint32_t)(int32_t)(int16_t)(%s)',
               0b110: '(uint32_t)(uint8_t)(%s)', 0b111: '(uint32_t)(uint16_t)(%s)'}.get(k)
        if ext is None:
            raise DecodeError('extend16 %08x' % w)
        e = ext % src
        if rn != 15:
            e = 'R[%d] + %s' % (rn, e)
        ins.code = 'R[%d] = %s;' % (rd, e)
        return
    if b2720 == 0b01101011 and b74 == 0b0011:
        ins.code = 'R[%d] = __builtin_bswap32(R[%d]);' % (rd, rm)
        return
    if b2720 == 0b01101011 and b74 == 0b1011:
        ins.code = '{ uint32_t v = R[%d]; R[%d] = ((v&0x00ff00ffu)<<8)|((v>>8)&0x00ff00ffu); }' % (rm, rd)
        return
    if b2720 == 0b01101111 and b74 == 0b1011:
        ins.code = '{ uint32_t v = R[%d]; R[%d] = (uint32_t)(int32_t)(int16_t)(((v&0xff)<<8)|((v>>8)&0xff)); }' % (rm, rd)
        return
    if bits(w, 27, 21) in (0b0110101, 0b0110111) and (w >> 4) & 3 == 1:  # SSAT/USAT
        sat = bits(w, 20, 16)
        sh = bits(w, 11, 7)
        sv = '(int32_t)R[%d]' % rm
        if (w >> 6) & 1:
            sv = '((int32_t)R[%d] >> %d)' % (rm, sh if sh else 31)
        elif sh:
            sv = '(int32_t)(R[%d] << %d)' % (rm, sh)
        if bits(w, 22, 21) == 0b11:  # USAT
            ins.code = 'R[%d] = USAT(%s, %d);' % (rd, sv, sat)
        else:
            ins.code = 'R[%d] = SSAT(%s, %d);' % (rd, sv, sat + 1)
        return
    if b2720 == 0b01110101:  # SMMUL/SMMLA/SMMLS
        rdd = rn          # bits 19-16
        ra = rd           # bits 15-12
        rs = bits(w, 11, 8)
        R = (w >> 5) & 1
        op = bits(w, 7, 6)
        prod = '((int64_t)(int32_t)R[%d] * (int64_t)(int32_t)R[%d])' % (rm, rs)
        rnd = ' + 0x80000000LL' if R else ''
        if op == 0:
            if ra == 15:
                e = '(uint32_t)((uint64_t)(%s%s) >> 32)' % (prod, rnd)
            else:
                e = '(uint32_t)((uint64_t)((((int64_t)R[%d])<<32) + %s%s) >> 32)' % (ra, prod, rnd)
        elif op == 3:
            e = '(uint32_t)((uint64_t)((((int64_t)R[%d])<<32) - %s%s) >> 32)' % (ra, prod, rnd)
        else:
            raise DecodeError('smm %08x' % w)
        ins.code = 'R[%d] = %s;' % (rdd, e)
        return
    if b2720 == 0b01110000:  # SMLAD/SMUAD/SMLSD/SMUSD
        rdd = rn
        ra = rd
        rs = bits(w, 11, 8)
        X = (w >> 5) & 1
        sub = (w >> 6) & 1
        m = 'ROR32(R[%d],16)' % rs if X else 'R[%d]' % rs
        p1 = '((int32_t)(int16_t)R[%d] * (int32_t)(int16_t)(%s))' % (rm, m)
        p2 = '((int32_t)(int16_t)(R[%d]>>16) * (int32_t)(int16_t)((%s)>>16))' % (rm, m)
        e = '(uint32_t)(%s %s %s)' % (p1, '-' if sub else '+', p2)
        if ra != 15:
            e += ' + R[%d]' % ra
        ins.code = 'R[%d] = %s;' % (rdd, e)
        return
    if b2720 == 0b01101000 and (b74 & 3) == 1:  # PKHBT/PKHTB
        sh = bits(w, 11, 7)
        tb = (w >> 6) & 1
        if tb:
            sv = '(uint32_t)((int32_t)R[%d] >> %d)' % (rm, sh if sh else 31)
            ins.code = 'R[%d] = (R[%d] & 0xffff0000u) | (%s & 0xffffu);' % (rd, rn, sv)
        else:
            ins.code = 'R[%d] = (R[%d] & 0xffffu) | ((R[%d] << %d) & 0xffff0000u);' % (rd, rn, rm, sh)
        return
    if bits(w, 27, 21) == 0b0111111 and b74 & 7 == 5:  # UBFX (v6T2) - unlikely
        raise DecodeError('ubfx')
    if w & 0x0ff000f0 == 0x07f000f0:  # UDF
        ins.cf = 'trap'
        ins.code = ''
        return
    raise DecodeError('media %08x' % w)


# ---------------------------------------------------------------- VFP

def sreg(v, b):
    return (v << 1) | b


def vfp_dp(ins, w):
    """CDP-type VFP data processing (cp10/cp11)."""
    dbl = bits(w, 11, 8) == 11
    p = (w >> 23) & 1
    q = (w >> 21) & 1
    r = (w >> 20) & 1
    s = (w >> 6) & 1
    D = (w >> 22) & 1
    N = (w >> 7) & 1
    M = (w >> 5) & 1
    vd, vn, vm = bits(w, 15, 12), bits(w, 19, 16), w & 15
    if dbl:
        Fd, Fn, Fm = 'FD[%d]' % vd, 'FD[%d]' % vn, 'FD[%d]' % vm
        pqrs0 = (p << 3) | (q << 2) | (r << 1) | s
        if (M and not (pqrs0 == 15 and ((vn << 1) | N) in (16, 17))) or (pqrs0 != 15 and (D or N)) or (pqrs0 == 15 and D and ((vn << 1) | N) not in (15, 24, 25, 26, 27)):
            raise DecodeError('vfp d>15')
    else:
        Fd, Fn, Fm = 'FS[%d]' % sreg(vd, D), 'FS[%d]' % sreg(vn, N), 'FS[%d]' % sreg(vm, M)
    pqrs = (p << 3) | (q << 2) | (r << 1) | s
    if pqrs == 0:
        ins.code = '%s = %s + (%s * %s);' % (Fd, Fd, Fn, Fm)
    elif pqrs == 1:
        ins.code = '%s = %s - (%s * %s);' % (Fd, Fd, Fn, Fm)
    elif pqrs == 2:  # FNMSC: Fd = -Fd + Fn*Fm
        ins.code = '%s = -%s + (%s * %s);' % (Fd, Fd, Fn, Fm)
    elif pqrs == 3:  # FNMAC: Fd = -Fd - Fn*Fm
        ins.code = '%s = -%s - (%s * %s);' % (Fd, Fd, Fn, Fm)
    elif pqrs == 4:
        ins.code = '%s = %s * %s;' % (Fd, Fn, Fm)
    elif pqrs == 5:
        ins.code = '%s = -(%s * %s);' % (Fd, Fn, Fm)
    elif pqrs == 6:
        ins.code = '%s = %s + %s;' % (Fd, Fn, Fm)
    elif pqrs == 7:
        ins.code = '%s = %s - %s;' % (Fd, Fn, Fm)
    elif pqrs == 8:
        ins.code = '%s = %s / %s;' % (Fd, Fn, Fm)
    elif pqrs == 15:
        ext = (vn << 1) | N
        T = 'double' if dbl else 'float'
        if ext == 0:
            ins.code = '%s = %s;' % (Fd, Fm)
        elif ext == 1:
            ins.code = '%s = fabs%s(%s);' % (Fd, '' if dbl else 'f', Fm)
        elif ext == 2:
            ins.code = '%s = -%s;' % (Fd, Fm)
        elif ext == 3:
            ins.code = '%s = sqrt%s(%s);' % (Fd, '' if dbl else 'f', Fm)
        elif ext in (8, 9):
            ins.code = 'VFP_CMP(%s, %s);' % (Fd, Fm)
        elif ext in (10, 11):
            ins.code = 'VFP_CMP(%s, (%s)0);' % (Fd, T)
        elif ext == 15:  # convert precision
            if dbl:  # double -> single: Sd = Dm
                ins.code = 'FS[%d] = (float)FD[%d];' % (sreg(vd, D), vm)
            else:     # single -> double: Dd = Sm
                ins.code = 'FD[%d] = (double)FS[%d];' % (vd, sreg(vm, M))
        elif ext in (16, 17):  # UITO / SITO, source single
            src = 'FU[%d]' % sreg(vm, M)
            cast = '(int32_t)' if ext == 17 else ''
            ins.code = '%s = (%s)%s%s;' % (Fd, T, cast, src)
        elif ext in (24, 25, 26, 27):  # TOUI(Z) / TOSI(Z), dest single
            dst = 'FU[%d]' % sreg(vd, D)
            signed = ext >= 26
            z = ext & 1
            fn = ('VCVT_%s%s_%s' % ('S' if signed else 'U', 'Z' if z else 'R', 'D' if dbl else 'F'))
            ins.code = '%s = %s(%s);' % (dst, fn, Fm)
        else:
            raise DecodeError('vfp ext %d' % ext)
    else:
        raise DecodeError('vfp dp %d' % pqrs)


def vfp_ldst(ins, w):
    a = ins.addr
    P = (w >> 24) & 1
    U = (w >> 23) & 1
    D = (w >> 22) & 1
    W = (w >> 21) & 1
    L = (w >> 20) & 1
    rn = bits(w, 19, 16)
    vd = bits(w, 15, 12)
    dbl = bits(w, 11, 8) == 11
    imm8 = w & 0xff
    if bits(w, 27, 21) == 0b1100010:  # MCRR/MRRC: vmov 2 core regs
        rt2 = rn
        rt = vd
        vm = w & 15
        M = (w >> 5) & 1
        if dbl:
            if M:
                raise DecodeError('vmov d>15')
            if L:
                ins.code = 'R[%d] = FU[%d]; R[%d] = FU[%d];' % (rt, 2 * vm, rt2, 2 * vm + 1)
            else:
                ins.code = 'FU[%d] = R[%d]; FU[%d] = R[%d];' % (2 * vm, rt, 2 * vm + 1, rt2)
        else:
            sm = sreg(vm, M)
            if L:
                ins.code = 'R[%d] = FU[%d]; R[%d] = FU[%d];' % (rt, sm, rt2, sm + 1)
            else:
                ins.code = 'FU[%d] = R[%d]; FU[%d] = R[%d];' % (sm, rt, sm + 1, rt2)
        return
    first = 2 * vd if dbl else sreg(vd, D)
    if dbl and D:
        raise DecodeError('vldst d>15')
    if P and not W:  # VLDR/VSTR
        off = imm8 * 4
        if rn == 15:
            ea = '0x%xu' % (((a + 8) & ~3) + (off if U else -off))
            ins.pclit = int(ea[2:-1], 16)
            if dbl:
                ins.pclit2 = ins.pclit + 4
        else:
            ea = '(R[%d] %s %d)' % (rn, '+' if U else '-', off)
        if L:
            if dbl:
                ins.code = '{ uint32_t ea = %s; FU[%d] = LD32(ea); FU[%d] = LD32(ea+4); }' % (ea, first, first + 1)
            else:
                ins.code = 'FU[%d] = LD32(%s);' % (first, ea)
        else:
            if dbl:
                ins.code = '{ uint32_t ea = %s; ST32(ea, FU[%d]); ST32(ea+4, FU[%d]); }' % (ea, first, first + 1)
            else:
                ins.code = 'ST32(%s, FU[%d]);' % (ea, first)
        return
    # VLDM/VSTM
    nwords = (imm8 & ~1) if dbl else imm8
    if rn == 15:
        raise DecodeError('vldm pc')
    lines = ['uint32_t base = R[%d];' % rn]
    if P and not U and W:  # DB with writeback
        lines.append('uint32_t ea = base - %d;' % (imm8 * 4))
        newb = 'base - %d' % (imm8 * 4)
    elif not P and U:
        lines.append('uint32_t ea = base;')
        newb = 'base + %d' % (imm8 * 4)
    else:
        raise DecodeError('vldm mode')
    for i in range(nwords):
        if L:
            lines.append('FU[%d] = LD32(ea+%d);' % (first + i, 4 * i))
        else:
            lines.append('ST32(ea+%d, FU[%d]);' % (4 * i, first + i))
    if W:
        lines.append('R[%d] = %s;' % (rn, newb))
    ins.code = '{ %s }' % ' '.join(lines)


def vfp_xfer(ins, w):
    """MCR/MRC with cp10/11."""
    L = (w >> 20) & 1
    rt = bits(w, 15, 12)
    opc1 = bits(w, 23, 21)
    vn = bits(w, 19, 16)
    N = (w >> 7) & 1
    cp = bits(w, 11, 8)
    if cp == 10 and opc1 == 0:
        sn = sreg(vn, N)
        if L:
            if rt == 15:
                raise DecodeError('vmov to pc')
            ins.code = 'R[%d] = FU[%d];' % (rt, sn)
        else:
            ins.code = 'FU[%d] = R[%d];' % (sn, rt)
        return
    if cp == 10 and opc1 == 7:
        if vn == 1:
            if L:
                if rt == 15:
                    ins.code = 'NF = FNF; ZF = FZF; CF = FCF; VF = FVF;'
                else:
                    ins.code = 'R[%d] = (FNF<<31)|(FZF<<30)|(FCF<<29)|(FVF<<28)|(FPSCR_REST);' % rt
            else:
                ins.code = '{ uint32_t v = R[%d]; FNF=v>>31; FZF=(v>>30)&1; FCF=(v>>29)&1; FVF=(v>>28)&1; FPSCR_REST = v & 0x0fffffff; }' % rt
            return
        if L and vn in (0, 6, 7):  # FPSID / MVFR
            ins.code = 'R[%d] = 0x410120b4u;' % rt
            return
    if cp == 11 and opc1 in (0, 1):  # vmov.32 Dn[x], Rt
        idx = 2 * vn + opc1
        if N:
            raise DecodeError('vmov d>15')
        if L:
            ins.code = 'R[%d] = FU[%d];' % (rt, idx)
        else:
            ins.code = 'FU[%d] = R[%d];' % (idx, rt)
        return
    raise DecodeError('vfp xfer %08x' % w)


def decode(addr, w):
    ins = Ins(addr, w)
    cond = w >> 28
    if cond == 0xF:
        if (w & 0xfd70f000) == 0xf550f000:  # PLD
            ins.cond = 14
            ins.code = ';'
            return ins
        raise DecodeError('uncond %08x' % w)
    op = bits(w, 27, 25)
    if op in (0, 1):
        if op == 0 and (w & 0x90) == 0x90:
            if bits(w, 6, 5) == 0:
                decode_mul(ins, w)
            else:
                decode_extra_ldst(ins, w)
        elif bits(w, 24, 23) == 2 and not (w & (1 << 20)):
            if op == 0:
                decode_misc(ins, w)
            else:
                if (w >> 21) & 1:   # MSR imm
                    mask = bits(w, 19, 16)
                    v = ror(w & 0xff, bits(w, 11, 8) * 2)
                    if mask & 8:
                        ins.code = 'NF=%d; ZF=%d; CF=%d; VF=%d;' % (v >> 31, (v >> 30) & 1, (v >> 29) & 1, (v >> 28) & 1)
                    else:
                        ins.code = ';'
                else:
                    raise DecodeError('undef %08x' % w)
        else:
            decode_dp(ins, w)
    elif op in (2, 3):
        if op == 3 and (w & 0x10):
            decode_media(ins, w)
        else:
            decode_ldst(ins, w)
    elif op == 4:
        decode_ldm(ins, w)
    elif op == 5:
        off = w & 0xffffff
        if off & 0x800000:
            off -= 0x1000000
        ins.target = (addr + 8 + off * 4) & 0xffffffff
        ins.cf = 'bl' if w & (1 << 24) else 'b'
    elif op == 6:
        cp = bits(w, 11, 8)
        if cp not in (10, 11):
            raise DecodeError('ldc %08x' % w)
        vfp_ldst(ins, w)
    else:
        if w & (1 << 24):
            ins.cf = 'trap'
            ins.code = ''
            return ins
        cp = bits(w, 11, 8)
        if cp not in (10, 11):
            raise DecodeError('cdp %08x' % w)
        if w & 0x10:
            vfp_xfer(ins, w)
        else:
            vfp_dp(ins, w)
    if ins.cond == 15:
        raise DecodeError('nv')
    return ins


# ---------------------------------------------------------------- analysis

class Program:
    def __init__(self, path):
        self.m = MachO(path)
        t = self.m.sections['__text']
        self.text_lo, self.text_hi = t[0], t[0] + t[1]
        self.ins = {}
        self.data_words = set()
        self.func_entries = set()
        self.bad = {}
        self.objc_imps = {}

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi and a % 4 == 0

    def get(self, a):
        if a in self.ins:
            return self.ins[a]
        if not self.in_text(a):
            return None
        w = self.m.u32(a)
        try:
            i = decode(a, w)
        except DecodeError as e:
            self.bad[a] = str(e)
            return None
        self.ins[a] = i
        return i

    def seeds(self):
        m = self.m
        s = {m.entry}
        # objc method lists
        for n in ('__objc_const',):
            a, sz, _ = m.sections[n]
        # scan data sections for code pointers
        for name, (a, sz, off) in m.sections.items():
            if name in ('__text', '__stub_helper', '__symbolstub1', '__cstring', '__objc_methname',
                        '__objc_classname', '__objc_methtype'):
                continue
            if m.seg_for(a) is None:
                continue
            for x in range(a, a + sz - 3, 4):
                v = m.u32(x)
                if v is not None and self.in_text(v):
                    w = m.u32(v)
                    # objc method lists are trusted; elsewhere require a prologue (push {...,lr})
                    if name == '__objc_const' or (w & 0x0fff4000) == 0x092d4000:
                        s.add(v)
        return s

    def explore(self):
        work = list(self.seeds())
        self.func_entries |= set(work)
        seen = set()
        # first pass: mark literal pools so that we never decode them as code
        while work:
            a = work.pop()
            while True:
                if a in seen or not self.in_text(a):
                    break
                i = self.get(a)
                if i is None:
                    break
                seen.add(a)
                for lit in (i.pclit, getattr(i, 'pclit2', None)):
                    if lit is not None and self.in_text(lit):
                        self.data_words.add(lit)
                if i.cf == 'bl':
                    if self.in_text(i.target):
                        if i.target not in self.func_entries:
                            self.func_entries.add(i.target)
                            work.append(i.target)
                    elif i.target in self.m.stubs and self.m.stubs[i.target] in NORETURN and i.cond == 14:
                        break
                elif i.cf == 'b':
                    if self.in_text(i.target):
                        work.append(i.target)
                    if i.cond == 14:
                        break
                elif i.cf in ('ret', 'trap'):
                    if i.cond == 14:
                        break
                elif i.cf == 'jump':
                    tbl = self.jump_table(a)
                    if tbl is not None:
                        work.extend(tbl)
                    if i.cond == 14:
                        break
                a += 4
        self.code = seen
        # remove data words that were decoded as code? report overlaps
        ov = self.code & self.data_words
        if ov:
            print('warning: %d addresses both code and literal' % len(ov), file=sys.stderr)

    def jump_table(self, a):
        """Recognise: cmp rX,#N ; bhi dflt ; lsl r1,rX,#2 ; add r2,pc,#4 ; ldr r1,[r1,r2] ; add pc,r1,r2"""
        i = self.ins.get(a)
        if not hasattr(i, 'dp_pc'):
            return None
        op, rn, w = i.dp_pc
        if op != 4 or w & (1 << 25) or w & 0xff0:
            return None
        rm = w & 15
        # find 'add rX, pc, #imm' (rX is either operand) and 'lsl rY, rIdx, #2'
        base = None
        idx = None
        count = None
        for back in range(1, 12):
            pa = a - 4 * back
            pw = self.m.u32(pa)
            if pw is None:
                break
            if (pw & 0x0fff0000) == 0x028f0000 and bits(pw, 15, 12) in (rm, rn) and base is None:
                base = (pa + 8 + ror(pw & 0xff, bits(pw, 11, 8) * 2)) & 0xffffffff
            if (pw & 0x0fff0ff0) == 0x01a00100 and idx is None:      # mov rY, rIdx, lsl #2
                idx = pw & 15
        if base is None:
            return None
        if idx is not None:
            for back in range(1, 48):
                pw = self.m.u32(a - 4 * back)
                if pw is None:
                    break
                if (pw & 0x0ff0f000) == 0x03500000 and bits(pw, 19, 16) == idx:   # cmp rIdx, #N
                    count = ror(pw & 0xff, bits(pw, 11, 8) * 2) + 1
                    break
                if (pw >> 28) == 14 and (pw & 0x0e000000) == 0x0a000000:
                    break   # unconditional branch: we left the basic block chain
        if count is None:
            count = 0
            while count < 256:
                off = self.m.u32(base + 4 * count)
                if off is None or off % 4 or off >= 0x10000 or not self.in_text(base + off):
                    break
                count += 1
        targets = []
        for k in range(count):
            off = self.m.u32(base + 4 * k)
            self.data_words.add(base + 4 * k)
            targets.append((base + off) & 0xffffffff)
        i.table = targets
        return targets

    def func_body(self, entry):
        body = []
        seen = set()
        work = [entry]
        labels = {entry}
        while work:
            a = work.pop()
            while True:
                if a in seen:
                    labels.add(a)
                    break
                if a != entry and a in self.func_entries:
                    break   # fall-through/branch into another function: handled by caller
                i = self.ins.get(a)
                if i is None:
                    break
                seen.add(a)
                body.append(a)
                stop = False
                if i.cf == 'b' and self.in_text(i.target) and i.target not in self.func_entries or \
                        (i.cf == 'b' and i.target == entry):
                    labels.add(i.target)
                    work.append(i.target)
                    stop = i.cond == 14
                elif i.cf == 'b':
                    stop = i.cond == 14
                elif i.cf == 'bl':
                    if i.target in self.m.stubs and self.m.stubs[i.target] in NORETURN and i.cond == 14:
                        stop = True
                elif i.cf in ('ret', 'trap'):
                    stop = i.cond == 14
                elif i.cf == 'jump':
                    tbl = getattr(i, 'table', None)
                    if tbl:
                        for t in tbl:
                            if t not in self.func_entries:
                                labels.add(t)
                                work.append(t)
                    stop = i.cond == 14
                if stop:
                    break
                a += 4
        body.sort()
        return body, labels


# ---------------------------------------------------------------- emission

def fname(a):
    return 'f_%x' % a


def emit_function(p, entry, out):
    body, labels = p.func_body(entry)
    m = p.m
    out.append('void %s%s(void) {' % ('fo_%x' % entry if entry in HOOKS else fname(entry), ''))
    out.append('  uint32_t NPC; (void)NPC;')
    bodyset = set(body)
    prev_end = None
    for idx, a in enumerate(body):
        i = p.ins[a]
        if prev_end is not None and prev_end != a:
            pass
        if a in labels:
            out.append('L_%x:' % a)
        cond = COND[i.cond] if i.cond != 14 else None
        stmts = []
        nxt = a + 4
        if a in PATCHES:
            stmts.append(PATCHES[a])
        elif i.cf is None:
            stmts.append(i.code)
        elif i.cf == 'trap':
            stmts.append('guest_trap(0x%xu);' % a)
        elif i.cf == 'bl':
            stmts.append('R[14] = 0x%xu;' % nxt)
            t = i.target
            if t in m.stubs:
                stmts.append('H%s();' % m.stubs[t])
                if m.stubs[t] in NORETURN:
                    stmts.append('guest_trap(0x%xu);' % a)
            elif p.in_text(t):
                stmts.append('%s();' % fname(t))
            else:
                stmts.append('guest_bad_call(0x%xu, 0x%xu);' % (a, t))
        elif i.cf == 'b':
            t = i.target
            if t in m.stubs:
                stmts.append('H%s(); return;' % m.stubs[t])
            elif t == entry or (p.in_text(t) and t not in p.func_entries):
                stmts.append('goto L_%x;' % t)
            elif p.in_text(t):
                stmts.append('%s(); return;' % fname(t))
            else:
                stmts.append('guest_bad_call(0x%xu, 0x%xu); return;' % (a, t))
        elif i.cf == 'ret':
            stmts.append(i.code)
            stmts.append('return;')
        elif i.cf == 'blxr':
            stmts.append(i.code)
            stmts.append('call_indirect(NPC);')
        elif i.cf == 'jump':
            stmts.append(i.code)
            prev = p.ins.get(a - 4)
            tbl = getattr(i, 'table', None)
            if prev is not None and prev.mov_lr_pc and (a - 4) in bodyset:
                stmts.append('call_indirect(NPC);')
            elif tbl:
                cases = ' '.join('case 0x%xu: goto L_%x;' % (t, t) if t not in p.func_entries
                                 else 'case 0x%xu: %s(); return;' % (t, fname(t)) for t in sorted(set(tbl)))
                stmts.append('switch (NPC) { %s default: guest_bad_jump(0x%xu, NPC); }' % (cases, a))
            else:
                stmts.append('jump_indirect(NPC); return;')
        s = ' '.join(stmts)
        if cond:
            out.append('  if (%s) { %s }' % (cond, s))
        else:
            out.append('  ' + s)
        # fall through into code that is not part of this body
        nxt_in = (idx + 1 < len(body) and body[idx + 1] == nxt)
        uncond_end = (i.cond == 14 and (i.cf in ('ret', 'trap') or
                      (i.cf == 'b') or (i.cf == 'jump' and not (prev is not None and prev.mov_lr_pc)) or
                      (i.cf == 'bl' and i.target in m.stubs and m.stubs[i.target] in NORETURN)))
        if not uncond_end and not nxt_in:
            if nxt in p.func_entries:
                out.append('  %s(); return;' % fname(nxt))
            elif nxt in bodyset:
                out.append('  goto L_%x;' % nxt)
            else:
                out.append('  guest_trap(0x%xu);' % nxt)
    out.append('}')
    out.append('')


def main():
    path, outdir = sys.argv[1], sys.argv[2]
    p = Program(path)
    p.explore()
    funcs = sorted(e for e in p.func_entries if e in p.code)
    missing = sorted(e for e in p.func_entries if e not in p.code)
    print('code insns: %d, functions: %d, bad seeds: %d, decode errors: %d' % (
        len(p.code), len(funcs), len(missing), len(p.bad)), file=sys.stderr)
    errs = collections.Counter(v.split(' ')[0] for v in p.bad.values())
    print('decode errors:', errs.most_common(20), file=sys.stderr)
    # report decode errors that are reached by real control flow
    os.makedirs(outdir, exist_ok=True)
    stubs_used = sorted(set(p.m.stubs.values()))
    with open(os.path.join(outdir, 'funcs.h'), 'w') as f:
        f.write('#pragma once\n#include "cpu.h"\n')
        for e in funcs:
            f.write('void %s(void);\n' % fname(e))
            if e in HOOKS:
                f.write('void fo_%x(void);\n' % e)
        for s in stubs_used:
            f.write('void H%s(void);\n' % s)
    nfiles = 16
    chunk = (len(funcs) + nfiles - 1) // nfiles
    for k in range(nfiles):
        out = ['#include "funcs.h"', '']
        for e in funcs[k * chunk:(k + 1) * chunk]:
            emit_function(p, e, out)
        with open(os.path.join(outdir, 'code_%02d.c' % k), 'w') as f:
            f.write('\n'.join(out))
    with open(os.path.join(outdir, 'dispatch.c'), 'w') as f:
        f.write('#include "funcs.h"\n\n')
        f.write('const struct guest_fn guest_fn_table[] = {\n')
        entries = [(e, fname(e)) for e in funcs]
        entries += [(a, 'H' + s) for a, s in p.m.stubs.items()]
        entries.sort()
        for a, n in entries:
            f.write('  {0x%xu, %s},\n' % (a, n))
        f.write('};\nconst unsigned guest_fn_count = %d;\n\n' % len(entries))
        f.write('const struct guest_bind guest_binds[] = {\n')
        stub_of = {s: a for a, s in p.m.stubs.items()}
        for a, s in sorted(p.m.binds.items()):
            f.write('  {0x%xu, "%s", 0x%xu},\n' % (a, s, stub_of.get(s, 0)))
        f.write('};\nconst unsigned guest_bind_count = %d;\n\n' % len(p.m.binds))
        f.write('const struct guest_seg guest_segs[] = {\n')
        for name, vm, vs, fo, fs in p.m.segments:
            f.write('  {"%s", 0x%xu, 0x%xu, 0x%xu, 0x%xu},\n' % (name, vm, vs, fo, fs))
        f.write('};\nconst unsigned guest_seg_count = %d;\n' % len(p.m.segments))
        f.write('const uint32_t guest_entry = 0x%xu;\n' % p.m.entry)
    with open(os.path.join(outdir, 'stubs.txt'), 'w') as f:
        f.write('\n'.join(stubs_used))


def debug():
    p = Program(sys.argv[2])
    p.explore()
    for a, e in sorted(p.bad.items()):
        print(hex(a), e)
    print(sorted(hex(x) for x in p.code & p.data_words))


if __name__ == "__main__" and sys.argv[1] == "--debug":
    debug(); sys.exit()
if __name__ == "__main__":
    main()
