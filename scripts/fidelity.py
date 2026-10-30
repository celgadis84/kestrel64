#!/usr/bin/env python3
"""Fidelity score: community HW test suites, run the same way on kestrel64 and ares.

NOT a gate. It answers "how close to the console are we, and how does ares do on the
same ROMs", so it is run on demand, never after every change.

Suites (each ROM's own oracle, never a reference image):

    krom      PeterLemon ROMs that print PASS (green) / FAIL (red) per case and ship a
              HW capture (.png). A case = one green text box in the capture or in ours;
              it matches when its 8x8 cells have the capture's colour.
    dillon    Dillonb/n64-tests: r30 = -1 when every case passed, N = first failed case.
    systest   lemmy-64/n64-systemtest: "Failed X of Y tests" lines on the ISViewer.
    libdragon libdragon tests/testrom_emu.z64 (emulator build: no 5% wall-timing check;
              --libdragon-rom testrom.z64 for the HW build). ISViewer log:
              "**** Starting test" then ASSERTION FAILED / TIMING FAIL / MEMORY LEAK / SKIP.

Sets (--set): "core" (default) is the battery without overlap. n64-systemtest already
tests every CPU/COP0/COP1 opcode and exception case exhaustively, so Dillon's 26 opcode
ROMs and krom's CPUTest/{CPU,CP0,CP1,Exceptions} (91 ROMs) add run time, not coverage;
core keeps krom's RSP / RDP / RCP / DMA ROMs (HW captures nobody else has) plus systest and
libdragon. "full" runs everything. kestrel's krom ROMs run --jobs at a time on their own
ports; each ROM stops as soon as two captures in a row are identical and show results.

kestrel runs in its faithful configuration (KESTREL_SPLEAD=0: no CPU lead over the RSP
barrier; KESTREL_SPSIGQ=0: CPU->RSP signals at their exact instant; KESTREL_DPLOGLEAD=0),
the same one the gates use. --kmode default measures the shipped speed settings instead.

Both emulators are driven through their telemetry servers (kestrel: length-prefixed
JSON; ares fork: line JSON, one client per process). Every child is killed by PID.

usage:
    python scripts/fidelity.py [--emu kestrel|ares|both] [--suite krom,dillon,...]
                               [--filter SUBSTR] [--report out/fidelity.md]
"""

import romcfg  # configuracion local: scripts/local.env
import argparse
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROMS = romcfg.testroms()
KESTREL = Path(os.environ.get("KESTREL_EXE", ROOT / "build-prdp-static" / "kestrel64.exe"))
FAITHFUL = {"KESTREL_SPLEAD": "0", "KESTREL_SPSIGQ": "0", "KESTREL_DPLOGLEAD": "0"}
# RDP backend, fixed explicitly: the shipped exe starts on GPU-RDP, which is not complete yet.
RDP_ENV = {"prdp": {"KESTREL_PRDP": "1"}, "gpu": {"KESTREL_GPURDP": "1"}, "soft": {"KESTREL_PRDP": "0"}}
ARES = Path(romcfg.get("ARES_EXE", "ares.exe"))
OUT = ROOT / "out" / "fidelity"
KPORT = 9133
KPORTS = (9141, 9142, 9143, 9144)   # krom en paralelo
# Cubierto entero por n64-systemtest (opcodes CPU/COP0/COP1 y excepciones).
CORE_SKIP = ("CPUTest/CPU/", "CPUTest/CP0/", "CPUTest/CP1/", "CPUTest/Exceptions/")
APORT = 9123


# ------------------------------------------------------------------ emulator drivers

class Emu:
    """One emulator process with one ROM. stdout (ISViewer) goes to a file."""

    def __init__(self, kind, rom, log, port=None):
        self.kind, self.rom, self.log = kind, rom, log
        self.port = port or (KPORT if kind == "kestrel" else APORT)
        self.sock = None
        self.buf = b""
        self.id = 0
        env = dict(os.environ)
        if kind == "kestrel":
            env["KESTREL_AUDIO"] = "0"
            if ARGS.kmode == "faithful":
                env.update(FAITHFUL)
            env.pop("KESTREL_PRDP", None)          # an explicit KESTREL_PRDP turns GPU-RDP off
            env.update(RDP_ENV[ARGS.rdp])
            cmd = [str(KESTREL), str(rom), "--run", "--port", str(self.port)]
        else:
            cmd = [str(ARES), str(rom), "--system", "Nintendo 64", "--mcp"]
        self.fh = open(log, "wb")
        self.p = subprocess.Popen(cmd, env=env, stdout=self.fh, stderr=subprocess.STDOUT)

    def text(self):
        try:
            return self.log.read_bytes().decode("utf-8", "replace").replace("\0", "")
        except OSError:
            return ""

    def connect(self, timeout=20):
        port = self.port
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.p.poll() is not None:
                return False
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
                return True
            except OSError:
                time.sleep(0.3)
        return False

    def query(self, cmd, **args):
        """Returns (data, blob) or (None, b"") on any failure."""
        if not self.sock:
            return None, b""
        self.id += 1
        req = {"id": self.id, "cmd": cmd}
        if args:
            req["args"] = args
        try:
            if self.kind == "kestrel":
                j = json.dumps(req).encode()
                pay = struct.pack("<I", len(j)) + j
                self.sock.sendall(struct.pack("<I", len(pay)) + pay)
                (n,) = struct.unpack("<I", self._recv(4))
                pay = self._recv(n)
                (jl,) = struct.unpack("<I", pay[:4])
                resp = json.loads(pay[4:4 + jl].decode())
                blob = pay[4 + jl:]
            else:
                self.sock.sendall(json.dumps(req).encode() + b"\n")
                while b"\n" not in self.buf:
                    c = self.sock.recv(65536)
                    if not c:
                        return None, b""
                    self.buf += c
                line, self.buf = self.buf.split(b"\n", 1)
                resp, blob = json.loads(line.decode()), b""
        except (OSError, ValueError, struct.error):
            return None, b""
        return (resp.get("data") if resp.get("ok") else None), blob

    def _recv(self, n):
        b = bytearray()
        while len(b) < n:
            c = self.sock.recv(n - len(b))
            if not c:
                raise OSError("closed")
            b += c
        return bytes(b)

    def r30(self):
        d, _ = self.query("cpu.regs")
        if not d:
            return None
        v = d["gpr"]["fp"] if self.kind == "kestrel" else d["gpr"][30]
        v = int(v, 16) if isinstance(v, str) else int(v)
        v &= (1 << 64) - 1
        return v - (1 << 64) if v >> 63 else v

    def framebuffer(self, png, want_h=0):
        """VI framebuffer as a PIL RGB image (None if the VI is blanked).

        Both servers size the image from the VI's visible lines. krom's 640-wide
        screens are 480 lines of RDRAM shown by a 240-line VI, and the HW capture
        has all 480: with want_h the rest is read straight from RDRAM at the origin."""
        from PIL import Image
        if self.kind == "kestrel":
            d, blob = self.query("vi.capture")
            if not d or not blob:
                return None
            w, h, bpp = int(d["width"]), int(d["height"]), int(d.get("bpp", 32))
            img = Image.frombytes("RGBA", (w, h), blob).convert("RGB")
            origin = int(d.get("origin", 0))
        else:
            d, _ = self.query("video.framebuffer", path=str(png))
            if not d or not Path(png).exists():
                return None
            with Image.open(png) as im:
                img = im.convert("RGB")
            w, h = img.size
            bpp = 16 if int(d.get("colorDepth", 3)) == 2 else 32
            o = d.get("origin", 0)
            origin = (int(o, 16) if isinstance(o, str) else int(o)) & 0x7FFFFF
        if want_h <= h:
            return img
        raw = self.rdram(origin, w * want_h * bpp // 8)
        if raw is None:
            return img
        if bpp == 32:
            return Image.frombytes("RGBX", (w, want_h), raw).convert("RGB")
        px = bytearray()
        for i in range(0, len(raw), 2):
            p = raw[i] << 8 | raw[i + 1]
            for sh in (11, 6, 1):
                c = (p >> sh) & 31
                px.append(c << 3 | c >> 2)
        return Image.frombytes("RGB", (w, want_h), bytes(px))

    def rdram(self, addr, n):
        out = bytearray()
        while len(out) < n:
            k = min(65536, n - len(out))
            d, blob = self.query("mem.read", region="RDRAM", addr=addr + len(out), len=k)
            if not d:
                return None
            out += blob if self.kind == "kestrel" else bytes.fromhex(d["hex"])
        return bytes(out)

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        if self.p.poll() is None:
            self.p.kill()
        try:
            self.p.wait(10)
        except subprocess.TimeoutExpired:
            pass
        self.fh.close()


def wait_text(emu, done, secs):
    """Poll the ISViewer log until done(text) or secs elapse. Returns the text."""
    t0 = time.time()
    while time.time() - t0 < secs:
        txt = emu.text()
        if done(txt) or emu.p.poll() is not None:
            return txt
        time.sleep(0.5)
    return emu.text()


# ------------------------------------------------------------------ suites

def krom_roms(filt):
    """ROMs whose .asm prints PASS in FontGreen and that ship a HW capture (.png)."""
    base = ROMS / "PeterLemon-N64"
    out, seen = [], set()
    for asm in sorted(base.rglob("*.asm")):
        if "EMU" in asm.parts:
            continue
        try:
            if "FontGreen" not in asm.read_text(errors="replace"):
                continue
        except OSError:
            continue
        for rom in asm.parent.iterdir():
            if rom.suffix.lower() != ".n64" or rom.stem.lower() != asm.stem.lower():
                continue
            png = next((q for q in asm.parent.iterdir()
                        if q.suffix.lower() == ".png" and q.stem.lower() == rom.stem.lower()), None)
            name = rom.relative_to(base).with_suffix("").as_posix()
            if ARGS.set == "core" and name.startswith(CORE_SKIP):
                continue
            if png and name not in seen and (not filt or filt.lower() in name.lower()):
                seen.add(name)
                out.append((name, rom, png))
    return out


def cell_class(img):
    """8x8 cell -> 'g' (green font) / 'r' (red font) / absent."""
    w, h = img.size
    px = img.load()
    cls = {}
    for y in range(h):
        for x in range(w):
            R, G, B = px[x, y]
            if R > 180 and G < 70 and B < 70:
                cls[(x >> 3, y >> 3)] = "r"
            elif 90 < G < 170 and R < 60 and B < 60:
                cls.setdefault((x >> 3, y >> 3), "g")
    return cls


def krom_score(img, ref):
    """Result boxes = runs of green cells in the HW capture or in ours (PASS is the only
    thing krom draws in green). A box matches when every cell has the HW colour."""
    from PIL import Image
    if img.size != ref.size:
        img = img.resize(ref.size, Image.NEAREST)
    a, b = cell_class(img), cell_class(ref)
    cells = sorted({c for c, v in a.items() if v == "g"} | {c for c, v in b.items() if v == "g"},
                   key=lambda c: (c[1], c[0]))
    boxes, cur = [], []
    for c in cells:
        if cur and c[1] == cur[-1][1] and c[0] <= cur[-1][0] + 2:
            cur.append(c)
        else:
            if cur:
                boxes.append(cur)
            cur = [c]
    if cur:
        boxes.append(cur)
    # A box may land one 8-px row off (krom's text cursor vs the capture's crop): try the
    # whole box shifted by -1/0/+1 rows, never cell by cell.
    def fits(bx, dy):
        return all(a.get((x, y + dy)) == b.get((x, y)) for x, y in bx)
    ok = sum(1 for bx in boxes if any(fits(bx, dy) for dy in (0, -1, 1)))
    return ok, len(boxes)


def load_png(path):
    """Some krom captures carry ancillary chunks PIL rejects (zTXt/iCCP from GIMP): on
    failure keep only the critical chunks."""
    import io
    from PIL import Image
    try:
        with Image.open(path) as r:
            return r.convert("RGB")
    except Exception:
        data = path.read_bytes()
        out, i = [data[:8]], 8
        while i + 8 <= len(data):
            n = struct.unpack(">I", data[i:i + 4])[0]
            if data[i + 4:i + 8] in (b"IHDR", b"PLTE", b"IDAT", b"IEND"):
                out.append(data[i:i + 12 + n])
            i += 12 + n
        with Image.open(io.BytesIO(b"".join(out))) as r:
            return r.convert("RGB")


def krom_one(kind, name, rom, png, port, args):
    try:
        ref = load_png(png)
    except Exception:
        return (name, 0, 0, "BADREF")
    tag = f"{kind}-krom-{port}"
    e = Emu(kind, rom, OUT / f"{tag}.log", port)
    img = None
    if e.connect():
        # Sondeo: la ROM pinta sus casillas una vez y se queda quieta. Parar en cuanto dos
        # capturas seguidas son iguales y ya hay alguna casilla verde o roja; tope krom_secs.
        t0, prev = time.time(), None
        while time.time() - t0 < args.krom_secs:
            time.sleep(0.4)
            cur = e.framebuffer(OUT / f"{tag}.png", ref.size[1])
            if cur is None:
                continue
            img = cur
            raw = cur.tobytes()
            if raw == prev and cell_class(cur):
                break
            prev = raw
    e.close()
    if img is None:
        return (name, 0, 0, "NOFB")
    ok, tot = krom_score(img, ref)
    return (name, ok, tot, "PASS" if tot and ok == tot else ("NORESULT" if not tot else "FAIL"))


def suite_krom(kind, args):
    from concurrent.futures import ThreadPoolExecutor
    import queue
    roms = krom_roms(args.filter)
    jobs = max(1, min(args.jobs, len(KPORTS))) if kind == "kestrel" else 1   # ares: un puerto
    ports = queue.Queue()
    for p in (KPORTS[:jobs] if kind == "kestrel" else (APORT,)):
        ports.put(p)

    def run(item):
        port = ports.get()
        try:
            row = krom_one(kind, *item, port, args)
        finally:
            ports.put(port)
        progress(kind, "krom", row[0], row[3])
        return row
    with ThreadPoolExecutor(jobs) as ex:
        return list(ex.map(run, roms))


def suite_dillon(kind, args):
    rows = []
    if args.set == "core":
        return rows
    for rom in sorted((ROMS / "dillon-n64-tests").glob("*.z64")):
        if args.filter and args.filter.lower() not in rom.stem.lower():
            continue
        e = Emu(kind, rom, OUT / f"{kind}-dillon.log")
        v = None
        if e.connect():
            t0 = time.time()
            while time.time() - t0 < args.dillon_secs:
                v = e.r30()
                if v:
                    break
                time.sleep(0.3)
        e.close()
        verdict = "PASS" if v == -1 else (f"FAIL case {v}" if v else "NORESULT")
        rows.append((rom.stem, verdict))
        progress(kind, "dillon", rom.stem, verdict)
    return rows


ST_RE = re.compile(r"(Base|Timing|Cycle):\s+Failed\s+(\d+)\s+of\s+(\d+)\s+tests")


def suite_systest(kind, args):
    rom = ROMS / "n64-systemtest.z64"
    log = OUT / f"{kind}-systest.log"
    e = Emu(kind, rom, log)
    txt = wait_text(e, lambda t: len(ST_RE.findall(t)) >= 3, args.systest_secs)
    e.close()
    hits = ST_RE.findall(txt)
    return [(k, int(f), int(t)) for k, f, t in hits]


def suite_libdragon(kind, args):
    rom = ROMS / "libdragon-tests" / args.libdragon_rom
    log = OUT / f"{kind}-libdragon.log"
    e = Emu(kind, rom, log)
    txt = wait_text(e, lambda t: "Passed:" in t or "Testsuite finished" in t, args.libdragon_secs)
    e.close()
    rows, cur, res = [], None, None
    for line in txt.splitlines():
        m = re.match(r"\*\*\*\* Starting test: (\S+)", line.strip())
        if m:
            if cur:
                rows.append((cur, res or "PASS"))
            cur, res = m.group(1), None
            continue
        if cur and res is None:
            s = line.strip()
            if s.startswith("ASSERTION FAILED") or s.startswith("TEST FAILED"):
                res = "FAIL"
            elif s.startswith("TIMING FAIL"):
                res = "TIMING"
            elif s.startswith("MEMORY LEAK"):
                res = "LEAK"
            elif s == "SKIP" or s.startswith("TEST SKIPPED"):
                res = "SKIP"
    if cur:
        # the last test only counts if the suite got past it
        finished = "Passed:" in txt or "Testsuite finished" in txt
        rows.append((cur, (res or "PASS") if finished else "HANG"))
    return rows


def progress(kind, suite, name, verdict):
    if not ARGS.quiet:
        sys.stderr.write(f"\r[{kind}:{suite}] {name[:50]:<50} {verdict:<14}")
        sys.stderr.flush()


SUITES = {"krom": suite_krom, "dillon": suite_dillon, "systest": suite_systest,
          "libdragon": suite_libdragon}


# ------------------------------------------------------------------ scoring / report

def score(suite, rows):
    """(passed, total, detail-lines-of-failures)."""
    if suite == "krom":
        rows = [r for r in rows if r[3] != "BADREF"]
        ok = sum(1 for r in rows if r[3] == "PASS")
        cases_ok, cases = sum(r[1] for r in rows), sum(r[2] for r in rows)
        bad = [f"{n}: {v} ({g}/{t} casos como HW)" for n, g, t, v in rows if v != "PASS"]
        return ok, len(rows), bad, f"casos {cases_ok}/{cases}"
    if suite == "dillon":
        ok = sum(1 for r in rows if r[1] == "PASS")
        return ok, len(rows), [f"{n}: {v}" for n, v in rows if v != "PASS"], ""
    if suite == "systest":
        if not rows:
            return 0, 1, ["no summary"], ""
        tot = sum(t for _, _, t in rows)
        fail = sum(f for _, f, _ in rows)
        return tot - fail, tot, [f"{k}: failed {f} of {t}" for k, f, t in rows if f], \
            "  ".join(f"{k} {t-f}/{t}" for k, f, t in rows)
    if suite == "libdragon":
        run = [r for r in rows if r[1] != "SKIP"]
        ok = sum(1 for r in run if r[1] == "PASS")
        timing = sum(1 for r in run if r[1] == "TIMING")
        func_ok = sum(1 for r in run if r[1] in ("PASS", "TIMING"))
        return ok, len(run), [f"{n}: {v}" for n, v in run if v != "PASS"], \
            f"funcional {func_ok}/{len(run)}, timing-fail {timing}, skip {len(rows)-len(run)}"
    return 0, 0, [], ""


def main():
    global ARGS
    ap = argparse.ArgumentParser(description="fidelity score (not a gate)")
    ap.add_argument("--emu", default="both", choices=["kestrel", "ares", "both"])
    ap.add_argument("--suite", default="krom,dillon,systest,libdragon")
    ap.add_argument("--filter")
    ap.add_argument("--set", default="core", choices=["core", "full"],
                    help="core: sin lo que systemtest ya cubre; full: todo")
    ap.add_argument("--jobs", type=int, default=4, help="ROMs de krom a la vez en kestrel")
    ap.add_argument("--krom-secs", type=float, default=8.0, help="tope por ROM de krom")
    ap.add_argument("--dillon-secs", type=float, default=10.0)
    ap.add_argument("--systest-secs", type=float, default=300.0)
    ap.add_argument("--libdragon-secs", type=float, default=900.0)
    ap.add_argument("--libdragon-rom", default="testrom_emu.z64")
    ap.add_argument("--kmode", default="faithful", choices=["faithful", "default"],
                    help="kestrel: faithful (gate settings) or default (shipped speed settings)")
    ap.add_argument("--rdp", default="gpu", choices=sorted(RDP_ENV),
                    help="kestrel RDP backend: gpu (GPU-RDP, default), soft, prdp (parallel-rdp, being retired)")
    ap.add_argument("--report", default=str(OUT / "fidelity.md"))
    ap.add_argument("--quiet", action="store_true")
    ARGS = args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    emus = ["kestrel", "ares"] if args.emu == "both" else [args.emu]
    suites = [s.strip() for s in args.suite.split(",") if s.strip()]
    if args.set == "core":
        suites = [s for s in suites if s != "dillon"]
    res = {}
    for s in suites:
        for k in emus:
            t0 = time.time()
            rows = SUITES[s](k, args)
            res[(s, k)] = (score(s, rows), time.time() - t0)
            if not args.quiet:
                sys.stderr.write("\n")
            (OUT / f"{k}-{s}.json").write_text(json.dumps(rows, indent=1))

    lines = ["# Fidelity score", "", f"kestrel: {KESTREL} ({args.kmode}, rdp {args.rdp}); set {args.set}; libdragon ROM: {args.libdragon_rom}", "", "| suite | " + " | ".join(emus) + " |",
             "|---|" + "---|" * len(emus)]
    for s in suites:
        cells = []
        for k in emus:
            (ok, tot, _, extra), secs = res[(s, k)]
            pct = 100.0 * ok / tot if tot else 0.0
            cells.append(f"{ok}/{tot} ({pct:.1f}%){' · ' + extra if extra else ''} · {secs:.0f}s")
        lines.append(f"| {s} | " + " | ".join(cells) + " |")
    lines.append("")
    for s in suites:
        for k in emus:
            bad = res[(s, k)][0][2]
            if bad:
                lines.append(f"## {s} — {k}: {len(bad)} fallos")
                lines += [f"- {b}" for b in bad]
                lines.append("")
    text = "\n".join(lines)
    Path(args.report).write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
