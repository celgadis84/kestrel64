#!/usr/bin/env python3
"""One RDP frame of any ROM: raw command dump + who spends the pixels (any SDK / port / native ROM).
   python tools/rdpframe.py <rom> [--frames N | --sym MAP SYMBOL VALUE] [--dump FILE]
                           [--poke MAP SYMBOL VALUE] [--port 9474] [--top 15] [--exe kestrel64.exe]
Boots, runs N frames (default 300) or until the u32 SYMBOL (address from a GNU ld / nm map)
reaches VALUE, records one frame with the kestrel64 debugger (dbg.rdp.frame: RDRAM and XBUS
command lists alike) and prints: area per (other modes, combiner, texture image), duplicate
triangles, redundant texture loads, an HW cost estimate (fit from SDK demo 54), and a
coplanar-merge bound. --dump writes the frame's raw RDP words (big-endian u64) to FILE, to be
replayed on HW / emulators. --poke writes one byte at SYMBOL every 10 frames while waiting.
Area from the edge coefficients, scissor ignored: an estimate of the pixels walked."""
import argparse, os, re, struct, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mcp'))
ap = argparse.ArgumentParser()
ap.add_argument('rom')
ap.add_argument('--frames', type=int, default=300)
ap.add_argument('--sym', nargs=3, metavar=('MAP', 'SYMBOL', 'VALUE'))
ap.add_argument('--poke', nargs=3, metavar=('MAP', 'SYMBOL', 'VALUE'))
ap.add_argument('--dump')
ap.add_argument('--port', default='9474')
ap.add_argument('--top', type=int, default=15)
ap.add_argument('--rec', type=int, default=8, help='VI fields to record (slow games need more than one frame time)')
ap.add_argument('--stall', type=float, default=60, help='host seconds without SYMBOL moving = hang/crash: abort')
ap.add_argument('--pad', nargs=2, metavar=('BUTTONS', 'AT'), help='hold BUTTONS + stick up once SYMBOL >= AT (games that need input)')
ap.add_argument('--log', help='emulator stdout (default: next to --dump, or the temp dir)')
ap.add_argument('--exe', default=os.environ.get('KESTREL_EXE', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build-static', 'kestrel64.exe')))
a_ = ap.parse_args()
port, top = a_.port, a_.top


def sym_addr(mp, name):
    for l in open(mp, errors='replace'):
        m = re.match(r'\s*(?:0x)?([0-9a-fA-F]{8,16})\s+(?:[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+)?(?:[a-zA-Z]\s+)?' + re.escape(name) + r'\s*$', l)
        if m: return int(m.group(1), 16) & 0x1FFFFFFF
    sys.exit(f'{name} not in {mp}')


addr = sym_addr(a_.sym[0], a_.sym[1]) if a_.sym else None
gf = int(a_.sym[2], 0) if a_.sym else 0
ea = sym_addr(a_.poke[0], a_.poke[1]) if a_.poke else None
os.environ['KESTREL_TELEMETRY_PORT'] = port
import kestrel_mcp as k
env = dict({'KESTREL_CACHECOST': '1'}, **dict(os.environ, KESTREL_HUD_OFF='1', KESTREL_AUDIO='0'))   # caller's KESTREL_CACHECOST wins
import tempfile
logpath = a_.log or ((a_.dump + '.log') if a_.dump else os.path.join(tempfile.gettempdir(), 'rdpframe_%s.log' % port))
logf = open(logpath, 'wb')
# cwd = the ROM's folder: ports read their SD / fs data relative to it
proc = subprocess.Popen([a_.exe, os.path.abspath(a_.rom), '--port', port], env=env, cwd=os.path.dirname(os.path.abspath(a_.rom)),
                        stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT)


def crashed():
    """The guest's crash / panic banner in the emulator log, or the emulator gone."""
    if proc.poll() is not None: return 'emulator exited rc=%s' % proc.returncode
    try:
        t = open(logpath, 'rb').read().decode('latin-1')
    except OSError:
        return None
    for key in ('*** CRASH', 'PANIC', 'panic'):
        i = t.find(key)
        if i >= 0: return t[i:i + 300].strip()
    return None


SW, SH = 320, 240                      # screen (scissor) the area is clipped to


def s32(v): return v - (1 << 32) if v & 0x80000000 else v
def s14(v): return v - (1 << 14) if v & 0x2000 else v


def tri_area(w):
    w0, w1, w2, w3 = w[0], w[1], w[2], w[3]
    yl, ym, yh = s14((w0 >> 32) & 0x3FFF) / 4, s14((w0 >> 16) & 0x3FFF) / 4, s14(w0 & 0x3FFF) / 4
    xl, dl = s32(w1 >> 32) / 65536, s32(w1 & 0xFFFFFFFF) / 65536
    xh, dh = s32(w2 >> 32) / 65536, s32(w2 & 0xFFFFFFFF) / 65536
    xm, dm = s32(w3 >> 32) / 65536, s32(w3 & 0xFFFFFFFF) / 65536
    y0 = int(yh)                       # XH / XM are at the scanline of YH, XL at YM
    a = 0.0
    for y in range(max(0, y0), min(SH, int(yl + 0.999))):
        t = y + 0.5 - y0
        if t < 0: continue
        x1 = xh + dh * t
        x2 = xm + dm * t if y + 0.5 < ym else xl + dl * (y + 0.5 - ym)
        lo, hi = max(0.0, min(x1, x2)), min(float(SW), max(x1, x2))
        if hi > lo: a += hi - lo
    return a


def rect_area(w):
    w0, w1 = w[0], w[1] if len(w) > 1 else 0
    xl, yl, xh, yh = (w0 >> 44) & 0xFFF, (w0 >> 32) & 0xFFF, (w0 >> 12) & 0xFFF, w0 & 0xFFF
    return max(0, (xl - xh) / 4 + 1) * max(0, (yl - yh) / 4 + 1)


try:
    for _ in range(120):
        try:
            if k.emu_status() is not None: break
        except Exception: time.sleep(0.25)
    if addr is None:
        k.frame_advance(a_.frames, timeout_ms=600000)
    # RAM garbage before .bss is cleared can read >= VALUE: only accept VALUE..VALUE+0xFFFF
    last, moved, padded = None, time.time(), False
    while addr is not None:
        v = int(k.read_memory('rdram', addr, 4, coherent=True)['hex'], 16)
        if gf <= v < gf + 0x10000: break
        if v != last: last, moved = v, time.time()
        why = crashed() or (time.time() - moved > a_.stall and '%s stuck at %d for %.0f s' % (a_.sym[1], v, a_.stall))
        if why:
            shot = (a_.dump or os.path.join(tempfile.gettempdir(), 'rdpframe')) + '.hang.png'
            try: k.capture_framebuffer(shot)
            except Exception: pass
            sys.exit('ABORT: %s\n  log %s\n  screen %s' % (why, logpath, shot))
        if a_.pad and not padded and v >= int(a_.pad[1], 0) and v < gf + 0x10000:
            k.controller_set(buttons=a_.pad[0], stick_y=80, polls=-1)
            padded = True
        k.frame_advance(10, timeout_ms=600000)
        if ea is not None: k.write_memory('rdram', ea, '%02x' % (int(a_.poke[2], 0) & 0xFF))
    k.dbg_enable(True)
    k.frame_advance(4, timeout_ms=600000)
    s0, k0 = k.dbg_status(), k.query('rdp.stats.reset')
    k.frame_advance(a_.rec, timeout_ms=600000)
    s1, st = k.dbg_status(), k.query('rdp.stats')[0]
    print('dbg frames', s0.get('frames'), '->', s1.get('frames'), ' flips', st['flips'], ' ops', dict(sorted(st['ops'].items(), key=lambda x: -x[1])[:4]))
    k.dbg_freeze(True)
    cmds, start = [], 0
    while True:
        d = k.dbg_rdp_frame(frame=0, start=start, count=5000, decode=False)
        cmds += d['cmds']
        start += len(d['cmds'])
        if not d['cmds'] or start >= d['total']: break
    dump = a_.dump or os.environ.get('RDPDUMP')
    if dump:                               # raw frame list (big-endian u64 words)
        with open(dump, 'wb') as f:
            for c in cmds:
                for x in c['words']: f.write(struct.pack('>Q', int(x, 16)))
        print('dumped', len(cmds), 'cmds to', dump)
    groups, om, cc, timg = {}, 0, 0, 0
    tot = 0.0
    for c in cmds:
        op = c['op']
        w = [int(x, 16) for x in c['words']]
        if op == 0x2F: om = w[0] & 0x00FFFFFFFFFFFFFF
        elif op == 0x3C: cc = w[0] & 0x00FFFFFFFFFFFFFF
        elif op == 0x3D: timg = w[0] & 0x03FFFFFF
        elif 0x08 <= op <= 0x0F or op in (0x24, 0x25, 0x36):
            a = tri_area(w) if op < 0x10 else rect_area(w)
            key = (om, cc, timg if (op & 2) or op in (0x24, 0x25) else 0, op)
            g = groups.setdefault(key, [0.0, 0])
            g[0] += a; g[1] += 1; tot += a
    # Emitted twice? Same edge words (same screen geometry) seen before in this frame.
    seen, dup, dupa, hist, om2, timg2 = {}, 0, 0.0, {}, 0, 0
    for i, c in enumerate(cmds):
        op = c['op']
        hist[op] = hist.get(op, 0) + 1
        if op == 0x2F: om2 = int(c['words'][0], 16) & 0x00FFFFFFFFFFFFFF
        elif op == 0x3D: timg2 = int(c['words'][0], 16) & 0x03FFFFFF
        if 0x08 <= op <= 0x0F:
            w = tuple(int(x, 16) for x in c['words'][:4])
            if w in seen:
                dup += 1; a = tri_area(list(w)); dupa += a
                j, o0, t0 = seen[w]
                if dup <= 12: print(f'  dup #{i} of #{j} op {op:02x} {a:6.0f} px  om {o0:014x}->{om2:014x} timg {t0:06x}->{timg2:06x}  same words {c["words"] == cmds[j]["words"]}')
            seen[w] = (i, om2, timg2)
    # Texture loads (0x33 block, 0x34 tile, 0x30 tlut) that bring the same image + tile setup as
    # the last load into the same TMEM spot: the ucode could drop them.
    tmem, cur_ti, tiles, nld, red, redb = {}, 0, {}, 0, 0, 0
    for c in cmds:
        op, w0 = c['op'], int(c['words'][0], 16)
        if op == 0x3D: cur_ti = w0 & 0x00FFFFFFFFFFFFFF
        elif op == 0x35: tiles[(w0 >> 24) & 7] = w0 & 0x00FFFFFFFFFFFFFF
        elif op in (0x30, 0x33, 0x34):
            t = tiles.get((w0 >> 24) & 7, 0)
            addr = (t >> 32) & 0x1FF
            key = (cur_ti, t, w0)
            nld += 1
            if tmem.get(addr) == key:
                red += 1; redb += ((w0 >> 12) & 0xFFF) + 1 if op == 0x33 else 0
            tmem[addr] = key
    print(f'tex loads {nld}  redundant {red} ({redb} texels by LOAD_BLOCK)')
    # Sync audit: a SYNC_PIPE / TILE / LOAD is needed only if a primitive ran since the last
    # sync of its kind (else 'dup') AND what it guards changes before the next primitive
    # (pipe: modes / colours / images / scissor; tile: SET_TILE / TILE_SIZE / loads; load: a
    # load), else 'idle'. HW cost per sync (GCLK): pipe ~50, tile ~33, load ~25.
    PRIM = set(range(0x08, 0x10)) | {0x24, 0x25, 0x36}
    GUARD = {0x27: set(range(0x2D, 0x30)) | set(range(0x37, 0x40)) | {0x2C},
             0x28: {0x30, 0x32, 0x33, 0x34, 0x35},
             0x26: {0x30, 0x33, 0x34}}
    COST = {0x27: 50, 0x28: 33, 0x26: 25}
    since = {0x27: True, 0x28: True, 0x26: True}
    audit = {o: [0, 0, 0] for o in COST}        # total, dup, idle
    for i, c in enumerate(cmds):
        op = c['op']
        if op in PRIM:
            for o in since: since[o] = True
        elif op == 0x29:
            for o in since: since[o] = False
        elif op in COST:
            a = audit[op]; a[0] += 1
            if not since[op]: a[1] += 1
            else:
                for c2 in cmds[i + 1:]:
                    o2 = c2['op']
                    if o2 in GUARD[op]: break
                    if o2 in PRIM or o2 == 0x29: a[2] += 1; break
            since[op] = False
    waste = sum((audit[o][1] + audit[o][2]) * COST[o] for o in COST)
    print('syncs', '  '.join(f'{n} {audit[o][0]} (dup {audit[o][1]} idle {audit[o][2]})' for o, n in ((0x27, 'pipe'), (0x28, 'tile'), (0x26, 'load'))),
          f' full {hist.get(0x29, 0)}  waste ~{waste} clk ({waste / 62500:.2f} ms)')
    print(f'dup tris {dup}  dup area {dupa:.0f} px  ops', ' '.join(f'{o:02x}:{n}' for o, n in sorted(hist.items(), key=lambda x: -x[1])[:12]))
    # HW cost estimate from demo 54 (docs/data/rdptime_hw_2026-11-04.csv, tools/rdptime_fit.py):
    # clk = base + per line + per pixel, by Z / AA. Lines = scanlines the triangle spans.
    HW = {'': (38, 14, 1.0), 'R': (43, 39, 1.9), 'Z': (75, 49, 2.65), 'ZR': (72, 71, 3.3)}   # R = IM_RD (sweep b)
    est, om3, lines_tot, by = 0.0, 0, 0, {}
    for c in cmds:
        op = c['op']
        w = [int(x, 16) for x in c['words']]
        if op == 0x2F: om3 = w[0]
        elif 0x08 <= op <= 0x0F:
            yl, yh = s14((w[0] >> 32) & 0x3FFF) / 4, s14(w[0] & 0x3FFF) / 4
            ln = max(0, min(SH, int(yl + 0.999)) - max(0, int(yh)))
            a = tri_area(w)
            key = ('Z' if om3 & 0x10 else '') + ('R' if om3 & 0x40 else '')
            b, pl, pp = HW[key]
            cl = b + pl * ln + pp * a
            est += cl; lines_tot += ln
            g = by.setdefault(key or '-', [0, 0, 0.0, 0.0]); g[0] += 1; g[1] += ln; g[2] += a; g[3] += cl
        elif op in (0x24, 0x25, 0x36):
            a = rect_area(w); cl = 10 + 14 * 240 * a / 76800 + (0.6 if op == 0x36 else 1.0) * a
            est += cl; g = by.setdefault('rect', [0, 0, 0.0, 0.0]); g[0] += 1; g[2] += a; g[3] += cl
    print(f'HW est {est:.0f} clk = {est / 62500:.2f} ms  lines {lines_tot}')
    for kk, (n, ln, a, cl) in sorted(by.items(), key=lambda x: -x[1][3]):
        print(f'  {kk:5s} n {n:4d} lines {ln:6d} px {a:8.0f}  clk {cl:8.0f} ({cl / 62500:.2f} ms)  of which lines {100 * (HW.get(kk, (0, 0, 0))[1] * ln) / max(cl, 1):.0f}%')
    # Merge bound: triangles with the same render state whose Z, shade and texture planes are the
    # same (same gradients and same value at the origin) could be one bigger polygon (baker-side
    # coplanar merge). Merged cost bound per plane: 2 triangles covering its scanlines.
    def q(v, s=64): return int(round(s32(v) / s))
    planes, om4, cc4, ti4 = {}, 0, 0, 0
    for c in cmds:
        op = c['op']
        w = [int(x, 16) for x in c['words']]
        if op == 0x2F: om4 = w[0]
        elif op == 0x3C: cc4 = w[0]
        elif op == 0x3D: ti4 = w[0]
        elif 0x08 <= op <= 0x0F:
            yl, yh = s14((w[0] >> 32) & 0x3FFF) / 4, s14(w[0] & 0x3FFF) / 4
            xh = s32(w[2] >> 32) / 65536
            i, key = 4, [om4, cc4, ti4 if op & 2 else 0, op]
            blocks = []
            if op & 4:
                if 's' in os.environ.get('MERGE_KEYS', 'stz'): blocks.append(('s', w[i:i + 8]))
                i += 8
            if op & 2:
                if 't' in os.environ.get('MERGE_KEYS', 'stz'): blocks.append(('t', w[i:i + 8]))
                i += 8
            if op & 1: blocks.append(('z', w[i:i + 2])); i += 2
            for kind, b in blocks:
                if kind == 'z':
                    z0, dzdx, dzdy = s32(b[0] >> 32), s32(b[0] & 0xFFFFFFFF), s32(b[1] & 0xFFFFFFFF)
                    key += [round(dzdx / 4096), round(dzdy / 4096), round((z0 - dzdx * xh - dzdy * int(yh)) / 262144)]
                else:
                    # words: 0 value int, 2 frac; 1 d/dx int, 3 frac; 5 d/dy int, 7 frac (4 lanes)
                    def fx(wi, wf, sh):
                        v = (((b[wi] >> sh) & 0xFFFF) << 16) | ((b[wf] >> sh) & 0xFFFF)
                        return (v - (1 << 32) if v & 0x80000000 else v) / 65536
                    for lane in range(4):
                        sh = 48 - 16 * lane
                        v0, dx, dy = fx(0, 2, sh), fx(1, 3, sh), fx(5, 7, sh)
                        key += [round(dx * 64), round(dy * 64), round(v0 - dx * xh - dy * int(yh))]
            ln = max(0, min(SH, int(yl + 0.999)) - max(0, int(yh)))
            g = planes.setdefault(tuple(key), [0, 0, set()])
            g[0] += 1; g[1] += ln; g[2].update(range(max(0, int(yh)), min(SH, int(yl + 0.999))))
    sav_l = sav_n = 0
    for n, ln, rows in planes.values():
        if n > 2 and ln > 2 * len(rows): sav_l += ln - 2 * len(rows); sav_n += n - 2
    multi = sum(1 for n, _, _ in planes.values() if n > 1)
    print(f'merge bound: {len(planes)} planes ({multi} with >1 tri), save {sav_n} tris, {sav_l} lines '
          f'~ {(sav_n * 75 + sav_l * 50) / 62500:.2f} ms (Z model)')
    print(f'frame cmds {len(cmds)}  prims {sum(g[1] for g in groups.values())}  area {tot:.0f} px')
    for (o, cm, ti, op), (a, n) in sorted(groups.items(), key=lambda x: -x[1][0])[:top]:
        lo = o & 0xFFFFFFFF
        cyc = ('1c', '2c', 'cp', 'fl')[(o >> 52) & 3]
        fl = ''.join(f for f, b in (('R', 0x40), ('Z', 0x10), ('U', 0x20), ('A', 0x08)) if lo & b)
        print(f'{a:8.0f} px {100 * a / max(tot, 1):4.1f}%  n {n:4d}  op {op:02x} {cyc} {fl:4s} om {o:014x} cc {cm:014x} timg {ti:06x}')
finally:
    proc.kill()
