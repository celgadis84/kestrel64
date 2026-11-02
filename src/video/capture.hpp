#pragma once
// Capture -- screenshots (PNG) and video recording (MP4 / AVI) in GUEST time.
//
// Responsibility: grab the framebuffer the VI scans out of RDRAM when a field closes (not the
// window), and write it as PNG or as one video frame per emulated field together with the audio
// the AI emitted in that same guest time, so video and audio stay in sync at any emulation speed.
//
// Invariants:
// - Requests may arrive at any time, but they are served by the core hooks onField()/onIdle():
//   a screenshot is written at the next field close (or at once while paused).
// - Video frames are never dropped: if the encoder falls behind, the core waits.
// - stopVideo()/shutdown() must be called so the file is closed and valid.
//
// Threads: requestScreenshot/startVideo/stopVideo/toggleVideo/videoStats may be called from any
// thread (window keys, menu, telemetry, KESTREL_RECORD); state is guarded by g_recMu/g_shotMu
// and atomics. onField/onIdle/onAudio run on the core (CPU) thread. Video encoding runs on its
// own thread fed through a queue.
// kestrel64 -- captura de pantalla (PNG) y de video (AVI: MJPEG + PCM) en tiempo de INVITADO.
//
// Las dos salen del framebuffer que barre el VI, leido de la RDRAM al cerrar el campo (el
// mismo camino que KESTREL_FBDUMP / KESTREL_FBSEQ), no de la ventana: la foto no depende de
// la escala, del filtro ni de si hay ventana, y el video lleva un cuadro por campo emulado y
// el audio que el AI emitio en ese mismo tiempo, asi que casan aunque el emulador vaya a
// 0,5x o a 3x del tiempo real.
//
// Las peticiones llegan de cualquier hilo (teclas F9/F8 y menu de la ventana, telemetria,
// KESTREL_RECORD) y se atienden en el hilo del nucleo: al cerrar cada campo y, con la
// emulacion en pausa, en la vuelta de espera del bucle (una foto en pausa sale al momento).
// El video se codifica en un hilo propio; si no da abasto, el nucleo espera (nunca se tira un
// cuadro: perderlo desincronizaria el audio).

#include "../core/types.hpp"
#include <string>
#include <vector>

namespace kestrel {

struct Memory;

// Framebuffer que barre el VI en RGB24 de arriba abajo (lo implementa fbdump.cpp junto al volcado
// BMP de KESTREL_FBDUMP, que usa la MISMA geometria). Con el VI apagado sale negro del tamano
// que tocaria. Devuelve false solo sin memoria o sin geometria utilizable.
struct FbGrab {
  u32 w = 0, h = 0;
  u32 type = 0;          // VI_CTRL & 3: 2 = 16 bpp, 3 = 32 bpp, 0/1 = apagado
  u32 origin = 0;
  std::vector<u8> rgb;   // w*h*3
};
auto grabFramebuffer(Memory* mem, FbGrab& out) -> bool;
// KESTREL_FBDUMP: the framebuffer the VI scans out, as a 24-bit BMP at `path`.
auto dumpFramebufferBmp(Memory* mem, const char* path) -> void;
// KESTREL_FBSEQ: per-field (or guest-requested) BMP sequence; called when a VI field closes.
auto fbSeqField(Memory* mem, u32 field) -> void;

namespace capture {

// Nombre de la ROM para los ficheros por defecto (<rom>_<fecha>_<hora>.png/.avi).
auto setRomName(const std::string& name) -> void;

// --- foto -------------------------------------------------------------------------------
// Pide una foto. path vacio = carpeta de capturas por defecto. Devuelve un numero de peticion.
auto requestScreenshot(const std::string& path = {}) -> u64;
// Espera a que la peticion `id` quede atendida (como mucho timeoutMs). true = escrita; en
// `path`/`w`/`h` lo que salio, en `err` el motivo si no.
auto waitScreenshot(u64 id, u32 timeoutMs, std::string& path, u32& w, u32& h, std::string& err) -> bool;

// --- video ------------------------------------------------------------------------------
struct VideoOpts {
  std::string path;      // vacio = carpeta de videos por defecto
  u32 every = 1;         // un cuadro de cada N campos (2 = 30 fps en NTSC)
  u32 quality = 90;      // calidad JPEG 1..100
  u32 scale = 1;         // ampliacion entera (vecino mas proximo) 1..4
  bool audio = true;     // pista de audio PCM con lo que emite el AI
  // "mp4" (H.264 + AAC 48 kHz por Media Foundation, solo Windows) o "avi" (MJPEG + PCM,
  // sin perdidas en el audio). Vacio = por la extension de path; sin path, mp4.
  std::string format;
};
// Arma la grabacion: empieza en el siguiente cierre de campo. false si ya hay una en marcha.
auto startVideo(const VideoOpts& o, std::string& err) -> bool;
struct VideoStats {
  bool recording = false;
  std::string path;      // primer fichero (los trozos siguientes llevan _002, _003...)
  std::string format;    // "mp4" / "avi"
  u32 width = 0, height = 0;
  u64 frames = 0;        // cuadros escritos
  u64 audioFrames = 0;   // muestras estereo escritas
  u32 audioRate = 0;
  double fps = 0;
  double seconds = 0;    // duracion en tiempo de invitado
  u64 bytes = 0;         // todos los trozos
  u32 segments = 0;
  u32 queued = 0;        // cuadros esperando al codificador
};
// Para y cierra el fichero (espera a que el codificador vacie la cola). Devuelve lo grabado.
auto stopVideo() -> VideoStats;
auto videoStats() -> VideoStats;
auto recording() -> bool;
// Tecla/menu: si graba, para; si no, empieza con las opciones por defecto.
auto toggleVideo() -> void;

// --- ganchos del nucleo (hilo del nucleo) ------------------------------------------------
// Al cerrar un campo de video. Atiende fotos pendientes y mete el cuadro del video.
auto onField(Memory* mem) -> void;
// Con la emulacion parada (pausa, CPU detenida): solo atiende fotos pendientes.
auto onIdle(Memory* mem) -> void;
// Cada buffer que el AI entrega (s16 estereo big-endian en RDRAM), como audio::pushRdram.
auto onAudio(const u8* base, u32 size, u32 addr, u32 len, u32 sampleRate) -> void;
// Al salir: cierra la grabacion que quede abierta para que el fichero sea valido.
auto shutdown() -> void;

// --- codificadores sueltos (los usa tambien la telemetria) --------------------------------
auto encodePng(const u8* rgb, u32 w, u32 h, std::vector<u8>& out) -> void;
auto encodeJpeg(const u8* rgb, u32 w, u32 h, u32 quality, std::vector<u8>& out) -> void;
auto writeFile(const std::string& path, const std::vector<u8>& data) -> bool;

}  // namespace capture
}  // namespace kestrel
