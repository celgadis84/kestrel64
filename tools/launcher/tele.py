"""Cliente del servidor de telemetria de kestrel64 para el lanzador.

Mismo protocolo que `tools/mcp/kestrel_mcp.py` (marcos con longitud por delante: JSON y,
detras, bytes crudos), pero suelto: el lanzador no depende de FastMCP ni del puente. La
conexion se abre perezosa y se reabre sola, porque el emulador va y viene entre arranques.
"""

import json
import socket
import struct
import threading

HOST = "127.0.0.1"


class TeleError(RuntimeError):
    pass


class Tele:
    def __init__(self, port=9128):
        self.port = port
        self.sock = None
        self.lock = threading.Lock()
        self.id = 0

    def close(self):
        with self.lock:
            self._drop()

    def _drop(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def _recv(self, n):
        buf = bytearray()
        while len(buf) < n:
            c = self.sock.recv(n - len(buf))
            if not c:
                raise OSError("el emulador cerro la conexion")
            buf += c
        return bytes(buf)

    def query(self, cmd, **args):
        """Devuelve (data, blob). Lanza TeleError si no hay emulador o si el falla."""
        with self.lock:
            self.id += 1
            req = {"id": self.id, "cmd": cmd}
            if args:
                req["args"] = args
            j = json.dumps(req).encode()
            payload = struct.pack("<I", len(j)) + j
            frame = struct.pack("<I", len(payload)) + payload
            try:
                if self.sock is None:
                    s = socket.create_connection((HOST, self.port), timeout=3)
                    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    s.settimeout(15)
                    self.sock = s
                self.sock.sendall(frame)
                (total,) = struct.unpack("<I", self._recv(4))
                payload = self._recv(total)
            except OSError as e:
                self._drop()
                raise TeleError("sin telemetria en %s:%d (%s)" % (HOST, self.port, e))
        (jlen,) = struct.unpack("<I", payload[:4])
        resp = json.loads(payload[4:4 + jlen].decode())
        blob = payload[4 + jlen:]
        if not resp.get("ok", False):
            raise TeleError(resp.get("error", "error desconocido"))
        return resp.get("data", {}), blob

    def snapshot(self, cmd, **args):
        """Lectura de perfilador sin morir esperando el candado del nucleo. El bucle de
        marcha libre lo tiene cogido en tandas de ~un campo de video, asi que una consulta
        de solo lectura lanzada a media marcha puede esperar mucho a la ventana entre
        tandas. Pausar primero suelta el candado ya y da una foto sin desgarros; despues se
        restaura el estado anterior. Si ya estaba pausado, se queda pausado."""
        running = not bool(self.query("status")[0].get("paused", False))
        if running:
            self.query("pause")
        try:
            return self.query(cmd, **args)[0]
        finally:
            if running:
                self.query("resume")


def png(w, h, rgba):
    """PNG RGBA8 minimo, solo con zlib de la biblioteca estandar."""
    import binascii
    import zlib

    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + \
            struct.pack(">I", binascii.crc32(c) & 0xffffffff)

    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)                       # filtro: ninguno
        raw += rgba[y * stride:(y + 1) * stride]
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    out += chunk(b"IEND", b"")
    return out
