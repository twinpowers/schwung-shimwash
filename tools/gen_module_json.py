#!/usr/bin/env python3
"""Emit src/module.json from the plugin's own chain_params.

module.json is what the HOST reads; the plugin also serves chain_params from
get_param. A module whose two copies disagree fails in the worst way: the
plugin behaves and the host does not offer the control. So the file is
generated FROM the built plugin and never hand-edited.

  gen_module_json.py <built.so>            write src/module.json
  gen_module_json.py <built.so> --check    exit non-zero if it is stale
"""
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from swhost import Fx  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "src/module.json"
CAP = 8192          # the host reads at most 8 KB of module.json
VERSION = "0.3.0"


def build(chain):
    entries = []
    for e in chain:
        e = dict(e)
        if e["type"] == "enum":                 # module.json carries the NAME
            e["default"] = e["options"][e["default"]]
        entries.append(e)
    return {
        "id": "shimmerwash",
        "name": "Shimwash",
        "abbrev": "SHM",
        "version": VERSION,
        "description": ("Shoegaze shimmer reverb: two independent pitch shifters "
                        "(pre / feedback / post), input filter, ducker, freeze, "
                        "factory presets."),
        "author": "Dan",
        "dsp": "shimmerwash.so",
        "api_version": 2,
        "component_type": "audio_fx",
        "capabilities": {
            "audio_out": True, "audio_in": True,
            "midi_in": False, "midi_out": False,
            "chainable": True, "component_type": "audio_fx",
            "chain_params": entries,
        },
    }


def main():
    so = sys.argv[1]
    fx = Fx(so)
    chain = json.loads(fx.get("chain_params"))
    rendered = json.dumps(build(chain), indent=1) + "\n"
    size = len(rendered.encode())
    if size > CAP:
        print("FAIL: module.json is %d bytes, over the %d cap" % (size, CAP))
        return 1
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text() != rendered:
            print("FAIL: src/module.json is stale; regenerate with tools/gen_module_json.py")
            return 1
        print("module.json ok: %d params, %d bytes (cap %d)" % (len(chain), size, CAP))
        return 0
    OUT.write_text(rendered)
    print("wrote %s: %d params, %d bytes (cap %d)" % (OUT, len(chain), size, CAP))
    return 0


if __name__ == "__main__":
    sys.exit(main())
