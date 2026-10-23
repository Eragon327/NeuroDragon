import re, subprocess, os, json

root = r"D:\Documents\NeuroDragon"

old = subprocess.run(["git", "-C", root, "show", "HEAD:src/neurodragon/ui/Glyphs.h"],
                     capture_output=True, text=True, encoding="utf-8").stdout
new = open(os.path.join(root, "src/neurodragon/ui/Glyphs.h"), encoding="utf-8").read()

def chars(text):
    m = re.search(r'kUiGlyphs\[\] = "(.*)";', text)
    s = m.group(1)
    return set(s)

o, n = chars(old), chars(new)
print("removed chars:", "".join(sorted(o - n)))
print("added chars:", "".join(sorted(n - o)))

# chars used in zh_CN.json
zh = open(os.path.join(root, "src/lang/zh_CN.json"), encoding="utf-8").read()
zhdata = json.loads(zh)
used = set()
def walk(d):
    for v in d.values():
        if isinstance(v, dict): walk(v)
        elif isinstance(v, str):
            used.update(ch for ch in v if '\u4e00' <= ch <= '\u9fff')
walk(zhdata)
print("\nzh lang chars missing from glyphs:", "".join(sorted(used - n)))

# chars used in C++ sources (Chinese literals)
src_used = set()
for dirpath, _, files in os.walk(os.path.join(root, "src")):
    for fn in files:
        if fn.endswith((".cpp", ".h")):
            txt = open(os.path.join(dirpath, fn), encoding="utf-8-sig", errors="replace").read()
            src_used.update(ch for ch in txt if '\u4e00' <= ch <= '\u9fff')
print("cpp chars missing from glyphs:", "".join(sorted(src_used - n)))
