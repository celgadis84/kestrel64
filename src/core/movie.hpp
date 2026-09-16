#pragma once
// Grabacion y reproduccion de entradas ("peliculas", herramientas TAS).
//
// Lo que se graba NO es lo que el jugador aprieta sino lo que el JUEGO LEE: cada respuesta
// al comando 0x01 del joybus (leer botones) de cada conector. Esa es la unica frontera que
// el invitado percibe, y es la que hay que reproducir para que una repeticion sea la misma
// partida. Grabar por campo de video seria mentira: hay juegos que sondean el mando dos
// veces en un campo, otros que se saltan campos enteros, y el orden de los cuatro
// conectores lo decide el bloque de comandos que el juego escribe en la PIF RAM.
//
// Formato (.k64m), pensado para que un fichero se pueda leer con `xxd` y no dependa del
// relleno del compilador: cabecera de 64 bytes, todo entero en little-endian, y detras la
// lista plana de muestras de 5 bytes.
//
//   0x00  8   "K64MOVIE"
//   0x08  4   version (1)
//   0x0C  4   banderas (bit0: la pelicula empieza en un arranque en frio)
//   0x10  4   crc1 de la cabecera del cartucho (0x10)
//   0x14  4   crc2 de la cabecera del cartucho (0x14)
//   0x18  1   norma de television (0 PAL / 1 NTSC / 2 MPAL)
//   0x19  1   RDRAM en MB (4 u 8)
//   0x1A  1   mascara de conectores enchufados al grabar
//   0x1B  1   reservado
//   0x1C  8   numero de muestras (se rellena al cerrar; 0 = pelicula truncada)
//   0x24  20  nombre interno del cartucho
//   0x38  8   reservado
//
//   muestra: u8 conector · u8 botones alto · u8 botones bajo · s8 eje X · s8 eje Y
//
// Reproducir una pelicula de otro cartucho no es un error recuperable: se avisa con los dos
// CRC y no se reproduce nada, porque inyectar botones de otra partida solo produce basura
// que parece un fallo del emulador.
//
// Apagado no cuesta mas que comparar un enum contra cero en cada lectura de botones.

#include "types.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace kestrel::movie {

enum class Mode { Off, Rec, Play };

inline Mode  mode   = Mode::Off;
inline FILE* fp     = nullptr;
// Los lee la telemetria desde su hilo mientras el hilo de CPU los mueve (emu.status
// publica por donde va la cinta): atomicos relajados, que aqui cuestan lo mismo que un
// entero -- se tocan una vez por lectura del mando, o sea unas sesenta veces por segundo.
inline std::atomic<u64> polls{0};   // muestras grabadas o consumidas
inline std::atomic<bool> ended{false};  // la pelicula se acabo y manda otra vez el anfitrion
inline u64   total  = 0;        // muestras que dice la cabecera (solo al reproducir)
inline bool  warnedChan = false;
inline std::string path;

inline auto put32(u8* p, u32 v) -> void { p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24); }
inline auto get32(const u8* p) -> u32 { return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }
inline auto put64(u8* p, u64 v) -> void { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }
inline auto get64(const u8* p) -> u64 { return (u64)get32(p) | ((u64)get32(p + 4) << 32); }

constexpr u32 kHdrSize = 64;
constexpr u32 kOffPolls = 0x1C;

// Se llama una vez, con el cartucho ya cargado y la RDRAM ya dimensionada.
inline auto init(u32 crc1, u32 crc2, const std::string& name, u8 tv, u8 ramMb, u8 ports) -> void {
  const char* rec  = std::getenv("KESTREL_MOVIE_REC");
  const char* play = std::getenv("KESTREL_MOVIE_PLAY");
  if(rec && play) {
    std::fprintf(stderr, "[movie] KESTREL_MOVIE_REC y KESTREL_MOVIE_PLAY a la vez: se reproduce\n");
    rec = nullptr;
  }
  if(!rec && !play) return;

  if(play) {
    fp = std::fopen(play, "rb");
    if(!fp) { std::fprintf(stderr, "[movie] no se puede abrir %s para reproducir\n", play); return; }
    u8 h[kHdrSize] = {};
    if(std::fread(h, 1, kHdrSize, fp) != kHdrSize || std::memcmp(h, "K64MOVIE", 8) != 0) {
      std::fprintf(stderr, "[movie] %s no es una pelicula de kestrel64\n", play);
      std::fclose(fp); fp = nullptr; return;
    }
    u32 ver = get32(h + 8);
    if(ver != 1) {
      std::fprintf(stderr, "[movie] version %u desconocida (esta compilacion lee la 1)\n", ver);
      std::fclose(fp); fp = nullptr; return;
    }
    u32 c1 = get32(h + 0x10), c2 = get32(h + 0x14);
    if(c1 != crc1 || c2 != crc2) {
      std::fprintf(stderr, "[movie] esta pelicula es de otro cartucho: grabada con "
                           "crc %08x/%08x, cargado %08x/%08x. No se reproduce.\n", c1, c2, crc1, crc2);
      std::fclose(fp); fp = nullptr; return;
    }
    u8 mtv = h[0x18], mram = h[0x19];
    if(mtv != tv || mram != ramMb)
      std::fprintf(stderr, "[movie] AVISO: grabada con tv=%u rdram=%u MB y aqui hay tv=%u rdram=%u MB; "
                           "la repeticion puede separarse\n", mtv, mram, tv, ramMb);
    total = get64(h + kOffPolls);
    mode = Mode::Play;
    path = play;
    std::fprintf(stderr, "[movie] reproduciendo %s (%llu muestras%s)\n", play,
                 (unsigned long long)total, total ? "" : ", cabecera truncada");
    return;
  }

  fp = std::fopen(rec, "wb");
  if(!fp) { std::fprintf(stderr, "[movie] no se puede crear %s\n", rec); return; }
  u8 h[kHdrSize] = {};
  std::memcpy(h, "K64MOVIE", 8);
  put32(h + 8, 1);
  put32(h + 0x0C, 1);                       // bit0: arranque en frio
  put32(h + 0x10, crc1);
  put32(h + 0x14, crc2);
  h[0x18] = tv; h[0x19] = ramMb; h[0x1A] = ports;
  put64(h + kOffPolls, 0);                  // se rellena al cerrar
  std::memcpy(h + 0x24, name.c_str(), name.size() < 20 ? name.size() : 20);
  std::fwrite(h, 1, kHdrSize, fp);
  mode = Mode::Rec;
  path = rec;
  std::fprintf(stderr, "[movie] grabando en %s\n", rec);
}

// En el comando 0x01 del joybus, con el estado que el emulador iba a contestar. Al grabar
// lo apunta tal cual; al reproducir lo SUSTITUYE por el de la pelicula.
inline auto sample(int ch, u32& btn, s8& sx, s8& sy) -> void {
  if(mode == Mode::Off || !fp) return;
  if(mode == Mode::Rec) {
    u8 r[5] = { (u8)ch, (u8)(btn >> 8), (u8)btn, (u8)sx, (u8)sy };
    // Un disco lleno no puede pasar callando: la pelicula quedaria corta y la repeticion se
    // separaria sin que nadie supiera donde. Se dice, se cierra y se deja de grabar.
    if(std::fwrite(r, 1, 5, fp) != 5) {
      std::fprintf(stderr, "[movie] no se puede escribir en %s tras %llu muestras; se deja de grabar\n",
                   path.c_str(), (unsigned long long)polls.load(std::memory_order_relaxed));
      std::fclose(fp); fp = nullptr; mode = Mode::Off;
      return;
    }
    polls.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if(ended.load(std::memory_order_relaxed)) return;
  // El final lo manda la cabecera cuando la trae: rebobinar para regrabar un tramo deja
  // detras la cola de la toma anterior, y leerla seria reproducir entradas que la pelicula
  // ya no tiene. Sin cuenta en la cabecera (grabacion truncada) el final es el del fichero.
  u64 n = polls.load(std::memory_order_relaxed);
  u8 r[5];
  if((total && n >= total) || std::fread(r, 1, 5, fp) != 5) {
    ended.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "[movie] fin de la pelicula tras %llu muestras; a partir de aqui manda "
                         "el mando del anfitrion\n", (unsigned long long)n);
    return;
  }
  // El orden de los conectores lo fija el bloque de comandos del juego, asi que una muestra
  // que no case con el conector que la pide significa que la partida ya no es la misma
  // (otra region, otro numero de mandos enchufados). Se avisa UNA vez y se sigue: parar en
  // seco perderia el resto de la pelicula, que suele seguir siendo util.
  if(r[0] != (u8)ch && !warnedChan) {
    warnedChan = true;
    std::fprintf(stderr, "[movie] desincronizada en la muestra %llu: la pelicula trae el conector %u "
                         "y el juego pidio el %d\n", (unsigned long long)n, r[0], ch);
  }
  btn = ((u32)r[1] << 8) | r[2];
  sx  = (s8)r[3];
  sy  = (s8)r[4];
  polls.store(n + 1, std::memory_order_relaxed);
}

// Rebobina la cinta al sondeo `n`. Existe porque un estado guardado rebobina el JUEGO: si la
// pelicula no le sigue, la repeticion se va a la deriva justo en el uso que da sentido a las
// dos cosas juntas (rehacer un tramo). Grabando, escribir desde aqui pisa la toma anterior y
// la cuenta de la cabecera se ajusta al cerrar, o sea que es regrabar.
inline auto seek(u64 n) -> void {
  if(mode == Mode::Off || !fp) return;
  // fseek toma long: en Windows son 2 GiB, o sea 400 millones de sondeos (semanas de
  // partida). Mas alla de eso no se rebobina, y decirlo es mejor que fingirlo.
  if(n > (u64)((0x7fffffffu - kHdrSize) / 5)
     || std::fseek(fp, (long)(kHdrSize + n * 5), SEEK_SET) != 0) {
    std::fprintf(stderr, "[movie] no se puede rebobinar al sondeo %llu\n", (unsigned long long)n);
    return;
  }
  polls.store(n, std::memory_order_relaxed);
  ended.store(mode == Mode::Play && total && n >= total, std::memory_order_relaxed);
}

inline auto finish() -> void {
  if(!fp) return;
  u64 n = polls.load(std::memory_order_relaxed);
  if(mode == Mode::Rec) {
    u8 c[8]; put64(c, n);
    std::fseek(fp, (long)kOffPolls, SEEK_SET);
    std::fwrite(c, 1, 8, fp);
    std::fprintf(stderr, "[movie] grabadas %llu muestras en %s\n",
                 (unsigned long long)n, path.c_str());
  } else if(mode == Mode::Play) {
    std::fprintf(stderr, "[movie] reproducidas %llu muestras de %s%s\n",
                 (unsigned long long)n, path.c_str(),
                 ended.load(std::memory_order_relaxed) ? " (agotada)" : "");
  }
  std::fclose(fp);
  fp = nullptr;
  mode = Mode::Off;
}

}  // namespace kestrel::movie
