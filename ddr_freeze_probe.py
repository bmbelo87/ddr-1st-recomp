#!/usr/bin/env python3
"""Run while the DDR recomp is frozen in the OT merge loop (func_8007259C).
Connects to the psxrecomp debug server (127.0.0.1:4370), reads registers and
walks the primitive chain from $v0, printing each node until a cycle."""
import json, socket, sys

HOST, PORT = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 4370
_id = 0
def cmd(c, **kw):
    """One connection per request: the server drops the socket after replying."""
    global _id; _id += 1
    kw.update(id=_id, cmd=c)
    with socket.create_connection((HOST, PORT), timeout=5) as s:
        s.sendall((json.dumps(kw) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 20)
            if not chunk: break
            buf += chunk
    return json.loads(buf)

def read(addr, n):
    r = cmd("read_ram", addr="0x%08X" % addr, len=n)
    return bytes.fromhex(r["hex"])
def u32(addr):
    return int.from_bytes(read(addr, 4), "little")


# Sample registers many times: r2 = current node, r14 = its ORIGINAL next,
# r11 = OT slot, r4 = head slot. The loop rewrites node.next after reading it,
# so memory alone cannot show the chain the CPU actually follows.
samples = []
for i in range(400):
    r = cmd("get_registers"); g = [int(x, 16) for x in r["gpr"]]
    samples.append((r.get("pc") or r.get("cop0_epc"), g[2], g[14], g[11], g[4], g[8]))
pcs = sorted(set(s[0] for s in samples)); print("pcs seen:", pcs)
nodes = []
for s in samples:
    if not nodes or nodes[-1][0] != s[1]: nodes.append((s[1], s[2], s[3], s[4]))
print("distinct consecutive (node, orig_next_word, ot_slot, head_slot):", len(nodes))
for n in nodes[:80]:
    print("node %08X next_word %08X (next %06X bucket %X len %d) otslot %08X head_slot %08X" %
          (n[0], n[1], n[1] & 0xFFFFFF, n[1] >> 28, (n[1] >> 24) & 0xF, n[2], n[3]))
uniq = sorted(set(n[0] for n in nodes)); print("unique nodes:", ["%08X" % u for u in uniq])
if uniq:
    lo, hi = min(uniq) & 0x1FFFFF, (max(uniq) & 0x1FFFFF) + 32
    raw = read(0x80000000 | lo, hi - lo)
    print("-- raw", "%08X..%08X" % (0x80000000 | lo, 0x80000000 | hi))
    for i in range(0, len(raw), 32):
        print("%08X %s" % (0x80000000 | (lo + i), raw[i:i+32].hex()))
print("frame:", cmd("frame"))
