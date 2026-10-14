#!/usr/bin/env python3
"""Write an index.html listing the signed update files in a directory (GitHub Pages mirror of a release)."""
import hashlib, html, sys
from pathlib import Path

d = Path(sys.argv[1])
tag = sys.argv[2] if len(sys.argv) > 2 else ''
rows = []
for f in sorted(d.iterdir()):
    if f.name == 'index.html' or not f.is_file(): continue
    rows.append(f'<tr><td><a href="{html.escape(f.name)}">{html.escape(f.name)}</a></td><td>{f.stat().st_size:,}</td>'
                f'<td><code>{hashlib.sha256(f.read_bytes()).hexdigest()}</code></td></tr>')
page = f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Ultimatrix updates</title>
<style>
  body{{margin:0;background:#050809;color:#e8f4f8;font:15px/1.5 system-ui,sans-serif}}
  main{{max-width:960px;margin:0 auto;padding:32px 16px 64px}} a{{color:#46d2ff}}
  table{{border-collapse:collapse;width:100%}} td,th{{border-bottom:1px solid #1c272c;padding:6px 8px;text-align:left;vertical-align:top}}
  code{{font-size:12px;word-break:break-all}} p{{color:#8aa0a8}}
</style></head><body><main>
<h1>Ultimatrix updates {html.escape(tag)}</h1>
<p>Signed application images and manifests. Clocks fetch these from the
<a href="https://github.com/vandalon/ultimatrix/releases/latest">GitHub release</a> and verify the signature themselves;
this page is a mirror for manual download. To install on a new board use the <a href="../">web flasher</a>.</p>
<table><tr><th>File</th><th>Bytes</th><th>SHA-256</th></tr>{''.join(rows)}</table>
</main></body></html>
'''
(d / 'index.html').write_text(page)
