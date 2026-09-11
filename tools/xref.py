#!/usr/bin/env python3
"""Quem chama um endereco. Uso: xref.py 0x8007259C"""
import struct,sys,os
IMG=os.path.join(os.path.dirname(__file__),'..','SLPM_862.22')
d=open(IMG,'rb').read()[0x800:]
LOAD=0x80016000
tgt=int(sys.argv[1],16)
for i in range(0,len(d)-3,4):
    v=struct.unpack('<I',d[i:i+4])[0]
    a=LOAD+i
    op=v>>26
    if op in (2,3):
        t=(a&0xF0000000)|((v&0x3FFFFFF)<<2)
        if t==tgt: print(f"0x{a:08X} {'jal' if op==3 else 'j'}")
