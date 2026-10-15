#!/usr/bin/env python3
"""Build gate: loads the REAL .so the way the chain host does and checks the
layer the host actually consumes, then proves controls and presets APPLY.

  check_contract.py <built.so> [src/module.json]

The rule this file enforces: prove a parameter changes the audio, do not just
prove that setting it did not crash.
"""
import array
import hashlib
import json
import math
import pathlib
import random
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from swhost import Fx  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
FAILS = 0
CHECKS = 0


def ok(cond, msg):
    global FAILS, CHECKS
    CHECKS += 1
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        FAILS += 1


# ---- signals --------------------------------------------------------------
SR = 44100


def chord(secs, stab=0.5, level=0.25, seed=1):
    """Three-tone chord stab, then silence."""
    n = SR * secs
    out = array.array("h", [0]) * (n * 2)
    freqs = (196.0, 294.0, 392.0)
    for i in range(min(n, int(stab * SR))):
        t = i / SR
        env = min(1.0, i / 400.0) * min(1.0, (stab * SR - i) / 2000.0)
        s = sum(math.sin(2 * math.pi * f * t) for f in freqs) / 3.0
        v = int(max(-1.0, min(1.0, s * level * 2 * env)) * 32767)
        out[2 * i] = out[2 * i + 1] = v
    return out


def steady(secs, level=0.5):
    n = SR * secs
    out = array.array("h", [0]) * (n * 2)
    for i in range(n):
        t = i / SR
        s = (math.sin(2 * math.pi * 196 * t) + math.sin(2 * math.pi * 294 * t)) * 0.5 * level
        out[2 * i] = out[2 * i + 1] = int(s * 32767)
    return out


def rms(a, t0, t1):
    seg = a[int(t0 * SR) * 2:int(t1 * SR) * 2:2]
    return math.sqrt(sum(x * x for x in seg) / max(1, len(seg))) / 32768.0


def peak(a):
    return max(abs(x) for x in a) / 32768.0


def run(fx, sig):
    buf = array.array("h", sig)
    fx.process(buf)
    return buf


def digest(buf):
    return hashlib.md5(buf.tobytes()).hexdigest()


def fresh(base, **kv):
    fx = base.new_instance()
    for k, v in kv.items():
        fx.set(k, v)
    return fx


def main():
    so = sys.argv[1]
    modjson = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "src/module.json")

    print("== dlopen ==")
    base = Fx(so)
    api = base.api
    ok(api.api_version == 2, "api_version == 2")
    ok(bool(api.create_instance and api.destroy_instance and api.process_block
            and api.set_param and api.get_param), "all required entry points present")
    ok(any("loaded" in m for m in base.logs), "init logs once, outside the audio path")

    print("== chain_params ==")
    cp = json.loads(base.get("chain_params"))
    keys = [e["key"] for e in cp]
    ok(len(cp) == 32 and len(set(keys)) == 32, "32 unique controls declared")
    bad = [e["key"] for e in cp if e["type"] == "int" and not (e["min"] <= e["default"] <= e["max"])]
    ok(not bad, "every int default lies inside its range %s" % bad)
    bad = [e["key"] for e in cp if e["type"] == "enum" and not (0 <= e["default"] < len(e["options"]))]
    ok(not bad, "every enum default is a valid option index %s" % bad)

    print("== module.json ==")
    mj = json.loads(modjson.read_text())
    mkeys = [e["key"] for e in mj["capabilities"]["chain_params"]]
    ok(mkeys == keys, "module.json declares exactly the keys the plugin serves, in order")
    ok(modjson.stat().st_size <= 8192, "module.json is %d bytes (cap 8192)" % modjson.stat().st_size)
    ok(mj["id"] == "shimmerwash" and mj["dsp"] == "shimmerwash.so", "id and dsp name match the file the host loads")

    print("== ui_hierarchy ==")
    uh = json.loads(base.get("ui_hierarchy"))
    levels = uh["levels"]
    ok("root" in levels, "has a root level")
    placed = {}
    for lname, lv in levels.items():
        for k in lv.get("knobs", []):
            placed.setdefault(k, []).append(lname)
    missing = [k for k in keys if k not in placed]
    dupes = [k for k, v in placed.items() if len(v) > 1]
    ok(not missing and not dupes, "every control sits on exactly one page (missing %s, duplicated %s)" % (missing, dupes))
    ok(all(len(lv.get("knobs", [])) <= 8 for lv in levels.values()), "no page has more than 8 knobs")
    links = [p["level"] for p in levels["root"]["params"] if "level" in p]
    ok(all(l in levels for l in links) and set(levels) - {"root"} == set(links),
       "root links to every other page and nothing dangles %s" % links)
    pr = levels["presets"]
    ok(pr.get("list_param") == "preset" and pr.get("count_param") == "preset_count"
       and pr.get("name_param") == "preset_name" and "knobs" not in pr, "preset browser declared, and never as a knob")
    ok(all(any(p.get("key") == k for p in lv["params"]) for lv in levels.values() if "knobs" in lv for k in lv["knobs"]),
       "each page's params carry a label for every knob")

    print("== help.json ==")
    hp = json.loads((ROOT / "src/help.json").read_text())
    titles = {c["title"].lower() for c in hp["children"]}
    need = {"main"} | {lv["name"].lower() for n, lv in levels.items() if n != "root"}
    ok(need <= titles, "help covers every page %s" % sorted(need - titles))

    print("== reads, writes, enums ==")
    fx = base.new_instance()
    for e in cp:
        k = e["key"]
        if e["type"] == "enum":
            want = e["options"][e["default"]]
            ok(fx.get(k) == want, "%-10s default reads back as name %r" % (k, want))
            for i, name in enumerate(e["options"]):
                fx.set(k, name)
                a = fx.get(k)
                fx.set(k, i)
                b = fx.get(k)
                if not (a == name and b == name):
                    ok(False, "%s: set by name/number gave %r/%r, wanted %r" % (k, a, b, name))
                    break
            else:
                ok(True, "%-10s sets by name and by number" % k)
            fx.set(k, e["options"][e["default"]])
        else:
            fx.set(k, e["max"] + 999)
            hi = fx.get(k)
            fx.set(k, e["min"] - 999)
            lo = fx.get(k)
            fx.set(k, e["default"])
            ok(hi == str(e["max"]) and lo == str(e["min"]) and fx.get(k) == str(e["default"]),
               "%-10s clamps to [%d, %d]" % (k, e["min"], e["max"]))
    fx.set("fx1:a_pitch", 5)
    ok(fx.get("fx1:a_pitch") == "5" and fx.get("a_pitch") == "5", "'<component>:<key>' addressing works")
    ok(fx.get("no_such_key") is None, "unknown key returns -1")

    print("== state ==")
    a = fresh(base, a_pitch=-7, b_mode="POST", duck_thr=-40, a_fine=-33, freeze="On", filt_type="LP")
    blob = a.get("state")
    sj = json.loads(blob)
    ok(sj.get("sw") == 1 and all(k in sj for k in keys) and "preset" in sj, "state is one JSON object carrying every control + preset")
    ok(len(blob) < 1536, "state is %d bytes (kept well clear of the edge)" % len(blob))
    b = base.new_instance()
    b.set("state", blob)
    ok(all(a.get(k) == b.get(k) for k in keys), "a restored instance reads back identically")
    sig = chord(2)
    ok(digest(run(a, sig)) == digest(run(b, sig)), "a restored instance renders identically")
    c = base.new_instance()
    c.set("state", '{"hello":1,"a_pitch":19}')
    ok(c.get("a_pitch") == "12", "a foreign blob is ignored")

    print("== presets ==")
    n = int(fx.get("preset_count"))
    ok(n == 14, "preset_count is %d" % n)
    names = []
    flat = None
    ref = chord(3)
    for i in range(n):
        p = base.new_instance()
        p.set("freeze", "On")
        p.set("preset", i)
        names.append(p.get("preset_name"))
        h = digest(run(p, ref))
        if i == 0:
            flat = h
            ok(names[0] == "INIT", "preset 0 is INIT")
            ok(all(p.get(e["key"]) == (e["options"][e["default"]] if e["type"] == "enum" else str(e["default"]))
                   for e in cp if e["key"] != "freeze"), "INIT puts every control at its default")
        else:
            q = base.new_instance()
            q.set("preset", i)
            ok(digest(run(q, ref)) != flat and names[-1], "preset %2d %-15s changes the audio" % (i, names[-1]))
        ok(p.get("freeze") == "On" and p.get("preset") == str(i), "preset %2d leaves Freeze alone" % i) if i in (0, n - 1) else None
    ok(len(set(names)) == n and all(len(x) <= 16 for x in names), "preset names are unique and fit the list (<=16 chars)")

    print("== every control APPLIES ==")
    # prerequisites: a control whose stage is inactive is given its prerequisite first
    pre = {"duck_amt": 60}
    probes = {
        "mix": 90, "decay": 20, "size": 20, "color": 90, "modrate": 100, "modamt": 5, "predelay": 160, "bloom": 100,
        "a_pitch": -12, "a_fine": 60, "a_delay": 400, "a_mode": "POST", "a_amt": 90, "a_pan": -100,
        "b_pitch": 19, "b_fine": 60, "b_delay": 400, "b_mode": "PRE", "b_amt": 90, "b_pan": 100,
        "sh_tone": 0, "sh_low": 100, "spread": 0, "freeze": "On",
        "in_gain": -9, "filt_type": "LP", "filt_freq": 60, "filt_q": 90,
        "duck_amt": 80, "duck_thr": -50, "duck_att": 100, "duck_rel": 600,
    }
    loud = chord(3, stab=1.0, level=0.45)
    base_hash = {}
    for k, v in probes.items():
        extra = dict(pre)
        if k in ("filt_q", "filt_type"):
            extra["filt_freq"] = 60
        if k.startswith("duck_") and k != "duck_amt":
            extra["duck_amt"] = 60
        ref_fx = fresh(base, **extra)
        h0 = digest(run(ref_fx, loud))
        t = dict(extra)
        t[k] = v
        h1 = digest(run(fresh(base, **t), loud))
        ok(h0 != h1, "%-10s %-5s -> audio changed" % (k, v))

    print("== stability and tails ==")
    burst = chord(22, stab=0.3, level=0.3)
    worst = 0.0
    for i in range(n):
        p = base.new_instance()
        p.set("preset", i)
        buf = run(p, burst)
        early, late = rms(buf, 4, 8), rms(buf, 17, 22)
        # a tail may not GROW; near-infinite presets only need to stay flat
        good = late <= early * 1.05 + 1e-6 and peak(buf) < 1.0
        ok(good, "preset %2d %-15s tail decays (%.4f -> %.4f), peak %.2f" % (i, names[i], early, late, peak(buf)))
    # steady, loud, long: every preset must stay bounded
    for i in (3, 8, 13):
        p = base.new_instance()
        p.set("preset", i)
        buf = run(p, steady(25, level=0.7))
        ok(rms(buf, 15, 25) < 0.6, "preset %2d %-15s steady loud tone stays bounded (rms %.3f)" % (i, names[i], rms(buf, 15, 25)))
    # both shifters maxed in feedback, longest decay, 1 s delays, random live edits
    p = fresh(base, decay=100, a_amt=100, b_amt=100, a_delay=1000, b_delay=650, color=100)
    buf = run(p, chord(30, stab=0.3, level=0.45))
    ok(rms(buf, 25, 30) < rms(buf, 3, 8) * 1.05 + 1e-6, "worst case (both shifters at 100, decay 100, 1 s delays) decays")
    rnd = random.Random(7)
    p = base.new_instance()
    ok_all = True
    sig = steady(1, 0.4)
    for _ in range(40):
        for k, e in [(e["key"], e) for e in cp]:
            if rnd.random() < 0.15:
                if e["type"] == "enum":
                    p.set(k, rnd.randrange(len(e["options"])))
                else:
                    p.set(k, rnd.randint(e["min"], e["max"]))
        b2 = run(p, sig)
        ok_all &= peak(b2) <= 1.0
    ok(ok_all, "40 s of random live edits to every control: no blow-ups")

    print("== freeze ==")
    p = fresh(base, decay=60, freeze="Off")
    buf = array.array("h", chord(1, stab=0.5, level=0.4) + array.array("h", [0]) * (SR * 2 * 20))
    p.process(buf[:SR * 2])                       # input stab (Freeze off)
    p.set("freeze", "On")
    tail = array.array("h", buf[SR * 2:])
    p.process(tail)
    r1, r2 = rms(tail, 2, 4), rms(tail, 16, 20)
    ok(r2 > r1 * 0.5, "frozen wash holds (%.4f at 2-4 s, %.4f at 16-20 s)" % (r1, r2))

    print("== dry path ==")
    p = fresh(base, mix=0)
    sig = steady(4, 0.4)
    out = run(p, sig)
    d = max(abs(a - b) for a, b in zip(out[SR * 2 * 2:], sig[SR * 2 * 2:]))
    ok(d <= 1, "Mix 0 passes the dry signal through (max error %d LSB)" % d)

    print("== cost ==")
    p = fresh(base, a_amt=70, b_amt=70, a_delay=800, b_delay=500, duck_amt=50)
    sig = steady(10, 0.4)
    t0 = time.perf_counter()
    run(p, sig)
    dt = time.perf_counter() - t0
    # Includes Python call overhead (~2% of a block); the number is conservative.
    ok(dt / 10.0 < 0.25, "10 s of audio in %.3f s (%.1f%% of real time on this CPU, all stages active)" % (dt, 100 * dt / 10.0))

    print("\n%d checks, %d failed" % (CHECKS, FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
