// GENERADO por tools/gen_optdefs.py desde tools/launcher/options.py. NO EDITAR A MANO.
// Regenerar: python tools/gen_optdefs.py
#include "optdefs.hpp"

namespace kestrel::ui {

static const Choice kCh_speedmode[] = {
  {"libre", "Libre - manda lo que se elija aqui debajo"},
  {"hw", "Fiel a consola - relojes N64 exactos y 59.94 campos/s"},
};
static const Choice kCh_throttle[] = {
  {"auto", "Automatico - limita si hay ventana"},
  {"1", "Siempre limitado a 59.94 campos/s"},
  {"0", "Sin limite - a tope"},
};
static const Choice kCh_plugin[] = {
  {"auto", "Automatico - la compilacion disponible"},
  {"soft", "SoftRDP - rasterizador propio en CPU, determinista"},
  {"prdp", "paraLLEl-RDP - LLE por Vulkan en GPU"},
};
static const Choice kCh_winscale[] = {
  {"1", "1x - 320x240"},
  {"2", "2x - 640x480"},
  {"3", "3x - 960x720"},
  {"4", "4x - 1280x960"},
  {"5", "5x - 1600x1200"},
  {"6", "6x - 1920x1440"},
  {"8", "8x - 2560x1920"},
};
static const Choice kCh_aspect[] = {
  {"4:3", "4:3 - la senal que saca el VI (fiel)"},
  {"16:9", "16:9 - estirar la imagen anamorfica"},
  {"estirar", "Llenar la ventana (deforma)"},
};
static const Choice kCh_upscale[] = {
  {"1", "1x - resolucion nativa"},
  {"2", "2x"},
  {"4", "4x"},
  {"8", "8x"},
};
static const Choice kCh_savetype[] = {
  {"auto", "Automatico - por ID de cartucho"},
  {"none", "Ninguno"},
  {"eeprom4k", "EEPROM 4 kbit"},
  {"eeprom16k", "EEPROM 16 kbit"},
  {"sram256k", "SRAM 256 kbit"},
  {"sram768k", "SRAM 768 kbit"},
  {"flash1m", "FlashRAM 1 Mbit"},
};
static const Choice kCh_tvtype[] = {
  {"auto", "Automatico - por region del cartucho"},
  {"ntsc", "NTSC (60 Hz)"},
  {"pal", "PAL (50 Hz)"},
  {"mpal", "PAL-M (60 Hz)"},
};
static const Choice kCh_rdram[] = {
  {"8", "8 MB - con Expansion Pak"},
  {"4", "4 MB - consola de serie"},
};

static const Option kOpt_cpu[] = {
  {"jit", "KESTREL_JIT", "Recompilador dinamico (JIT)", OType::Bool, "1", "Traduce bloques MIPS a x86-64. Apagado = interprete puro, que es el oraculo de correccion: si un juego falla con JIT, comprobar aqui primero.", false, false, true, 0, 0, 0, nullptr, 0, false},
  {"jit_link", "KESTREL_JIT_NOLINK", "Encadenado de bloques", OType::Bool, "1", "Un bloque salta directo a su sucesor sin volver al despachador. Palanca grande de rendimiento del JIT.", false, true, false, 0, 0, 0, nullptr, 0, false},
  {"jit_regcache", "KESTREL_JIT_NOREGCACHE", "Cache de registros", OType::Bool, "1", "Mantiene registros MIPS calientes en registros x86 callee-saved dentro del bloque.", false, true, false, 0, 0, 0, nullptr, 0, false},
  {"jit_smc", "KESTREL_JIT_NOSMC", "Validacion de codigo automodificable", OType::Bool, "1", "Revalida las palabras del bloque contra la linea de I-cache en cada entrada. Apagarlo va mas rapido y es INCORRECTO en cuanto un juego reescriba codigo.", false, true, false, 0, 0, 0, nullptr, 0, false},
  {"fetchfast", "KESTREL_NOFETCHFAST", "Fetch fast-path del interprete", OType::Bool, "1", "Memoiza la traduccion de la linea de I-cache de 32 B. Gana mucho en codigo mapeado por TLB.", false, true, false, 0, 0, 0, nullptr, 0, false},
  {"jit_chain", "KESTREL_JIT_CHAIN", "Longitud maxima de cadena", OType::Int, "0", "0 = valor por defecto del emulador. Cuantos bloques enlazados se recorren antes de volver al despachador.", true, false, false, 0, 4096, 0, nullptr, 0, false},
  {"jit_stats", "KESTREL_JIT_STATS", "Estadisticas del JIT", OType::Bool, "0", "Cobertura, longitud media de bloque e histograma de por que se declina un bloque.", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"jit_trace", "KESTREL_JIT_TRACE", "Traza de compilacion", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"jit_diff", "KESTREL_JIT_DIFF", "Diff contra el interprete", OType::Bool, "0", "Ejecuta cada bloque en JIT y en interprete y compara el estado. Lentisimo; caza bugs del JIT.", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"jit_brdiff", "KESTREL_JIT_BRDIFF", "Diff solo de saltos", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"jit_nobranch", "KESTREL_JIT_NOBRANCH", "Sin absorcion de saltos", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"jit_nojmp", "KESTREL_JIT_NOJMP", "Sin absorcion de J/JAL/JR", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"jit_nofast", "KESTREL_JIT_NOFAST", "Sin prologo rapido", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"cpuidle", "KESTREL_CPUIDLE", "Salto del bucle ocioso de la CPU", OType::Bool, "1", "El hilo ocioso de libultra es un salto a si mismo con NOP en la ranura de retardo: no observa nada mas que Count, asi que se cobra de golpe hasta el mismo instante en que la cadena del JIT habria vuelto a mirar los eventos. Apagarlo tiene que dar el mismo resultado, solo mas lento.", false, false, true, 0, 0, 0, nullptr, 0, false},
  {"lle_ipl3", "KESTREL_LLE_IPL3", "IPL3 real (LLE)", OType::Bool, "0", "Arranca ejecutando el IPL3 del cartucho en vez del arranque HLE.", true, false, false, 0, 0, 0, nullptr, 0, false},
};

static const Option kOpt_rcp[] = {
  {"threads", "KESTREL_THREADS", "RCP multihilo", OType::Bool, "1", "RDP y RSP en sus propios hilos del sistema. Apagado = lockstep determinista (el modo que valida los md5 de referencia).", false, false, true, 0, 0, 0, nullptr, 0, false},
  {"rspjit", "KESTREL_RSPJIT", "Recompilador del RSP", OType::Bool, "1", "Dynarec para el microcodigo del RSP.", false, false, true, 0, 0, 0, nullptr, 0, false},
  {"rspjit_stats", "KESTREL_RSPJIT_STATS", "Estadisticas del RSP JIT", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"rspsse", "KESTREL_NORSPSSE", "VU por SSE", OType::Bool, "1", "Unidad vectorial del RSP con instrucciones SSE del anfitrion en vez de escalar.", false, true, false, 0, 0, 0, nullptr, 0, false},
  {"vecfast", "KESTREL_NOVECFAST", "Cargas vectoriales rapidas", OType::Bool, "1", "", false, true, false, 0, 0, 0, nullptr, 0, false},
  {"rspidle", "KESTREL_RSPIDLE", "Aparcar el RSP en la espera del FIFO", OType::Bool, "1", "Con el motor del RDP drenado, el sondeo del microcodigo sobre DPC_CURRENT no puede cambiar de respuesta: se aparca el RSP hasta que la CPU archiva el siguiente tramo. Apagarlo tiene que dar el mismo resultado.", false, false, true, 0, 0, 0, nullptr, 0, false},
  {"spinpause", "KESTREL_SPINPAUSE", "Pista PAUSE en las esperas activas", OType::Bool, "1", "La CPU y el RSP se vigilan girando sobre contadores que escribe el otro. PAUSE le dice al nucleo que eso es una espera, para que no le robe la linea de cache ni las ranuras de emision al hermano. Es solo una pista de anfitrion: el resultado sale identico.", true, false, true, 0, 0, 0, nullptr, 0, false},
  {"barspin", "KESTREL_BARSPIN", "Vueltas de la barrera del SP", OType::Int, "0", "0 = por defecto (2048). Cuanto gira la CPU en la barrera del RSP antes de dormir.", true, false, false, 0, 1e+06, 0, nullptr, 0, false},
  {"rdpspin", "KESTREL_RDPSPIN", "Vueltas del RDP ocioso", OType::Int, "32768", "Cuanto gira el hilo del RDP, sin trabajo, antes de dormir. 0 = dormir enseguida. Ahorra despertarlo por el kernel en cada DPC_END: -13 % de tiempo con Parallel-RDP.", true, false, false, 0, 1e+07, 0, nullptr, 0, false},
  {"rspspin", "KESTREL_RSPSPIN", "Vueltas del RSP ocioso", OType::Int, "0", "Cuanto gira el hilo del RSP entre tareas antes de dormir. Medido neutro; 0 = dormir enseguida.", true, false, false, 0, 1e+07, 0, nullptr, 0, false},
  {"dpspin", "KESTREL_DPSPIN", "Vueltas esperando al RDP", OType::Int, "262144", "Cuanto gira la CPU (y el RSP) esperando a que el RDP cierre un tramo antes de dormir. 0 = dormir enseguida.", true, false, false, 0, 1e+07, 0, nullptr, 0, false},
  {"rspinline", "KESTREL_RSPINLINE", "RSP en linea", OType::Bool, "0", "Ejecuta la tarea del RSP dentro del hilo de CPU en vez de cederla al hilo del RCP.", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"rdpinline", "KESTREL_RDPINLINE", "RDP en linea", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"rdpdrain", "KESTREL_RDPDRAIN", "Drenar RDP en cada sync", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"paceslack", "KESTREL_PACESLACK", "Holgura del regulador RCP", OType::Int, "0", "0 = por defecto. Cuanto puede adelantarse la CPU al RSP antes de esperar.", true, false, false, 0, 1e+06, 0, nullptr, 0, false},
  {"occ", "KESTREL_OCC", "Muestreo de solape (us)", OType::Int, "0", "0 = apagado. Periodo del muestreador de ocupacion de los hilos del RCP.", true, false, false, 0, 100000, 0, nullptr, 0, false},
};

static const Option kOpt_oc[] = {
  {"speedmode", "KESTREL_SPEEDMODE", "Modo de velocidad", OType::Choice, "libre", "Fiel a consola ignora los multiplicadores y el CPI puesto a mano, y con ventana clava el campo de video a 59.94 Hz: el juego ve el mismo tiempo que veria en la maquina real. Sin ventana no limita el ritmo de pared, porque no hay pantalla que respetar y el resultado no cambia. Es lo que hay que poner para las demos que cuentan campos (la liana de Donkey Kong 64) y para comparar contra hardware.", false, false, false, 0, 0, 0, kCh_speedmode, 2, false},
  {"oc_link", nullptr, "Ligar los tres dominios", OType::Bool, "1", "Un solo mando para CPU, RSP y RDRAM.", false, false, false, 0, 0, 0, nullptr, 0, false},
  {"oc_all", "KESTREL_OC", "Multiplicador global", OType::Float, "1", "", false, false, false, 0.25, 8, 0.05, nullptr, 0, false},
  {"oc_cpu", "KESTREL_OC_CPU", "CPU - R4300i 93.75 MHz", OType::Float, "1", "", false, false, false, 0.25, 8, 0.05, nullptr, 0, false},
  {"oc_rsp", "KESTREL_OC_RSP", "RSP - 62.5 MHz", OType::Float, "1", "", false, false, false, 0.25, 8, 0.05, nullptr, 0, false},
  {"oc_rdram", "KESTREL_OC_RDRAM", "RDRAM - 250 MHz DDR", OType::Float, "1", "Solo mueve el modelo de ancho de banda; no acelera la logica del juego.", false, false, false, 0.25, 8, 0.05, nullptr, 0, false},
  {"throttle", "KESTREL_THROTTLE", "Limitar a tiempo real", OType::Choice, "auto", "El ancla es el campo de video, no el reloj de CPU: con overclock el juego hace mas trabajo por campo, pero los campos siguen saliendo a 59.94 Hz.", false, false, false, 0, 0, 0, kCh_throttle, 3, true},
  {"viticks", "KESTREL_VITICKS", "Subdivisiones del campo de video", OType::Int, "16", "Con cuanta precision cae la interrupcion del VI dentro del campo. Subirlo afina los efectos de rastreo y cuesta vueltas de bucle.", true, false, false, 1, 256, 0, nullptr, 0, false},
};

static const Option kOpt_video[] = {
  {"plugin", nullptr, "Rasterizador (plugin grafico)", OType::Choice, "auto", "paraLLEl-RDP necesita una compilacion con KESTREL_PRDP=ON. El lanzador elige el ejecutable adecuado y activa la variable.", false, false, false, 0, 0, 0, kCh_plugin, 3, false},
  {"video", "KESTREL_VIDEO", "Abrir ventana", OType::Bool, "1", "Apagado = sin ventana (modo lote / captura).", false, false, false, 0, 0, 0, nullptr, 0, false},
  {"winscale", "KESTREL_WINSCALE", "Escala de ventana", OType::Choice, "2", "", false, false, false, 0, 0, 0, kCh_winscale, 7, true},
  {"winsize", "KESTREL_WINSIZE", "Tamano exacto WxH", OType::Text, "", "Vacio = usar la escala. Ejemplo: 1600x900. Manda sobre la escala.", false, false, false, 0, 0, 0, nullptr, 0, true},
  {"fullscreen", "KESTREL_FULLSCREEN", "Pantalla completa", OType::Bool, "0", "Usa el modo actual del monitor primario; no cambia la resolucion del escritorio.", false, false, true, 0, 0, 0, nullptr, 0, true},
  {"aspect", "KESTREL_ASPECT", "Relacion de aspecto", OType::Choice, "4:3", "El N64 saca SIEMPRE 4:3. 16:9 no ensancha el campo de vision -- eso solo lo puede hacer el juego -- sino que estira la imagen que ya generan aplastada los juegos con modo panoramico propio (Perfect Dark, GoldenEye, Turok, Rush 2).", false, false, false, 0, 0, 0, kCh_aspect, 3, true},
  {"upscale", "KESTREL_UPSCALE", "Escalado interno (paraLLEl-RDP)", OType::Choice, "1", "Rasteriza a N veces la resolucion del N64 dentro de la GPU. Solo con paraLLEl-RDP; el SoftRDP va siempre a 1x. Lo que el juego lee de su propio framebuffer sigue siendo 1x, asi que no rompe los efectos que releen la imagen.", false, false, false, 0, 0, 0, kCh_upscale, 4, false},
  {"ssaa", "KESTREL_SSAA", "Supermuestreo al volcar a 1x", OType::Bool, "0", "Con escalado interno, al devolver la imagen ampliada al framebuffer del juego promedia las NxN muestras en vez de coger una. Antialiasing gratis en los efectos que releen el framebuffer, a cambio de una pasada mas.", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"hud", "KESTREL_HUD_OFF", "HUD de telemetria sobre la imagen", OType::Bool, "1", "", false, true, false, 0, 0, 0, nullptr, 0, true},
  {"noaa", "KESTREL_NOAA", "Antialiasing del RDP", OType::Bool, "1", "SoftRDP. Apagarlo sube el relleno y cambia el borde de los poligonos.", false, true, false, 0, 0, 0, nullptr, 0, false},
  {"nofilter", "KESTREL_NOFILTER", "Filtrado de texturas", OType::Bool, "1", "", false, true, false, 0, 0, 0, nullptr, 0, false},
  {"noblend", "KESTREL_NOBLEND", "Mezclador (blender)", OType::Bool, "1", "", false, true, false, 0, 0, 0, nullptr, 0, false},
  {"noraster", "KESTREL_NORASTER", "Rasterizado", OType::Bool, "1", "Apagado no dibuja nada: sirve para medir cuanto cuesta el rasterizador.", true, true, false, 0, 0, 0, nullptr, 0, false},
  {"prdp_stats", "KESTREL_PRDP_STATS", "Estadisticas de paraLLEl-RDP", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"video_test", "KESTREL_VIDEO_TEST", "Patron de prueba", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"video_dump", "KESTREL_VIDEO_DUMP", "Volcar cada campo a BMP", OType::Path, "", "", true, false, false, 0, 0, 0, nullptr, 0, false},
};

static const Option kOpt_audio[] = {
  {"audio", "KESTREL_AUDIO", "Audio activado", OType::Bool, "1", "", false, false, true, 0, 0, 0, nullptr, 0, true},
  {"volume", "KESTREL_VOLUME", "Volumen (%)", OType::Int, "100", "Atenuacion aplicada a las muestras antes de mandarlas al anfitrion. No toca el modelo del AI: el juego sigue viendo el mismo audio.", false, false, false, 0, 100, 0, nullptr, 0, true},
  {"audio_trace", "KESTREL_AUDIO_TRACE", "Traza de audio", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"audiohook", "KESTREL_AUDIOHOOK", "Enganche de audio", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
};

static const Option kOpt_save[] = {
  {"savetype", "KESTREL_SAVETYPE", "Tipo de guardado", OType::Choice, "auto", "Se resuelve por ID de cartucho; esto lo fuerza cuando la ROM no esta en la tabla.", false, false, false, 0, 0, 0, kCh_savetype, 7, false},
  {"tvtype", "KESTREL_TVTYPE", "Norma de television", OType::Choice, "auto", "Lo que el juego lee en osTvType, y de donde sale el ritmo de campo. En la consola de verdad lo fija la maquina, y la region del cartucho coincide con ella; algunos juegos se niegan a funcionar con la norma equivocada.", false, false, false, 0, 0, 0, kCh_tvtype, 4, false},
  {"rdram", "KESTREL_RDRAM", "Memoria RDRAM", OType::Choice, "8", "La N64 trae 4 MB y el Expansion Pak la sube a 8. Los juegos lo leen en osMemSize y algunos cambian de comportamiento: reservan menos buferes o bajan la resolucion con 4 MB, y Donkey Kong 64 y el modo de un jugador de Perfect Dark EXIGEN los 8. La RDRAM se dimensiona una sola vez, asi que cambiarlo pide relanzar, y un estado guardado con un tamano no se puede cargar con el otro.", false, false, false, 0, 0, 0, kCh_rdram, 2, false},
  {"cheats", "KESTREL_CHEATS", "Fichero de trucos (.cht)", OType::Path, "", "Codigos tipo GameShark, aplicados en cada campo de video igual que el cartucho de verdad. Sin fichero se usa el .cht que haya al lado de la ROM con su mismo nombre. Formato: [Nombre] abre un truco ([-Nombre] lo deja apagado) y debajo van las lineas AAAAAAAA VVVV tal como se publican.", false, false, false, 0, 0, 0, nullptr, 0, false},
};

static const Option kOpt_movie[] = {
  {"movie_rec", "KESTREL_MOVIE_REC", "Grabar entradas en", OType::Path, "", "Fichero .k64m donde apuntar cada lectura de botones. Se graba desde el arranque en frio: una repeticion vale desde el encendido, no desde media partida.", false, false, false, 0, 0, 0, nullptr, 0, false},
  {"movie_play", "KESTREL_MOVIE_PLAY", "Reproducir entradas de", OType::Path, "", "Sustituye el mando por el de la pelicula. Se comprueban los CRC del cartucho y no se reproduce una pelicula de otro juego. Al acabar la cinta vuelve a mandar el mando del anfitrion. Con las dos casillas puestas manda esta.", false, false, false, 0, 0, 0, nullptr, 0, false},
};

static const Option kOpt_rewind[] = {
  {"rewind", "KESTREL_REWIND", "Activar rebobinado", OType::Bool, "0", "Con la tecla de retroceso el juego va hacia atras mientras se mantenga apretada. Apagado no cuesta nada; encendido, el emulador fotografia la maquina cada pocos campos (parando el RDP y el RSP en cada foto) y guarda solo las diferencias.", false, false, false, 0, 0, 0, nullptr, 0, false},
  {"rewind_fields", "KESTREL_REWIND_FIELDS", "Campos entre fotos", OType::Int, "2", "Cada cuantos campos de video se toma una foto. Menos = rebobinado mas fino y mas caro; mas = mas barato y a saltos mas gordos. 2 son unas 30 fotos por segundo, que es el paso con el que se juega.", false, false, false, 1, 60, 0, nullptr, 0, false},
  {"rewind_mb", "KESTREL_REWIND_MB", "Memoria para la cinta (MB)", OType::Int, "256", "Tope de memoria de las diferencias. Al llenarse se tira el pasado LEJANO, que es lo que no se va a pedir. Cuanto dura depende del juego: lo que ocupa una foto es lo que el juego cambia entre foto y foto.", false, false, false, 8, 4096, 0, nullptr, 0, false},
};

static const Option kOpt_mcp[] = {
  {"port", nullptr, "Puerto del servidor", OType::Int, "9128", "", false, false, false, 1, 65535, 0, nullptr, 0, false},
  {"paused", nullptr, "Arrancar en pausa", OType::Bool, "0", "Arranca detenido para poder poner puntos de ruptura antes de la primera instruccion. Sin esto el emulador arranca en marcha libre (--run).", false, false, false, 0, 0, 0, nullptr, 0, false},
  {"heartbeat", "KESTREL_HEARTBEAT", "Latido en consola", OType::Bool, "0", "Imprime velocidad, MIPS y campos cada poco.", false, false, false, 0, 0, 0, nullptr, 0, false},
  {"hostprof", "KESTREL_HOSTPROF", "Perfilador del anfitrion", OType::Bool, "0", "Muestrea donde gasta el tiempo el proceso del emulador, no el juego.", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"hostprof_out", "KESTREL_HOSTPROF_OUT", "Fichero de salida del perfilador", OType::Path, "", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"hostprof_who", "KESTREL_HOSTPROF_WHO", "Hilos a perfilar", OType::Text, "", "", true, false, false, 0, 0, 0, nullptr, 0, false},
};

static const Option kOpt_batch[] = {
  {"maxinsn", "KESTREL_MAXINSN", "Tope de instrucciones", OType::Int, "0", "0 = sin tope. El emulador termina al llegar.", false, false, false, 0, 1e+11, 0, nullptr, 0, false},
  {"maxflips", "KESTREL_MAXFLIPS", "Tope de intercambios de framebuffer", OType::Int, "0", "", false, false, false, 0, 1e+06, 0, nullptr, 0, false},
  {"maxsyncs", "KESTREL_MAXSYNCS", "Tope de DP full-sync", OType::Int, "0", "", true, false, false, 0, 1e+06, 0, nullptr, 0, false},
  {"stable", "KESTREL_STABLE", "Parar cuando la imagen se estabilice", OType::Int, "0", "", true, false, false, 0, 100000, 0, nullptr, 0, false},
  {"fbdump", "KESTREL_FBDUMP", "Volcar framebuffer al salir", OType::Path, "", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"fielddump", "KESTREL_FIELDDUMP", "Volcar campos", OType::Path, "", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"fieldhash", "KESTREL_FIELDHASH", "Hash por campo", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"watchdog", "KESTREL_WATCHDOG", "Perro guardian (s)", OType::Int, "0", "Mata el proceso si no progresa. 0 = apagado.", true, false, false, 0, 100000, 0, nullptr, 0, false},
  {"hangdog", "KESTREL_HANGDOG", "Detector de cuelgue", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
};

static const Option kOpt_debug[] = {
  {"bp", "KESTREL_BP", "Punto de ruptura (PC hex)", OType::Hex, "", "", false, false, false, 0, 0, 0, nullptr, 0, false},
  {"bptrace", "KESTREL_BPTRACE", "Traza en el punto de ruptura", OType::Bool, "0", "", false, false, false, 0, 0, 0, nullptr, 0, false},
  {"watch", "KESTREL_WATCH", "Punto de vigilancia de escritura (addr)", OType::Hex, "", "", false, false, false, 0, 0, 0, nullptr, 0, false},
  {"watchlen", "KESTREL_WATCHLEN", "Longitud vigilada (bytes)", OType::Int, "0", "", false, false, false, 0, 1.04858e+06, 0, nullptr, 0, false},
  {"watchp", "KESTREL_WATCHP", "Vigilancia por D-cache", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"dis", "KESTREL_DIS", "Desensamblado", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"exctrace", "KESTREL_EXCTRACE", "Traza de excepciones", OType::Bool, "0", "", false, false, false, 0, 0, 0, nullptr, 0, false},
  {"exctail", "KESTREL_EXCTAIL", "Cola de excepciones", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"faulttrace", "KESTREL_FAULTTRACE", "Traza de fallos", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"faultstop", "KESTREL_FAULTSTOP", "Parar en el primer fallo", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"intlog", "KESTREL_INTLOG", "Registro de interrupciones", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"irqtrace", "KESTREL_IRQTRACE", "Traza de IRQ del MI", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"noint", "KESTREL_NOINT", "Sin interrupciones", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"vilog", "KESTREL_VILOG", "Registro del VI", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"silog", "KESTREL_SILOG", "Registro del SI (joybus)", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"rsptrace", "KESTREL_RSPTRACE", "Traza del RSP", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"rdptrace", "KESTREL_RDPTRACE", "Traza del RDP", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"rdpops", "KESTREL_RDPOPS", "Contador de ordenes del RDP", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"citrace", "KESTREL_CITRACE", "Traza de imagen de color", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"tridbg", "KESTREL_TRIDBG", "Depuracion de triangulos", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"dpsynclog", "KESTREL_DPSYNCLOG", "Registro de DP sync", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"vrdplog", "KESTREL_VRDPLOG", "Registro de paraLLEl-RDP", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"evlog", "KESTREL_EVLOG", "Registro de eventos del bus", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"pcring", "KESTREL_PCRING", "Anillo de PCs", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"pcsample", "KESTREL_PCSAMPLE", "Muestreo de PC", OType::Int, "0", "", true, false, false, 0, 1e+08, 0, nullptr, 0, false},
  {"guestthreads", "KESTREL_GUESTTHREADS", "Hilos del guest (libultra)", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"threadscan", "KESTREL_THREADSCAN", "Barrido de hilos del guest", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"qchk", "KESTREL_QCHK", "Invariante de cola del kernel (addr)", OType::Hex, "", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"qchkn", "KESTREL_QCHKN", "Periodo de comprobacion (insn)", OType::Int, "0", "", true, false, false, 0, 1e+08, 0, nullptr, 0, false},
  {"trapzero", "KESTREL_TRAPZERO", "Trampa: escritura a r0", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"trapwild", "KESTREL_TRAPWILD", "Trampa: acceso salvaje", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"trapri", "KESTREL_TRAPRI", "Trampa: registros RI", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"trapspreg", "KESTREL_TRAPSPREG", "Trampa: registros SP", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"halt_unimpl", "KESTREL_HALT_UNIMPL", "Parar en instruccion no implementada", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"fporacle", "KESTREL_FPORACLE", "Oraculo de coma flotante", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"fptrace", "KESTREL_FPTRACE", "Traza de FPU", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"vustat", "KESTREL_VUSTAT", "Estadisticas de la VU", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
  {"dcwt", "KESTREL_DCWT", "Traza de write-through de D-cache", OType::Bool, "0", "", true, false, false, 0, 0, 0, nullptr, 0, false},
};

static const Category kCats[] = {
  {"cpu", "CPU", "Interprete VR4300 y recompilador dinamico x86-64.", kOpt_cpu, (int)(sizeof kOpt_cpu / sizeof(Option))},
  {"rcp", "RCP e hilos", "RSP, RDP y el reparto en hilos del anfitrion.", kOpt_rcp, (int)(sizeof kOpt_rcp / sizeof(Option))},
  {"oc", "Overclock", "Multiplicadores por dominio de reloj. 1.00x = consola real. Subir el de CPU quita ralentizacion, pero cambia cuanto trabajo hace el juego entre campos de video: hay juegos que atan su logica al reloj y se rompen.", kOpt_oc, (int)(sizeof kOpt_oc / sizeof(Option))},
  {"video", "Video", "Rasterizador, ventana y presentacion.", kOpt_video, (int)(sizeof kOpt_video / sizeof(Option))},
  {"audio", "Audio", "Salida de sonido del anfitrion.", kOpt_audio, (int)(sizeof kOpt_audio / sizeof(Option))},
  {"save", "Cartucho", "Dispositivo de guardado del cartucho.", kOpt_save, (int)(sizeof kOpt_save / sizeof(Option))},
  {"movie", "Peliculas", "Grabacion y reproduccion de entradas (.k64m). Lo que se graba no es lo que aprieta el jugador sino lo que el JUEGO LEE en cada lectura del joybus, que es la unica frontera que el invitado percibe.", kOpt_movie, (int)(sizeof kOpt_movie / sizeof(Option))},
  {"rewind", "Rebobinado", "Deshacer lo que acaba de pasar. Cuesta CPU y memoria: cada foto obliga a parar el RCP y a recorrer el estado entero, asi que viene apagado y solo se paga si se enciende.", kOpt_rewind, (int)(sizeof kOpt_rewind / sizeof(Option))},
  {"mcp", "Telemetria y MCP", "Servidor TCP+JSON propio del emulador. Es la via por la que el MCP lee registros, memoria, framebuffer y perfiles.", kOpt_mcp, (int)(sizeof kOpt_mcp / sizeof(Option))},
  {"batch", "Limites y lote", "Topes de ejecucion. Imprescindibles para no dejar nunca un proceso colgado.", kOpt_batch, (int)(sizeof kOpt_batch / sizeof(Option))},
  {"debug", "Depuracion", "Trazas y trampas. Todas cuestan rendimiento; ninguna esta activa de fabrica.", kOpt_debug, (int)(sizeof kOpt_debug / sizeof(Option))},
};

auto categories() -> const Category* { return kCats; }
auto categoryCount() -> int { return (int)(sizeof kCats / sizeof(Category)); }

static const PadCtl kPads[] = {
  {"A", "A", 0x8000, "X", "A"},
  {"B", "B", 0x4000, "C", "B"},
  {"Z", "Z", 0x2000, "SPACE", "LEFT_TRIGGER"},
  {"START", "Start", 0x1000, "ENTER", "START"},
  {"DU", "Cruz arriba", 0x0800, "UP", "DPAD_UP"},
  {"DD", "Cruz abajo", 0x0400, "DOWN", "DPAD_DOWN"},
  {"DL", "Cruz izquierda", 0x0200, "LEFT", "DPAD_LEFT"},
  {"DR", "Cruz derecha", 0x0100, "RIGHT", "DPAD_RIGHT"},
  {"L", "L", 0x0020, "Q", "LEFT_BUMPER"},
  {"R", "R", 0x0010, "E", "RIGHT_BUMPER"},
  {"CU", "C arriba", 0x0008, "I", ""},
  {"CD", "C abajo", 0x0004, "K", ""},
  {"CL", "C izquierda", 0x0002, "J", ""},
  {"CR", "C derecha", 0x0001, "L", ""},
  {"SX+", "Stick derecha", 0, "D", ""},
  {"SX-", "Stick izquierda", 0, "A", ""},
  {"SY+", "Stick arriba", 0, "W", ""},
  {"SY-", "Stick abajo", 0, "S", ""},
};

auto padControls() -> const PadCtl* { return kPads; }
auto padControlCount() -> int { return (int)(sizeof kPads / sizeof(PadCtl)); }

}  // namespace kestrel::ui
