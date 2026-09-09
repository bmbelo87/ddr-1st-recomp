#!/usr/bin/env python3
"""Le a tabela de itens do mode select e o cursor, com o jogo NO MENU."""
import json, socket
HOST, PORT = "127.0.0.1", 4370
def cmd(c, **kw):
    kw.update(id=1, cmd=c)
    with socket.create_connection((HOST, PORT), timeout=30) as s:
        s.sendall((json.dumps(kw)+"\n").encode()); b=b""
        while not b.endswith(b"\n"):
            ch=s.recv(1<<20)
            if not ch: break
            b+=ch
    return json.loads(b)
def rd(a,n): return bytes.fromhex(cmd("read_ram", addr="0x%08X"%a, len=n)["hex"])

TAB = 0x8007ECE8
raw = rd(TAB, 12*9)
print("tabela viva @%08X (12 bytes/entrada)\n" % TAB)
for i in range(9):
    e = raw[i*12:(i+1)*12]
    ptr = int.from_bytes(e[8:12], "little")
    idv = int.from_bytes(e[6:8], "little")
    print("  item %d  up=%-3d down=%-3d  id=%-3d ptr=%08X | %s"
          % (i, e[0], e[1], idv, ptr, e.hex(' ')))

st = rd(0x80097068, 16)
print("\nestado do menu @80097068: %s" % st.hex(' '))
print("cursor (+4) = %d" % int.from_bytes(st[4:8], "little"))
