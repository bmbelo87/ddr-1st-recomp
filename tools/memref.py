#!/usr/bin/env python3
"""Quem acessa um endereco absoluto, seguindo lui (+addiu) e deslocamento.
Uso: memref.py 0x800921D4 [janela]"""
import struct,sys,os
IMG=os.path.join(os.path.dirname(__file__),'..','SLPM_862.22')
d=open(IMG,'rb').read()[0x800:]
LOAD=0x80016000
tgt=int(sys.argv[1],16)
WIN=int(sys.argv[2]) if len(sys.argv)>2 else 16
MEM={0x23:'lw',0x2B:'sw',0x24:'lbu',0x25:'lhu',0x20:'lb',0x21:'lh',0x28:'sb',0x29:'sh'}
def w(i): return struct.unpack('<I',d[i:i+4])[0]
def s16(v): return v-0x10000 if v&0x8000 else v
hits=[]
for i in range(0,len(d)-3,4):
    v=w(i)
    if v>>26!=0x0F: continue                       # lui
    rt=(v>>16)&31
    base=(v&0xFFFF)<<16
    reg={rt:base}
    for k in range(1,WIN+1):
        j=i+4*k
        if j+4>len(d): break
        n=w(j); op=n>>26; ns=(n>>21)&31; nt=(n>>16)&31
        if op==9 and ns in reg:                    # addiu rt,rs,imm
            reg[nt]=(reg[ns]+s16(n&0xFFFF))&0xFFFFFFFF
            continue
        if op==0x0F: reg.pop(nt,None); reg[nt]=(n&0xFFFF)<<16; continue
        if op in MEM and ns in reg:
            if (reg[ns]+s16(n&0xFFFF))&0xFFFFFFFF==tgt:
                hits.append((LOAD+j,MEM[op],LOAD+i))
for a,m,b in hits: print("0x%08X  %-3s  (base em 0x%08X)"%(a,m,b))
if not hits: print("(nenhum acesso direto encontrado)")
