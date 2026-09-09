import json, socket
HOST, PORT = "127.0.0.1", 4370
_id = 0
def cmd(c, **kw):
    global _id; _id += 1
    kw.update(id=_id, cmd=c)
    with socket.create_connection((HOST, PORT), timeout=8) as s:
        s.sendall((json.dumps(kw)+"\n").encode()); b = b""
        while not b.endswith(b"\n"):
            ch = s.recv(1 << 20)
            if not ch: break
            b += ch
    return json.loads(b)
def rd(addr, n):
    return bytes.fromhex(cmd("read_ram", addr="0x%08X" % addr, len=n)["hex"])
def u32(a):
    return int.from_bytes(rd(a, 4), "little")
def hexdump(addr, n, label):
    print("-- %s  %08X..%08X" % (label, addr, addr+n))
    raw = rd(addr, n)
    for i in range(0, n, 16):
        w = ["%08X" % int.from_bytes(raw[i+j:i+j+4], "little") for j in range(0, 16, 4)]
        print("%08X  %s" % (addr+i, " ".join(w)))

r = cmd("get_registers"); g = [int(x, 16) for x in r["gpr"]]
pc = r.get("pc") or r.get("cop0_epc")
print("pc", pc, "frame", cmd("frame").get("frame"))
names = "zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra".split()
print(" ".join("%s=%08X" % (names[i], g[i]) for i in (2,3,4,5,6,8,11,12,14,16,17,31)))

node = g[2]; otslot = g[11] or g[5]; otbase = g[5] - 32
struct_a0 = 0x80084598
print("\nstruct @%08X: " % struct_a0, ["%08X" % u32(struct_a0+o) for o in range(0, 0x20, 4)])
start = u32(struct_a0+4); end = u32(struct_a0+16)
print("pending array %08X..%08X (%d slots), iterador r4=%08X" % (start, end, (end-start)//4, g[4]))
raw = rd(start, min(end-start, 0x400))
slots = [int.from_bytes(raw[i:i+4], "little") for i in range(0, len(raw), 4)]
nz = [(start+4*i, v) for i, v in enumerate(slots) if v]
print("slots nao-zero (%d):" % len(nz), ["%08X:%08X" % t for t in nz[:40]])

hexdump(otbase-16, 0xA0, "OT (base %08X)" % otbase)
hexdump((node & ~0xF) - 0x40, 0xC0, "node %08X" % node)

print("\n-- walk (next=word&0xFFFFFF, para em 0) --")
seen = {}; cur = node; 
for i in range(40):
    if cur in seen:
        print("CICLO: %08X ja visto no passo %d" % (cur, seen[cur])); break
    seen[cur] = i
    w = u32(cur)
    print("%2d %08X word=%08X len=%d next=%06X" % (i, cur, w, w >> 24, w & 0xFFFFFF))
    nx = w & 0xFFFFFF
    if nx == 0 or nx == 0xFFFFFF: print("fim"); break
    cur = 0x80000000 | nx
