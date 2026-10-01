#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Experimental direct AEP inspection and one measured linear-position-key edit.

Original implementation from synthetic AE 26.2.1x2 files. This is not the app's
project importer. Unknown bytes are retained; arbitrary AEP semantics/rendering
remain unverified. No AE process, XML conversion or third-party parser is used.
"""
import argparse
from dataclasses import dataclass
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import struct


@dataclass
class Chunk:
    tag: bytes
    kind: bytes
    offset: int
    payload: bytes
    children: list


def walk(nodes):
    for node in nodes:
        yield node
        yield from walk(node.children)


def parse(data):
    if len(data) > 32 * 1024**2:
        raise ValueError('Probe limit: AEP exceeds 32 MiB')
    if len(data) < 12 or data[:4] != b'RIFX' or data[8:12] != b'Egg!':
        raise ValueError('Unsupported AEP container: expected big-endian RIFX/Egg!')
    limit = 8 + struct.unpack_from('>I', data, 4)[0]
    if not 12 <= limit <= len(data):
        raise ValueError('Truncated AEP container')
    count = 0

    def read(start, end, depth):
        nonlocal count
        if depth > 32:
            raise ValueError('AEP nesting exceeds probe limit')
        result = []
        while start < end:
            if end - start < 8:
                raise ValueError('Incomplete chunk header')
            tag = data[start:start+4]
            size = struct.unpack_from('>I', data, start+4)[0]
            stop = start + 8 + size
            padded = stop + (size & 1)
            if stop > end or padded > end:
                raise ValueError('Chunk exceeds its parent')
            count += 1
            if count > 100000:
                raise ValueError('AEP chunk count exceeds probe limit')
            container = tag == b'LIST'
            if container and size < 4:
                raise ValueError('Container has no subtype')
            kind = data[start+8:start+12] if container else b''
            children = read(start+12, stop, depth+1) if container else []
            result.append(Chunk(tag, kind, start, b'' if container else data[start+8:stop], children))
            start = padded
        return result

    return read(12, limit, 0), limit


def one(nodes, tag):
    found = [n for n in nodes if n.tag == tag]
    if len(found) != 1:
        raise ValueError('Expected exactly one ' + tag.decode('ascii') + ' record')
    return found[0]


def text(data):
    return data.split(b'\0')[0].decode('utf-8')


def number(data, offset, fmt='>I'):
    if offset + struct.calcsize(fmt) > len(data):
        raise ValueError('Incomplete measured record layout')
    return struct.unpack_from(fmt, data, offset)[0]


def rational(n, d):
    if d <= 0:
        raise ValueError('Invalid time denominator')
    value = Fraction(n, d)
    return [value.numerator, value.denominator]


def named_property(nodes, name):
    for index, node in enumerate(nodes):
        if node.tag == b'tdmn' and text(node.payload) == name:
            if index+1 >= len(nodes) or nodes[index+1].kind != b'tdbs':
                raise ValueError('Unsupported property representation: ' + name)
            return nodes[index+1]
        result = named_property(node.children, name)
        if result is not None:
            return result
    return None


def position_keys(layer):
    prop = named_property(layer.children, 'ADBE Position')
    if prop is None:
        return []
    definition = one(prop.children, b'tdb4').payload
    if len(definition) != 124 or number(definition, 2, '>H') != 3:
        raise ValueError('Unsupported position definition')
    table = next((n for n in prop.children if n.kind == b'list'), None)
    if table is None:
        raise ValueError('Position is not the measured keyed representation')
    header = one(table.children, b'lhd3').payload
    data = one(table.children, b'ldat')
    count, stride = number(header, 8), number(header, 16)
    if number(header, 0) != 0x00d00bee or stride != 128 or count*stride != len(data.payload):
        raise ValueError('Unsupported position key record layout')
    result = []
    for index in range(count):
        start = index*stride
        record = data.payload[start:start+stride]
        if record[4:8] != bytes.fromhex('01010007'):
            raise ValueError('Unsupported position interpolation flags')
        values = list(struct.unpack_from('>ddd',record,56))
        if not all(math.isfinite(v) for v in values):
            raise ValueError('Nonfinite position key')
        result.append(dict(time=rational(number(record,0,'>i'),number(definition,12)), value=values,
                           value_offset=data.offset+8+start+56))
    return result


def inspect(data):
    roots, end = parse(data)
    if one(roots, b'head').payload[:8] != bytes.fromhex('006100050f910e02'):
        raise ValueError('Unsupported AEP file-version signature')
    items = [n for n in walk(roots) if n.kind == b'Item']
    sources = {}
    for item in items:
        if any(n.tag == b'cdta' for n in item.children):
            continue
        pin = next((n for n in item.children if n.kind == b'Pin '), None)
        if pin is None:
            continue
        desc = next((n for n in walk(pin.children) if n.tag == b'sspc'),None)
        if desc is None:
            continue
        if len(desc.payload) != 222:
            raise ValueError('Unsupported source record layout')
        source_id = number(one(item.children,b'iide').payload,0,'<I')
        source = dict(id=source_id,width=number(desc.payload,30),height=number(desc.payload,34))
        options = one(list(walk(pin.children)),b'opti').payload
        if options[:4] == b'Soli':
            a, r, g, b = struct.unpack_from('>ffff',options,10)
            source.update(kind='solid',name=text(options[26:]),color_rgba=[r,g,b,a])
        elif options[:4] == b'png!':
            alias = json.loads(one(list(walk(pin.children)),b'alas').payload)
            source.update(kind='image',path=alias['fullpath'],name=Path(alias['fullpath']).name)
        else:
            source.update(kind='unsupported',signature=options[:4].decode('latin1'))
        if not 0 < source['width'] <= 8192 or not 0 < source['height'] <= 8192:
            raise ValueError('Source dimensions exceed measured probe range')
        if source_id in sources:
            raise ValueError('Duplicate source ID')
        sources[source_id]=source
    compositions=[]
    for item in items:
        definition=next((n.payload for n in item.children if n.tag==b'cdta'),None)
        if definition is None:
            continue
        if len(definition)!=204:
            raise ValueError('Unsupported composition record layout')
        comp=dict(id=number(one(item.children,b'iide').payload,0,'<I'),
                  name=text(one(item.children,b'Utf8').payload),width=number(definition,140,'>H'),height=number(definition,142,'>H'),
                  fps=rational(number(definition,8),number(definition,4)),duration=rational(number(definition,44),number(definition,48)),layers=[])
        for layer in (n for n in item.children if n.kind==b'Layr'):
            meta=one(layer.children,b'ldta').payload
            if len(meta)!=164:
                raise ValueError('Unsupported layer record layout')
            source_id=number(meta,40)
            record=dict(id=number(meta,0),source=source_id,name=text(one(layer.children,b'Utf8').payload) or sources.get(source_id,{}).get('name',''),
                        start=rational(number(meta,12,'>i'),number(meta,16)),in_time=rational(number(meta,20,'>i'),number(meta,24)),out_time=rational(number(meta,28,'>i'),number(meta,32)),
                        position_keys=position_keys(layer))
            scale=named_property(layer.children,'ADBE Scale')
            if scale is not None:
                value=one(scale.children,b'cdat').payload
                if len(value)!=120: raise ValueError('Unsupported scale representation')
                record['static_scale']=list(struct.unpack_from('>ddd',value,0))
                if not all(math.isfinite(v) and abs(v)<=1e12 for v in record['static_scale']):
                    raise ValueError('Invalid scale value')
            comp['layers'].append(record)
        compositions.append(comp)
    return dict(status='experimental-subset',native_import_supported=False,render_compatibility='unverified',
                observed_reference='AE 26.2.1x2 synthetic fixture',sha256=hashlib.sha256(data).hexdigest(),compositions=compositions,
                sources=list(sources.values()),preserved_opaque_trailer_bytes=len(data)-end,
                limitation='Only measured record layouts are decoded. Other properties, flags and unknown records are not semantically interpreted. This output is not a native scene import.')


def edit_position(data, layer_id, key_index, component, value):
    if component not in (0,1,2) or not math.isfinite(value) or abs(value)>1e12:
        raise ValueError('Invalid position component/value')
    report=inspect(data)
    layers=[layer for comp in report['compositions'] for layer in comp['layers'] if layer['id']==layer_id]
    if len(layers)!=1 or not 0 <= key_index < len(layers[0]['position_keys']):
        raise ValueError('Unknown layer/key')
    key=layers[0]['position_keys'][key_index]
    offset=key['value_offset']+8*component
    output=bytearray(data)
    struct.pack_into('>d',output,offset,value)
    # Preserve all unknown metadata and source bytes, including the opaque XMP trailer.
    if output[:offset]!=data[:offset] or output[offset+8:]!=data[offset+8:]:
        raise AssertionError('Edit escaped its eight-byte field')
    inspect(output)
    return bytes(output)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('input',type=Path)
    p.add_argument('--output',type=Path)
    p.add_argument('--layer',type=int)
    p.add_argument('--key',type=int,default=0,help='zero-based key index')
    p.add_argument('--component',type=int,default=0)
    p.add_argument('--value',type=float)
    args=p.parse_args()
    with args.input.open('rb') as source:
        data=source.read(32 * 1024**2 + 1)
    if args.output:
        if args.layer is None or args.value is None: p.error('Editing needs --layer and --value')
        edited=edit_position(data,args.layer,args.key,args.component,args.value)
        with args.output.open('xb') as f: f.write(edited)
        print(json.dumps(dict(status='experimental-key-edit',changed_bytes=sum(a!=b for a,b in zip(data,edited)),bytes=len(edited)),indent=2))
    else:
        print(json.dumps(inspect(data),indent=2))

if __name__=='__main__':
    main()
