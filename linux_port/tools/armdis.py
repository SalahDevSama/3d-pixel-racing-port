"""Annotated disassembly of a range: python dis.py START END"""
import sys, struct
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from macho import MachO
import capstone
from capstone import arm

m = MachO(sys.argv[3] if len(sys.argv) > 3 else 'gen/armv6.macho')
cs = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
cs.detail = True
cs.skipdata = True

def describe(v):
    if v is None:
        return ''
    if v in m.stubs:
        return m.stubs[v]
    if v in m.binds:
        return '&' + m.binds[v]
    sec = m.section_of(v)
    if sec in ('__cstring', '__objc_methname', '__objc_classname'):
        return repr(m.cstr(v))
    if sec == '__objc_selrefs':
        return '@sel(' + str(m.cstr(m.u32(v))) + ')'
    if sec == '__cfstring':
        return '@"' + str(m.cstr(m.u32(v + 8))) + '"'
    if sec == '__objc_classrefs':
        t = m.u32(v)
        return '@class(' + (m.binds.get(v) or hex(t or 0)) + ')'
    return sec or ''

def main():
    start = int(sys.argv[1], 16); end = int(sys.argv[2], 16)
    code = m.read(start, end - start)
    lits = {}  # reg -> value pending pc-relative add
    for ins in cs.disasm(code, start):
        note = ''
        if ins.id == arm.ARM_INS_LDR and len(ins.operands) == 2:
            mem = ins.operands[1].mem
            if mem.base == arm.ARM_REG_PC and mem.index == 0:
                lit = m.u32(ins.address + 8 + mem.disp)
                note = 'lit=%#x' % lit
                lits[ins.operands[0].reg] = lit
            elif mem.base == arm.ARM_REG_PC and mem.index in lits:
                a = (lits[mem.index] + ins.address + 8) & 0xffffffff
                note = '[%#x] -> %s' % (a, describe(a))
        if ins.id == arm.ARM_INS_ADD and len(ins.operands) == 3 and ins.operands[1].reg == arm.ARM_REG_PC \
                and ins.operands[2].type == arm.ARM_OP_REG and ins.operands[2].reg in lits:
            a = (lits[ins.operands[2].reg] + ins.address + 8) & 0xffffffff
            note = '= %#x %s' % (a, describe(a))
        if ins.id in (arm.ARM_INS_BL, arm.ARM_INS_B, arm.ARM_INS_BLX) and ins.operands[0].type == arm.ARM_OP_IMM:
            note = describe(ins.operands[0].imm)
        print('%6x: %-8s %-36s %s' % (ins.address, ins.mnemonic, ins.op_str, note))

main()
