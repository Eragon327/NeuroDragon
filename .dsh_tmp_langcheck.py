import json, re, subprocess, os, sys

root = r"D:\Documents\NeuroDragon"
en = json.load(open(os.path.join(root, "src/lang/en_US.json"), encoding="utf-8"))
zh = json.load(open(os.path.join(root, "src/lang/zh_CN.json"), encoding="utf-8"))

def flat(d, prefix=""):
    out = {}
    for k, v in d.items():
        if isinstance(v, dict):
            out.update(flat(v, prefix + k + "."))
        else:
            out[prefix + k] = v
    return out

fe, fz = flat(en), flat(zh)
print("en keys", len(fe), "zh keys", len(fz))
print("only en:", sorted(set(fe) - set(fz)))
print("only zh:", sorted(set(fz) - set(fe)))

# collect referenced keys in src
srcroot = os.path.join(root, "src")
pat = re.compile(r'"([a-zA-Z0-9_]+\.[a-zA-Z0-9_.]+)"')
refs = {}
for dirpath, _, files in os.walk(srcroot):
    for fn in files:
        if fn.endswith((".cpp", ".h")):
            p = os.path.join(dirpath, fn)
            txt = open(p, encoding="utf-8-sig", errors="replace").read()
            for m in pat.finditer(txt):
                key = m.group(1)
                refs.setdefault(key, set()).add(os.path.relpath(p, root))

def looks_like_i18n(k):
    return any(k.startswith(p) for p in ("nr.", "ui.", "log.", "config."))

missing = {}
for k, files in sorted(refs.items()):
    if not looks_like_i18n(k):
        continue
    if k not in fe or k not in fz:
        missing[k] = (files, k in fe, k in fz)
print("\nreferenced but missing in lang:")
for k, v in missing.items():
    print(" ", k, "en" if v[1] else "-", "zh" if v[2] else "-", sorted(v[0]))

# placeholder check between en and zh for every key
place = re.compile(r"\{(\d+)(?::[^}]*)?\}")
mismatch = []
for k in sorted(set(fe) & set(fz)):
    if isinstance(fe[k], str) and isinstance(fz[k], str):
        a = place.findall(fe[k]); b = place.findall(fz[k])
        if a != b:
            mismatch.append((k, fe[k], fz[k]))
print("\nplaceholder mismatch en/zh:")
for m in mismatch:
    print(" ", m)
