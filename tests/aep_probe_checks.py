#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Small executable checks for the measured binary AEP probe, independent of AE."""
from pathlib import Path
import math
import struct
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from aep_probe import inspect, edit_position, parse

root=Path(__file__).resolve().parents[1]
data=(root/'fixtures/ae26/synthetic.aep').read_bytes()
report=inspect(data)
comp=report['compositions'][0]
assert (comp['width'],comp['height'],comp['fps'],comp['duration'])==(64,64,[30,1],[2,1])
assert len(comp['layers'])==2 and len(report['sources'])==2
solid=next(l for l in comp['layers'] if l['name']=='Original solid')
assert [(k['time'],k['value']) for k in solid['position_keys']]==[([0,1],[16,16,0]),([1,1],[40,32,0])]
image=next(s for s in report['sources'] if s['kind']=='image')
assert (image['width'],image['height'])==(560,560) and image['path'].endswith('original-image.png')
color=next(s['color_rgba'] for s in report['sources'] if s['kind']=='solid')
assert all(abs(a-b)<1e-7 for a,b in zip(color,[.2,.4,.6,1]))
edited=edit_position(data,solid['id'],1,0,45)
offset=solid['position_keys'][1]['value_offset']
assert data[:offset]==edited[:offset] and data[offset+8:]==edited[offset+8:]
assert len(data)==len(edited) and report['preserved_opaque_trailer_bytes']>0
actual=next(l for l in inspect(edited)['compositions'][0]['layers'] if l['id']==solid['id'])
reference=next(l for l in inspect((root/'fixtures/ae26/authored-variant.aep').read_bytes())['compositions'][0]['layers'] if l['id']==solid['id'])
assert actual['position_keys']==reference['position_keys']

def rejects(fn):
    try:
        fn()
    except ValueError:
        return True
    return False

assert rejects(lambda:parse(data[:100]))
assert rejects(lambda:parse(b'not an AEP'))
malformed=bytearray(data);struct.pack_into('>I',malformed,16,2**32-1)
assert rejects(lambda:parse(malformed))
assert rejects(lambda:edit_position(data,solid['id'],-1,0,40))
assert rejects(lambda:edit_position(data,solid['id'],1,3,40))
assert rejects(lambda:edit_position(data,solid['id'],1,0,math.nan))
malformed=bytearray(data);malformed[offset-52]=0
assert rejects(lambda:inspect(malformed))
print('AEP probe: metadata, media reference, independent keys, exact edit isolation and 7 negative cases passed')
