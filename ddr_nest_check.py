import json, socket, sys
def cmd(c, **kw):
    kw.update(id=1, cmd=c)
    with socket.create_connection(("127.0.0.1", 4370), timeout=5) as s:
        s.sendall((json.dumps(kw)+"\n").encode()); buf=b""
        while not buf.endswith(b"\n"):
            ch=s.recv(1<<20)
            if not ch: break
            buf+=ch
    return json.loads(buf)
r = cmd("freeze_check")
for k in ("exception_entries","exception_reentry_blocks","nested_deliveries","nestgate_depth","nestgate_iec","nestgate_rfepend","nestgate_escreason","current_func"):
    print(k, r.get(k))
print("frame", cmd("frame").get("frame"))
