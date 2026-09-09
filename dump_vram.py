#!/usr/bin/env python3
"""Extrai a VRAM do jogo em BMP. Rode com o jogo NA TELA que voce quer capturar.

  py dump_vram.py                 -> VRAM inteira (1024x512), duas leituras
  py dump_vram.py 512 240 256 128 -> so um recorte (x y w h)

Gera:
  vram_16bit.bmp  - interpretacao 16 bits (texturas 15-bit e o framebuffer)
  vram_4bit.bmp   - cada word vira 4 pixels de 4 bits (texturas indexadas,
                    que e como quase todo texto de PS1 e guardado)
"""
import json, socket, struct, sys

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

def peek(x, y, w, h):
    r = cmd("vram_peek", x=x, y=y, w=w, h=h)
    hx = r["hex"] if "hex" in r else r.get("data", "")
    return [int(hx[i:i+4], 16) for i in range(0, len(hx), 4)]

def grab(X, Y, W, H):
    px = [[0]*W for _ in range(H)]
    for ty in range(0, H, 128):
        for tx in range(0, W, 128):
            w, h = min(128, W-tx), min(128, H-ty)
            vals = peek(X+tx, Y+ty, w, h)
            for r in range(h):
                for c in range(w):
                    px[ty+r][tx+c] = vals[r*w + c]
        print("  linha %d/%d" % (ty+128, H))
    return px

def bmp(path, rows):
    h, w = len(rows), len(rows[0])
    pad = (-w*3) % 4
    data = bytearray()
    for y in range(h-1, -1, -1):            # BMP e de baixo para cima
        for x in range(w):
            r, g, b = rows[y][x]
            data += bytes((b, g, r))
        data += b"\0" * pad
    hdr = struct.pack("<2sIHHI", b"BM", 14+40+len(data), 0, 0, 14+40)
    hdr += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    open(path, "wb").write(hdr + bytes(data))
    print("  gravado %s (%dx%d)" % (path, w, h))

a = [int(v) for v in sys.argv[1:5]] if len(sys.argv) >= 5 else [0, 0, 1024, 512]
X, Y, W, H = a
print("lendo VRAM %dx%d em (%d,%d)..." % (W, H, X, Y))
px = grab(X, Y, W, H)

# 16 bits: RGB555 -> RGB888
bmp("vram_16bit.bmp",
    [[(((p & 31) << 3), (((p >> 5) & 31) << 3), (((p >> 10) & 31) << 3))
      for p in row] for row in px])

# 4 bits: cada word = 4 indices; rampa de cinza para enxergar a forma
bmp("vram_4bit.bmp",
    [[(v*17, v*17, v*17)
      for p in row for v in ((p & 15), (p >> 4) & 15, (p >> 8) & 15, (p >> 12) & 15)]
     for row in px])
