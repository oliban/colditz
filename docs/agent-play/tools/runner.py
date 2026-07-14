#!/usr/bin/env python3
"""Compiled-route runner: one process, tight polls, zero LLM. Route lines:
switch N | run | <room> exit I | <room> item NAME [FROM] | <room> tile X Y"""
import json, sys, time, urllib.request, urllib.error

API = "http://127.0.0.1:8765"
LOG = __file__.rsplit('/',1)[0] + "/live-log.jsonl"

def call(path, body=None):
    for _ in range(3):
        try:
            try:
                r = urllib.request.urlopen(urllib.request.Request(
                    API+path, data=json.dumps(body).encode() if body else None), timeout=2)
            except urllib.error.HTTPError as e:
                return {"_http": e.code, "_body": e.read().decode(errors="replace")}
            d = r.read()
            # event-bus logging (dashboard)
            if body:
                with open(LOG,'a') as f:
                    f.write(json.dumps({"ts":time.time(),"type":"cmd",
                        "endpoint":path.strip('/'),"body":body})+"\n")
            return json.loads(d) if d[:1] in (b'{',b'[') else d
        except Exception:
            time.sleep(0.05)
    return None

def st(): return call("/state")
def me(s): return s["prisoners"][s["current_prisoner"]]

def drain(t=4.0):
    end = time.time()+t
    while time.time() < end:
        s = st()
        if s and s["input_queue"] == 0: return s
        time.sleep(0.03)
    return st()

def waitwalk(run_kick=False, t=30.0):
    end = time.time()+t; kicked = not run_kick
    while time.time() < end:
        s = st()
        if s:
            if not kicked and s["walk"] == "walking":
                call("/input", {"key":"walk_run","ms":150})  # engage RUN mid-walk
                kicked = True
            if s["walk"] != "walking" and s["input_queue"] == 0 and kicked:
                return s
        time.sleep(0.03)
    return st()

def split(s0, s, tag, ok):
    dt = (s["game_time"]-s0)/1000
    print(f"leg [{tag}] -> {s['walk']} | room {me(s)['room']} | split {dt:.1f}s {'OK' if ok else 'FAIL'}")
    with open(LOG,'a') as f:
        f.write(json.dumps({"ts":time.time(),"type":"split",
            "text":f"[{tag}] {s['walk']} room {me(s)['room']} @{dt:.1f}s"})+"\n")

def main(routefile):
    s = st(); t0 = s["game_time"]; want_run = False; fail = 0
    for line in open(routefile):
        w = line.split('#')[0].split()
        if not w: continue
        if w[0] == "switch":
            drain(); call("/input", {"key":f"prisoner_{w[1]}","ms":250})
            for _ in range(20):
                s = st()
                if s and s["current_prisoner"] == int(w[1])-1: break
                time.sleep(0.05)
            continue
        if w[0] == "run": want_run = True; continue
        room, verb = int(w[0]), w[1]
        s = drain()
        if me(s)["room"] != room:
            print(f"ABORT: expected room {room}, in {me(s)['room']}"); fail = 1; break
        def fire():
            if verb == "exit": return call("/walk", {"exit": int(w[2])})
            if verb == "item":
                b = {"item": w[2], "pickup": True}
                if len(w) > 3: b["from"] = w[3]
                return call("/walk", b)
            return call("/walk", {"tile":[int(w[2]),int(w[3])]})
        resp = fire()
        if isinstance(resp, dict) and resp.get("_http"):
            print(f"  leg rejected ({resp['_http']}: {resp.get('_body','')[:40]}), retrying once")
            time.sleep(0.4); drain(); resp = fire()
        s = waitwalk(run_kick=want_run); want_run = False
        ok = (verb != "exit" or me(s)["room"] != room) and s["walk"] in ("arrived","idle")
        split(t0, s, ' '.join(w), ok)
        if not ok: fail = 1; break
    sys.exit(fail)

main(sys.argv[1])
