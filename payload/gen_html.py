#!/usr/bin/env python3
"""Embed web/index.html into webui_html.h"""
d = open('web/index.html', 'rb').read()
out = ['/* generated from web/index.html by gen_html.py — do not edit */', 'static const unsigned char webui_html[] = {']
for i in range(0, len(d), 16):
    out.append('  ' + ','.join('0x%02x' % b for b in d[i:i+16]) + ',')
out += ['};', 'static const unsigned int webui_html_len = %d;' % len(d)]
open('webui_html.h', 'w').write('\n'.join(out) + '\n')
