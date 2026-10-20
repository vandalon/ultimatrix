#!/usr/bin/env python3
"""Checks that every key stored in the "ultimatrix" NVS namespace is in kKnownKeys (src/main.cpp), which erases
everything else at every boot, and that the Home cleanup list covers every Home key. Heuristic: reads string literals
passed to Preferences/cacheblob calls and "name%d" formats in files that open the namespace. Run: python3 tools/check-nvs-keys.py"""
import pathlib, re, sys
root = pathlib.Path(__file__).resolve().parent.parent
main = (root / "src/main.cpp").read_text()
known = set(re.findall(r'"([A-Za-z0-9]+)"', re.search(r"kKnownKeys\[\] = \{(.*?)\};", main, re.S).group(1)))
homeList = set(re.findall(r'"([A-Za-z0-9]+)"', re.search(r"homeKeys\[\] = \{(.*?)\};", main, re.S).group(1)))
call = re.compile(r'(?:\.|::)(?:get\w*|put\w*|isKey|remove|getBytesLength)\(\s*(?:\w+\s*,\s*)?"([A-Za-z][A-Za-z0-9]*)"')
fmt = re.compile(r'snprintf\(\w+,\s*[^,]+,\s*"([A-Za-z][A-Za-z0-9]*)%d"')
used = {}
for path in list((root / "src").glob("*.cpp")) + list((root / "lib").rglob("*.cpp")) + list((root / "lib").rglob("*.h")):
    text = path.read_text(errors="ignore")
    # FirmwareUpdate keeps its keys in "um-updates"; Settings.cpp keeps one CRC'd blob plus the legacy V1 keys of the migration (not scanned)
    if '"ultimatrix"' not in text or path.name in ("klipy_key.h", "FirmwareUpdate.cpp", "Settings.cpp"): continue
    for m in call.finditer(text): used.setdefault(m.group(1), path.name)
    for m in fmt.finditer(text): used.setdefault(m.group(1) + "%d", path.name)
bad = []
for key, file in sorted(used.items()):
    if key.endswith("%d"):
        if not any(k.startswith(key[:-2]) for k in known): bad.append("%s (%s): no known key starts with it" % (key, file))
    elif key not in known:
        bad.append("%s (%s): not in kKnownKeys, it would be erased at boot" % (key, file))
    if len(key) > 15: bad.append("%s (%s): NVS keys are 15 characters at most" % (key, file))
for key in sorted(k for k in known if k.startswith("ha")):
    if key not in homeList: bad.append("%s: Home key missing from homeKeys, it survives the crowded-NVS cleanup" % key)
for line in bad: print("NVS check:", line)
print("NVS check: %d keys found in code, %d allowed, %d problem(s)" % (len(used), len(known), len(bad)))
sys.exit(1 if bad else 0)
