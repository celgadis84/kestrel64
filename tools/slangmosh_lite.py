#!/usr/bin/env python3
"""Regenerate parallel-rdp's shaders/slangmosh.hpp SPIR-V bank.

Upstream bakes the compiled shaders into that header with Themaister's `slangmosh`
tool, which lives in Granite and is not vendored here.  kestrel has to patch the
shaders (parallel-rdp assumes ares' word-swapped RDRAM layout, kestrel keeps the
guest's big-endian byte order), so the bank has to be rebuilt from source.

Only the SPIR-V changes.  The reflection bank describes descriptor sets, push
constants and spec constants, and the patches never touch a binding, so those
bytes are reused verbatim.  The generator rewrites the header in place: it reads
the existing constructor to recover the exact shader/variant order slangmosh
emitted, recompiles every permutation with glslangValidator, and patches the
(offset, size) pair of each request_program/request_shader call.

    python tools/slangmosh_lite.py [--check]

--check compiles everything and reports what would change, without writing.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHD = os.path.join(ROOT, 'third_party', 'parallel-rdp', 'parallel-rdp', 'shaders')
HPP = os.path.join(SHD, 'slangmosh.hpp')
JSON = os.path.join(SHD, 'slangmosh.json')
# Granite resolves #include itself; glslangValidator wants the GOOGLE extension
# enabled, so inject it as a preamble instead of editing every shader.
PREAMBLE = '-P#extension GL_GOOGLE_include_directive : require'
TARGET_ENV = 'vulkan1.1'   # SPIR-V 1.3, same as the upstream bank

CALL = re.compile(r'this->(\w+)(?:\[(\d+)\])?\s*=\s*device\.request_(program|shader)'
                  r'\(spirv_bank \+ (\d+), (\d+), &layout\);')
COND = re.compile(r'resolver\("(\w+)", "(\w+)"\) == (\d+)')
IF_OR_CALL = re.compile(r'if \((.*?)\)\r?\n\t\t\{|' + CALL.pattern, re.S)


def load_json():
    spec = json.load(open(JSON, encoding='utf-8'))
    out = {}
    for s in spec['shaders']:
        stage = 'comp' if s.get('compute') else os.path.splitext(s['path'])[1][1:]
        out[s['name']] = (s['path'], stage, s.get('variants', []))
    return out


def parse_calls(text):
    """Recover the emitted order as a list of call records."""
    ctor = text.index('Shaders(Device &device, Layout &layout')
    body = text[ctor:]
    calls, pending = [], {}
    for m in IF_OR_CALL.finditer(body):
        if m.group(1) is not None:
            # `if (resolver(...) == k && ...)` scopes the call that follows it.
            pending = {d: int(v) for _, d, v in COND.findall(m.group(1))}
            continue
        calls.append(dict(name=m.group(2),
                          index=None if m.group(3) is None else int(m.group(3)),
                          off=int(m.group(5)), size=int(m.group(6)),
                          span=(ctor + m.start(5), ctor + m.end(6)),
                          defines=pending))
        pending = {}
    return calls


def defines_for(call, variants):
    """Variants without "resolve" become arrays; decode the index (first define fastest)."""
    if call['defines']:
        return call['defines']
    if call['index'] is None:
        return {}
    i, out = call['index'], {}
    for v in variants:
        out[v['define']] = i % v['count']
        i //= v['count']
    return out


def compile_one(path, stage, defines, tmp):
    raw = os.path.join(tmp, 'raw.spv')
    out = os.path.join(tmp, 'out.spv')
    cmd = (['glslangValidator', '-V', '--target-env', TARGET_ENV, '-S', stage,
            '-I' + SHD, PREAMBLE] +
           ['-D%s=%d' % kv for kv in sorted(defines.items())] +
           [os.path.join(SHD, path), '-o', raw])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit('glslangValidator failed for %s %s' % (path, defines))
    # Raw glslang output keeps every temporary in a function-local OpVariable. The
    # RX 570's driver does not recover from that in the ubershader: SM64 goes from
    # ~23 to under 1 field per second. slangmosh runs the SPIRV-Tools optimizer, so
    # we do too.
    r = subprocess.run(['spirv-opt', '-O', raw, '-o', out], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit('spirv-opt failed for %s %s' % (path, defines))
    return open(out, 'rb').read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    spec = load_json()
    text = open(HPP, encoding='utf-8', newline='').read()
    eol = '\r\n' if '\r\n' in text else '\n'
    calls = parse_calls(text)

    blobs, total = [], 0
    with tempfile.TemporaryDirectory() as tmp:
        for c in calls:
            path, stage, variants = spec[c['name']]
            c['defines'] = defines_for(c, variants)
            blob = compile_one(path, stage, c['defines'], tmp)
            assert len(blob) % 4 == 0
            c['new'] = (total, len(blob))
            total += len(blob) // 4
            blobs.append(blob)
    print('%d permutations, %d SPIR-V words (bank was %d)' %
          (len(calls), total, calls[-1]['off'] + calls[-1]['size'] // 4))

    if args.check:
        for c in calls:
            if (c['off'], c['size']) != c['new']:
                print('  %-32s %-46s %s -> %s' %
                      (c['name'], c['defines'], (c['off'], c['size']), c['new']))
        return

    data = b''.join(blobs)
    ws = ['0x%08xu' % int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
    rows = ['\t' + ', '.join(ws[i:i + 8]) + ',' for i in range(0, len(ws), 8)]
    bank = ('static const uint32_t spirv_bank[] =' + eol + '{' + eol +
            eol.join(rows) + eol + '};')

    # Patch the call sites back to front so the earlier spans stay valid, then
    # splice the new bank in (it sits before the constructor).
    out = text
    for c in sorted(calls, key=lambda c: -c['span'][0]):
        a, b = c['span']
        out = out[:a] + '%d, %d' % c['new'] + out[b:]
    start = out.index('static const uint32_t spirv_bank[] =')
    end = out.index(eol + '};', start) + len(eol) + 2
    out = out[:start] + bank + out[end:]
    open(HPP, 'w', encoding='utf-8', newline='').write(out)
    print('wrote %s (%.1f MiB)' % (HPP, len(out) / (1 << 20)))


main()
