#!/usr/bin/env python3
"""Desmontador MIPS minimo da imagem do DDR. Uso: dis.py 0x8004A380 [n]"""
import struct, sys, os
IMG = os.path.join(os.path.dirname(__file__), '..', 'SLPM_862.22')
d = open(IMG, 'rb').read()[0x800:]
LOAD = 0x80016000
def w(a): return struct.unpack('<I', d[a-LOAD:a-LOAD+4])[0]
R=['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
   's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']
def dis(a):
    v=w(a);op=v>>26;rs=(v>>21)&31;rt=(v>>16)&31;rd=(v>>11)&31;sa=(v>>6)&31;f=v&63
    imm=v&0xFFFF;s=imm-0x10000 if imm&0x8000 else imm
    m={0x23:'lw',0x2B:'sw',0x24:'lbu',0x25:'lhu',0x20:'lb',0x21:'lh',0x28:'sb',0x29:'sh'}
    if op==0:
        if v==0: return 'nop'
        if f in (0,2,3): return f"{ {0:'sll',2:'srl',3:'sra'}[f]} {R[rd]},{R[rt]},{sa}"
        if f==8: return f"jr {R[rs]}"
        sp={0x21:'addu',0x23:'subu',0x25:'or',0x24:'and',9:'jalr',0x2A:'slt',0x2B:'sltu',
            0x12:'mflo',0x10:'mfhi',0x18:'mult',0x19:'multu',0x1A:'div',0x1B:'divu',
            0x26:'xor',0x27:'nor',4:'sllv',6:'srlv',7:'srav'}
        return f"{sp.get(f,hex(f))} {R[rd]},{R[rs]},{R[rt]}"
    if op==9:    return f"addiu {R[rt]},{R[rs]},{s}"
    if op==0x0C: return f"andi {R[rt]},{R[rs]},0x{imm:X}"
    if op==0x0D: return f"ori {R[rt]},{R[rs]},0x{imm:X}"
    if op==0x0F: return f"lui {R[rt]},0x{imm:04X}"
    if op==0x0A: return f"slti {R[rt]},{R[rs]},{s}"
    if op==0x0B: return f"sltiu {R[rt]},{R[rs]},{s}"
    if op==4:    return f"beq {R[rs]},{R[rt]},0x{a+4+s*4:08X}"
    if op==5:    return f"bne {R[rs]},{R[rt]},0x{a+4+s*4:08X}"
    if op==6:    return f"blez {R[rs]},0x{a+4+s*4:08X}"
    if op==7:    return f"bgtz {R[rs]},0x{a+4+s*4:08X}"
    if op==1:    return f"{'bltz' if rt==0 else 'bgez'} {R[rs]},0x{a+4+s*4:08X}"
    if op==3:    return f"jal 0x{((a&0xF0000000)|((v&0x3FFFFFF)<<2)):08X}"
    if op==2:    return f"j 0x{((a&0xF0000000)|((v&0x3FFFFFF)<<2)):08X}"
    if op in m:  return f"{m[op]} {R[rt]},{s}({R[rs]})"
    return f"op{op:02X} {v:08X}"
a0=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 24
for a in range(a0,a0+n*4,4): print(hex(a),dis(a))
