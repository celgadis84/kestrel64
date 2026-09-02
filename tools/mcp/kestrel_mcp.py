#!/usr/bin/env python3
"""kestrel64 MCP bridge.

Speaks kestrel64's telemetry protocol: length-framed JSON(+binary) over TCP.

Frame:  [u32 LE totalLen][totalLen bytes payload]
payload = [u32 LE jsonLen][json][blob]

The JSON carries structure; bulk data (memory, framebuffers) rides as a raw binary
blob with zero hex/base64 overhead on the wire. This module hands the agent hex only
at the tool boundary.

Run standalone for a quick smoke test:  python kestrel_mcp.py --selftest
As an MCP server (needs `mcp`):          python kestrel_mcp.py
"""

import json
import socket
import struct
import threading
import argparse

HOST = "127.0.0.1"
PORT = 9128

_lock = threading.Lock()
_sock = None
_id = 0


class KestrelError(RuntimeError):
    pass


def _connect():
    global _sock
    if _sock is not None:
        return
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # run_until can block server-side for up to its timeout_ms; keep the socket
    # read timeout comfortably above that so a legitimately long wait isn't torn down.
    s.settimeout(30)
    _sock = s


def _drop():
    global _sock
    if _sock is not None:
        try:
            _sock.close()
        except OSError:
            pass
    _sock = None


def _recv_exact(n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = _sock.recv(n - len(buf))
        if not chunk:
            raise OSError("connection closed by kestrel64")
        buf += chunk
    return bytes(buf)


def query(cmd: str, blob: bytes = b"", **args):
    """Send one request, return (data_dict, blob_bytes)."""
    global _id
    with _lock:
        _id += 1
        req = {"id": _id, "cmd": cmd}
        if args:
            req["args"] = args
        payload_json = json.dumps(req).encode()
        payload = struct.pack("<I", len(payload_json)) + payload_json + blob
        frame = struct.pack("<I", len(payload)) + payload
        try:
            _connect()
            _sock.sendall(frame)
            (total,) = struct.unpack("<I", _recv_exact(4))
            payload = _recv_exact(total)
        except OSError as e:
            _drop()
            raise KestrelError(f"telemetry lost ({e}); is kestrel64 running on {HOST}:{PORT}?")
    (jlen,) = struct.unpack("<I", payload[:4])
    resp = json.loads(payload[4:4 + jlen].decode())
    rblob = payload[4 + jlen:]
    if not resp.get("ok", False):
        raise KestrelError(resp.get("error", "unknown error"))
    return resp.get("data", {}), rblob


def _addr(v) -> int:
    return v if isinstance(v, int) else int(str(v), 0)


# --- tool implementations (usable directly or wrapped by MCP) ----------------

def emu_status() -> dict:
    data, _ = query("status")
    return data


def memory_regions() -> dict:
    data, _ = query("mem.regions")
    return data


def read_memory(region: str, addr="0", length: int = 64, coherent: bool = False) -> dict:
    """Read raw bytes from a memory region as hex.

    coherent=True (RDRAM only) reads through the CPU's write-back D-cache, so kernel
    structures the CPU wrote but has not yet flushed to RAM are seen correctly. The
    plain (default) read returns the RDRAM backing store, which is stale for any
    dirty cache line — use coherent=True when inspecting libultra kernel state
    (thread structs, message-queue validCount, scheduler queues) of a running game."""
    data, blob = query("mem.read", region=region, addr=_addr(addr), len=int(length),
                       coherent=1 if coherent else 0)
    data["hex"] = blob.hex()
    return data


def write_memory(region: str, addr, hexbytes: str) -> dict:
    data, _ = query("mem.write", region=region, addr=_addr(addr), data=hexbytes)
    return data


def cpu_registers() -> dict:
    """Full CPU state: pc, gpr (by name), hi/lo, COP0, and the disasm at pc."""
    data, _ = query("cpu.regs")
    return data


def cpu_step(count: int = 1) -> dict:
    """Single-step (or step `count`) instructions. Returns new pc + disasm."""
    data, _ = query("cpu.step", count=int(count))
    return data


def cpu_disasm(addr, count: int = 16) -> dict:
    """Disassemble `count` instructions starting at virtual `addr`."""
    data, _ = query("cpu.disasm", addr=_addr(addr), count=int(count))
    return data


def run_control(action: str) -> dict:
    """action = 'pause' | 'resume' | 'reset'."""
    if action not in ("pause", "resume", "reset"):
        raise KestrelError("action must be pause|resume|reset")
    data, _ = query(action)
    return data


def rcp_registers() -> dict:
    """Full RCP MMIO register file: MI (mode/mask/intr/intrPending), SP, DPC, VI,
    AI, PI, SI. This is the state the boot code and scheduler poll — MI mask/intr
    drive CPU IP2, VI current/intr drive the retrace interrupt, SP status the RSP."""
    data, _ = query("rcp.regs")
    return data


def controller_set(buttons="", stick_x: int = 0, stick_y: int = 0, polls: int = 6) -> dict:
    """Press the player-1 controller from here, overriding the host keyboard/gamepad.

    `buttons` is either a 16-bit word (A=0x8000 ... C-Right=0x0001, START=0x1000) or a
    name list: "start", "a,b", "z+cup". Names: a b z start dup ddown dleft dright l r
    cup cdown cleft cright. `stick_x`/`stick_y` are the N64's own -80..80 analog range.

    `polls` is how long the press lasts, counted in joybus reads of controller 1 — not
    milliseconds. That is the only clock the game itself sees: a game that reads the pad
    once per frame gets `polls` frames of press whether the emulator runs at 30% or 200%
    of real time, and when the count runs out the host pad comes back, so the game sees a
    real release edge (what menus wait for). polls=-1 holds until the next call, polls=0
    releases immediately.

    Needed because the window loop republishes the pad every frame: writing the button
    word with write_memory is overwritten before the game ever polls it."""
    data, _ = query("pad.set", buttons=buttons, stick_x=int(stick_x),
                    stick_y=int(stick_y), polls=int(polls))
    return data


def controller_state() -> dict:
    """Effective player-1 pad state: whether the injected pad is driving it, how many
    joybus polls it has left, and the button word / stick the game will read next."""
    data, _ = query("pad.get")
    return data


def capture_framebuffer(path: str = "kestrel_fb.png", height: int = 240) -> dict:
    """Grab the live VI framebuffer and write it as a PNG to `path`.

    Decodes the exact buffer the VI is scanning out (origin/width/format read from
    the RCP registers), expanding 16bpp 5551 or 32bpp 8888 to RGBA. Returns the
    geometry plus the saved path and a small pixel histogram so a blank vs. rendered
    frame is obvious without opening the image. This is kestrel's own capture path —
    no host GPU readback, just the RDRAM the console would display."""
    data, blob = query("vi.capture", height=int(height))
    w, h = data["width"], data["height"]
    _write_png(path, w, h, blob)
    data["path"] = path
    # cheap distinct-pixel count so the agent can tell rendered from solid-fill
    seen = set()
    for i in range(0, len(blob), 4):
        seen.add(blob[i:i+4])
        if len(seen) > 4096:
            break
    data["distinctColors"] = len(seen)
    return data


def _write_png(path: str, w: int, h: int, rgba: bytes):
    """Minimal zlib-backed PNG writer (RGBA8, no external deps)."""
    import struct as _s, zlib, binascii
    def chunk(tag, payload):
        c = tag + payload
        return _s.pack(">I", len(payload)) + c + _s.pack(">I", binascii.crc32(c) & 0xffffffff)
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)  # filter: none
        raw += rgba[y * stride:(y + 1) * stride]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", _s.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def rsp_registers(vpr: bool = False) -> dict:
    """Low-level RSP core: pc (into IMEM), running flag, sp_status/sp_pc, 32 scalar
    GPRs, and (when vpr=True) the 32 vector registers as 8×u16 lanes each."""
    data, _ = query("rsp.regs", vpr=bool(vpr))
    return data


def breakpoint_add(addr) -> dict:
    """Set a PC breakpoint (virtual addr). The run loop auto-pauses when the CPU is
    about to execute it. Zero overhead while no breakpoints are set."""
    data, _ = query("cpu.bp.add", addr=_addr(addr))
    return data


def breakpoint_del(addr=None) -> dict:
    """Remove a PC breakpoint. Omit addr to clear all breakpoints."""
    if addr is None:
        data, _ = query("cpu.bp.del")
    else:
        data, _ = query("cpu.bp.del", addr=_addr(addr))
    return data


def breakpoint_list() -> dict:
    """List active PC breakpoints and the PC of the last breakpoint that fired."""
    data, _ = query("cpu.bp.list")
    return data


def run_until(addr, timeout_ms: int = 5000) -> dict:
    """Resume and block until PC reaches `addr` (or CPU halts / timeout). Uses a
    temporary breakpoint, leaves the core paused at the stop point. Returns
    {hit, timedOut, pc, halted, disasm}."""
    data, _ = query("cpu.run_until", addr=_addr(addr), timeout_ms=int(timeout_ms))
    return data


def profile_start() -> dict:
    """Start the hotpath profiler: enable + clear both samplers (CPU physical-PC,
    RSP per-IMEM-instruction). Run the workload, then call profile_cpu/profile_rsp
    to see where the emulated time goes. Returns {profiling}."""
    data, _ = query("prof.start")
    return data


def profile_stop() -> dict:
    """Stop the hotpath profiler (disable both samplers, keep counts for querying)."""
    data, _ = query("prof.stop")
    return data


def profile_reset() -> dict:
    """Zero both samplers' counts without changing enabled state (fresh window)."""
    data, _ = query("prof.reset")
    return data


def _snapshot(cmd: str, **args) -> dict:
    """Read a profiler snapshot without starving on coreMutex. The free-run loop
    holds coreMutex in ~one-video-field batches, so a read-only query issued mid-run
    can wait a long time for the tiny inter-batch window. Pausing first yields the
    lock immediately and gives a torn-free snapshot; we restore the prior run state
    after. If the core was already paused we leave it paused."""
    was_running = not bool(query("status")[0].get("paused", False))
    if was_running:
        query("pause")
    try:
        data, _ = query(cmd, **args)
    finally:
        if was_running:
            query("resume")
    return data


def profile_cpu(top: int = 20) -> dict:
    """Top CPU hotpath buckets by physical address (16-byte / 4-instruction
    resolution, so KSEG0 and TLB-mapped code that alias the same RDRAM merge).
    Each entry: {phys, kseg0, count, pct, disasm}. Also {total, resolutionBytes}.
    Disasm shows the first instruction of the bucket — cross-reference with
    cpu_disasm(kseg0) to see the whole hot routine. Momentarily pauses the core to
    take a clean snapshot, then restores the prior run state."""
    return _snapshot("prof.cpu", top=int(top))


def profile_rsp(top: int = 20) -> dict:
    """Top RSP microcode hotpath by IMEM instruction address (exact per-instruction).
    Each entry: {imem, count, pct}. Also {total}. Tells which microcode routine
    dominates RSP time (graphics vs audio vs custom). Momentarily pauses the core to
    take a clean snapshot, then restores the prior run state."""
    return _snapshot("prof.rsp", top=int(top))


def _selftest():
    print("status     :", json.dumps(emu_status(), indent=2)[:600])
    regs = memory_regions()
    print("regions    :", [(r["name"], hex(r["base"]), r["size"]) for r in regs["regions"]])
    # Read the ROM header magic + internal name.
    hdr = read_memory("CART_ROM", 0, 64)
    magic = hdr["hex"][:8]
    name = bytes.fromhex(hdr["hex"][0x40:0x40 + 0x28]).decode("latin1").rstrip()
    print(f"rom magic  : {magic}  (expect 80371240)")
    print(f"rom name   : {name!r}")
    # Round-trip a write into RDRAM and read it back.
    write_memory("RDRAM", 0x1000, "deadbeef")
    rb = read_memory("RDRAM", 0x1000, 4)["hex"]
    print(f"rdram rw   : wrote deadbeef, read {rb}  ({'OK' if rb=='deadbeef' else 'FAIL'})")
    # New debug ops.
    rcp = rcp_registers()
    print("rcp.mi     :", rcp.get("mi"))
    print("rcp.vi     :", {k: hex(v) for k, v in rcp.get("vi", {}).items()})
    print("rcp.sp     :", rcp.get("sp"))
    rsp = rsp_registers()
    print(f"rsp        : pc={rsp.get('pc')} running={rsp.get('running')} sp_status={hex(rsp.get('sp_status',0))}")
    pc = cpu_registers()["pc"]
    print(f"cpu.pc     : {hex(pc)}")
    breakpoint_del()  # clear
    breakpoint_add(pc)
    print("bp.list    :", breakpoint_list())
    breakpoint_del(pc)
    print("bp.cleared :", breakpoint_list()["breakpoints"])
    print("selftest done.")


def _serve_mcp():
    from mcp.server.fastmcp import FastMCP
    mcp = FastMCP("kestrel64")
    mcp.tool()(emu_status)
    mcp.tool()(memory_regions)
    mcp.tool()(read_memory)
    mcp.tool()(write_memory)
    mcp.tool()(cpu_registers)
    mcp.tool()(cpu_step)
    mcp.tool()(cpu_disasm)
    mcp.tool()(run_control)
    mcp.tool()(rcp_registers)
    mcp.tool()(rsp_registers)
    mcp.tool()(controller_set)
    mcp.tool()(controller_state)
    mcp.tool()(capture_framebuffer)
    mcp.tool()(breakpoint_add)
    mcp.tool()(breakpoint_del)
    mcp.tool()(breakpoint_list)
    mcp.tool()(run_until)
    mcp.tool()(profile_start)
    mcp.tool()(profile_stop)
    mcp.tool()(profile_reset)
    mcp.tool()(profile_cpu)
    mcp.tool()(profile_rsp)
    mcp.run()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()
    PORT = args.port
    if args.selftest:
        _selftest()
    else:
        _serve_mcp()
