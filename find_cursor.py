#!/usr/bin/env python3
"""Acha a variavel do item selecionado no menu.

Uso: com o jogo aberto NO MENU (PLEASE SELECT MODE), rode e siga as instrucoes.
Ele tira uma foto da RAM com o cursor em cada item e cruza os resultados,
sobrando os enderecos que valem 0,1,2,... exatamente como o cursor.
"""
import json, socket, sys

HOST, PORT = "127.0.0.1", 4370
LO, HI = 0x80010000, 0x80200000        # RAM do jogo (acima do kernel)

def cmd(c, **kw):
    kw.update(id=1, cmd=c)
    with socket.create_connection((HOST, PORT), timeout=60) as s:
        s.sendall((json.dumps(kw) + "\n").encode())
        b = b""
        while not b.endswith(b"\n"):
            ch = s.recv(1 << 20)
            if not ch: break
            b += ch
    return json.loads(b)

def snap():
    r = cmd("read_ram", addr="0x%08X" % LO, len=HI - LO)
    return bytes.fromhex(r["hex"])

shots = []
n = int(sys.argv[1]) if len(sys.argv) > 1 else 4
for i in range(n):
    input("\n>> Deixe o cursor no item %d (o primeiro e 0) e tecle ENTER... " % i)
    shots.append(snap())
    print("   foto %d capturada (%d bytes)" % (i, len(shots[-1])))

# byte que vale exatamente o indice do cursor em todas as fotos
cands = [o for o in range(len(shots[0])) if all(s[o] == i for i, s in enumerate(shots))]
print("\ncandidatos byte  (valor == indice): %d" % len(cands))
for o in cands[:40]:
    print("   %08X" % (LO + o))

# variantes: contagem invertida, ou base 1
inv = [o for o in range(len(shots[0]))
       if all(s[o] == (n - 1 - i) for i, s in enumerate(shots))]
b1 = [o for o in range(len(shots[0])) if all(s[o] == i + 1 for i, s in enumerate(shots))]
print("\ncandidatos (contagem invertida): %s" % ["%08X" % (LO + o) for o in inv[:15]])
print("candidatos (base 1):            %s" % ["%08X" % (LO + o) for o in b1[:15]])
