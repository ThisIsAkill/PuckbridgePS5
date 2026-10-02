#!/usr/bin/env python3
"""Embed web/index.html (with the button icons in web/icons inlined) into webui_html.h"""
import base64, glob, json, os

html = open('web/index.html', encoding='utf-8').read()

# Inline web/icons/*.png as data URIs at the /*__ICONS__*/{} placeholder.
icons = {}
for p in sorted(glob.glob('web/icons/*.png')):
    key = os.path.splitext(os.path.basename(p))[0]
    icons[key] = 'data:image/png;base64,' + base64.b64encode(open(p, 'rb').read()).decode()
html = html.replace('/*__ICONS__*/{}', json.dumps(icons, separators=(',', ':')))

d = html.encode('utf-8')
out = ['/* generated from web/index.html by gen_html.py — do not edit */', 'static const unsigned char webui_html[] = {']
for i in range(0, len(d), 16):
    out.append('  ' + ','.join('0x%02x' % b for b in d[i:i+16]) + ',')
out += ['};', 'static const unsigned int webui_html_len = %d;' % len(d)]
open('webui_html.h', 'w').write('\n'.join(out) + '\n')
print('embedded %d bytes, %d icons' % (len(d), len(icons)))
