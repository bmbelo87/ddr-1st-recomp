#!/usr/bin/env python3
"""Descobre QUEM escreve num endereco da RAM do jogo, com PC/$ra.

Uso: py who_writes.py 8009706C        (com o jogo aberto no menu)
Arma o rastreador, espera voce mexer o cursor, e lista as escritas.
"""
import json, socket, sys

HOST, PORT = "127.0.0.1", 4370
def cmd(c, **kw):
    kw.update(id=1, cmd=c)
    with socket.create_connection((HOST, PORT), timeout=30) as s:
        s.sendall((json.dumps(kw) + "\n").encode()); b = b""
        while not b.endswith(b"\n"):
            ch = s.recv(1 << 20)
            if not ch: break
            b += ch
    return json.loads(b)

addr = int(sys.argv[1] if len(sys.argv) > 1 else "8009706C", 16) & 0x1FFFFF
lo, hi = addr & ~0xF, (addr & ~0xF) + 0x10
print("armando rastreador em %06X..%06X" % (lo, hi))
print(cmd("wtrace_range", lo="0x%08X" % lo, hi="0x%08X" % hi))
print(cmd("wtrace_arm"))
input(">> Agora mexa o cursor do menu pra cima e pra baixo umas 5x e tecle ENTER... ")
r = cmd("wtrace_dump", addr_lo="0x%08X" % lo, addr_hi="0x%08X" % hi, count=40, newest=1)
ents = r.get("entries") or r.get("writes") or []
print("escritas capturadas: %d" % len(ents))
for e in ents[:30]:
    print("  seq=%s addr=%s %s->%s w=%s pc=%s ra=%s func=%s" % (
        e.get("seq"), e.get("addr"), e.get("old_val"), e.get("new_val"),
        e.get("width"), e.get("pc"), e.get("ra"), e.get("func_addr") or e.get("func")))
if not ents:
    print("(nada — confira se o cursor realmente mudou e se o endereco esta certo)")
    print("resposta crua:", json.dumps(r)[:400])
