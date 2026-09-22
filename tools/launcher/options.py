# -*- coding: utf-8 -*-
"""Esquema de opciones de kestrel64.

Una sola fuente de verdad para el lanzador: cada opcion dice como se llama la variable de
entorno que la lleva al emulador, de que tipo es, que valor trae de fabrica y que hace.
El frontend dibuja los controles a partir de esto; el backend traduce el perfil guardado a
un entorno de proceso. Anadir una bandera nueva al emulador = anadir una fila aqui.

tipos:  bool | int | float | text | path | choice | hex
`adv`   la opcion es de diagnostico: queda escondida tras "mostrar avanzadas".
`invert` la variable de entorno DESACTIVA algo (KESTREL_NOxxx): el control se muestra en
         positivo y el backend exporta la variable cuando el control esta APAGADO.
"""


def O(id, env, label, type, default=None, help="", adv=False, **kw):
    d = dict(id=id, env=env, label=label, type=type, default=default, help=help, adv=adv)
    d.update(kw)
    return d


CATEGORIES = [
  # ============================================================ CPU / dynarec
  dict(id="cpu", label="CPU", icon="cpu",
       desc="Interprete VR4300 y recompilador dinamico x86-64.", options=[
    O("jit", "KESTREL_JIT", "Recompilador dinamico (JIT)", "bool", True,
      "Traduce bloques MIPS a x86-64. Apagado = interprete puro, que es el oraculo de "
      "correccion: si un juego falla con JIT, comprobar aqui primero.", tri=True),
    O("jit_link", "KESTREL_JIT_NOLINK", "Encadenado de bloques", "bool", True, invert=True,
      help="Un bloque salta directo a su sucesor sin volver al despachador. Palanca grande "
           "de rendimiento del JIT."),
    O("jit_regcache", "KESTREL_JIT_NOREGCACHE", "Cache de registros", "bool", True, invert=True,
      help="Mantiene registros MIPS calientes en registros x86 callee-saved dentro del bloque."),
    O("jit_smc", "KESTREL_JIT_NOSMC", "Validacion de codigo automodificable", "bool", True,
      invert=True,
      help="Revalida las palabras del bloque contra la linea de I-cache en cada entrada. "
           "Apagarlo va mas rapido y es INCORRECTO en cuanto un juego reescriba codigo."),
    O("fetchfast", "KESTREL_NOFETCHFAST", "Fetch fast-path del interprete", "bool", True,
      invert=True,
      help="Memoiza la traduccion de la linea de I-cache de 32 B. Gana mucho en codigo "
           "mapeado por TLB."),
    O("jit_chain", "KESTREL_JIT_CHAIN", "Longitud maxima de cadena", "int", 0, adv=True,
      min=0, max=4096,
      help="0 = valor por defecto del emulador. Cuantos bloques enlazados se recorren antes "
           "de volver al despachador."),
    O("jit_stats", "KESTREL_JIT_STATS", "Estadisticas del JIT", "bool", False, adv=True,
      help="Cobertura, longitud media de bloque e histograma de por que se declina un bloque."),
    O("jit_trace", "KESTREL_JIT_TRACE", "Traza de compilacion", "bool", False, adv=True),
    O("jit_diff", "KESTREL_JIT_DIFF", "Diff contra el interprete", "bool", False, adv=True,
      help="Ejecuta cada bloque en JIT y en interprete y compara el estado. Lentisimo; "
           "caza bugs del JIT."),
    O("jit_brdiff", "KESTREL_JIT_BRDIFF", "Diff solo de saltos", "bool", False, adv=True),
    O("jit_nobranch", "KESTREL_JIT_NOBRANCH", "Sin absorcion de saltos", "bool", False, adv=True),
    O("jit_nojmp", "KESTREL_JIT_NOJMP", "Sin absorcion de J/JAL/JR", "bool", False, adv=True),
    O("jit_nofast", "KESTREL_JIT_NOFAST", "Sin prologo rapido", "bool", False, adv=True),
    O("cpuidle", "KESTREL_CPUIDLE", "Salto del bucle ocioso de la CPU", "bool", True,
      "El hilo ocioso de libultra es un salto a si mismo con NOP en la ranura de retardo: "
      "no observa nada mas que Count, asi que se cobra de golpe hasta el mismo instante en "
      "que la cadena del JIT habria vuelto a mirar los eventos. Apagarlo tiene que dar el "
      "mismo resultado, solo mas lento.", tri=True),
    O("lle_ipl3", "KESTREL_LLE_IPL3", "IPL3 real (LLE)", "bool", False, adv=True,
      help="Arranca ejecutando el IPL3 del cartucho en vez del arranque HLE."),
  ]),

  # ============================================================ RCP / hilos
  dict(id="rcp", label="RCP e hilos", icon="threads",
       desc="RSP, RDP y el reparto en hilos del anfitrion.", options=[
    O("threads", "KESTREL_THREADS", "RCP multihilo", "bool", True,
      "RDP y RSP en sus propios hilos del sistema. Apagado = lockstep determinista "
      "(el modo que valida los md5 de referencia).", tri=True),
    O("rspjit", "KESTREL_RSPJIT", "Recompilador del RSP", "bool", True,
      "Dynarec para el microcodigo del RSP.", tri=True),
    O("rspjit_stats", "KESTREL_RSPJIT_STATS", "Estadisticas del RSP JIT", "bool", False, adv=True),
    O("rspsse", "KESTREL_NORSPSSE", "VU por SSE", "bool", True, invert=True,
      help="Unidad vectorial del RSP con instrucciones SSE del anfitrion en vez de escalar."),
    O("vecfast", "KESTREL_NOVECFAST", "Cargas vectoriales rapidas", "bool", True, invert=True),
    O("rspidle", "KESTREL_RSPIDLE", "Aparcar el RSP en la espera del FIFO", "bool", True,
      "Con el motor del RDP drenado, el sondeo del microcodigo sobre DPC_CURRENT no puede "
      "cambiar de respuesta: se aparca el RSP hasta que la CPU archiva el siguiente tramo. "
      "Apagarlo tiene que dar el mismo resultado.", tri=True),
    O("dploglead", "KESTREL_DPLOGLEAD", "Adelanto de la CPU en la espera del FIFO", "choice", "auto",
      values=[["auto", "Automatico - puesto en velocidad libre, quitado en fiel a consola"],
              ["1", "Puesto - rapido, NO fiel"],
              ["0", "Quitado - fiel, multihilo == lockstep"]],
      help="Cuando el microcodigo sondea DPC_CURRENT/STATUS esperando al RDP, deja que la CPU "
           "se adelante un grano al RSP en vez de avanzar los dos clavados en el mismo instante. "
           "Perfect Dark en juego pasa de ~23 a ~35 fps, pero los eventos del RSP nacen tarde y "
           "el resultado depende del anfitrion: multihilo deja de coincidir con lockstep. Para "
           "comparar contra hardware o reproducir demos, modo de velocidad fiel a consola."),
    O("spinpause", "KESTREL_SPINPAUSE", "Pista PAUSE en las esperas activas", "bool", True,
      "La CPU y el RSP se vigilan girando sobre contadores que escribe el otro. PAUSE le dice "
      "al nucleo que eso es una espera, para que no le robe la linea de cache ni las ranuras "
      "de emision al hermano. Es solo una pista de anfitrion: el resultado sale identico.",
      tri=True, adv=True),
    O("barspin", "KESTREL_BARSPIN", "Vueltas de la barrera del SP", "int", 0, adv=True,
      min=0, max=1000000,
      help="0 = por defecto (16384). Cuanto gira la CPU en la barrera del RSP antes de dormir."),
    O("rdpspin", "KESTREL_RDPSPIN", "Vueltas del RDP ocioso", "int", 131072, adv=True,
      min=0, max=10000000,
      help="Cuanto gira el hilo del RDP, sin trabajo, antes de dormir. 0 = dormir enseguida. "
      "Ahorra despertarlo por el kernel en cada DPC_END: -13 % de tiempo con Parallel-RDP. "
      "De fabrica 131072 desde el re-barrido del 2026-09-18."),
    O("rspspin", "KESTREL_RSPSPIN", "Vueltas del RSP ocioso", "int", 0, adv=True,
      min=0, max=10000000,
      help="Cuanto gira el hilo del RSP entre tareas antes de dormir. Medido neutro; 0 = dormir enseguida."),
    O("dpspin", "KESTREL_DPSPIN", "Vueltas esperando al RDP", "int", 262144, adv=True,
      min=0, max=10000000,
      help="Cuanto gira la CPU (y el RSP) esperando a que el RDP cierre un tramo antes de dormir. 0 = dormir enseguida."),
    O("rspinline", "KESTREL_RSPINLINE", "RSP en linea", "bool", False, adv=True,
      help="Ejecuta la tarea del RSP dentro del hilo de CPU en vez de cederla al hilo del RCP."),
    O("rdpinline", "KESTREL_RDPINLINE", "RDP en linea", "bool", False, adv=True),
    O("rdpdrain", "KESTREL_RDPDRAIN", "Drenar RDP en cada sync", "bool", False, adv=True),
    O("paceslack", "KESTREL_PACESLACK", "Holgura del regulador RCP", "int", 0, adv=True,
      min=0, max=1000000,
      help="0 = por defecto (65536 con las barreras de invitado puestas, 4096 sin ellas). "
           "Cuanto puede adelantarse la CPU al RSP antes de esperar."),
    O("occ", "KESTREL_OCC", "Muestreo de solape (us)", "int", 0, adv=True,
      min=0, max=100000,
      help="0 = apagado. Periodo del muestreador de ocupacion de los hilos del RCP."),
  ]),

  # ============================================================ overclock
  dict(id="oc", label="Overclock", icon="bolt",
       desc="Multiplicadores por dominio de reloj. 1.00x = consola real. Subir el de CPU "
            "quita ralentizacion, pero cambia cuanto trabajo hace el juego entre campos de "
            "video: hay juegos que atan su logica al reloj y se rompen.",
       options=[
    O("speedmode", "KESTREL_SPEEDMODE", "Modo de velocidad", "choice", "libre",
      values=[["libre", "Libre - manda lo que se elija aqui debajo"],
              ["hw", "Fiel a consola - relojes N64 exactos y 59.94 campos/s"]],
      help="Fiel a consola ignora los multiplicadores y el CPI puesto a mano, y con ventana "
           "clava el campo de video a 59.94 Hz: el juego ve el mismo tiempo que veria en la "
           "maquina real. Sin ventana no limita el ritmo de pared, porque no hay pantalla que "
           "respetar y el resultado no cambia. Es lo que hay que poner para las demos que "
           "cuentan campos (la liana de Donkey Kong 64) y para comparar contra hardware."),
    O("oc_link", None, "Ligar los tres dominios", "bool", True,
      "Un solo mando para CPU, RSP y RDRAM."),
    O("oc_all", "KESTREL_OC", "Multiplicador global", "float", 1.0,
      min=0.25, max=8.0, step=0.05, presets=[0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0]),
    O("oc_cpu", "KESTREL_OC_CPU", "CPU - R4300i 93.75 MHz", "float", 1.0,
      min=0.25, max=8.0, step=0.05, presets=[0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0]),
    O("oc_rsp", "KESTREL_OC_RSP", "RSP - 62.5 MHz", "float", 1.0,
      min=0.25, max=8.0, step=0.05, presets=[0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0]),
    O("oc_rdram", "KESTREL_OC_RDRAM", "RDRAM - 250 MHz DDR", "float", 1.0,
      min=0.25, max=8.0, step=0.05, presets=[0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0],
      help="Solo mueve el modelo de ancho de banda; no acelera la logica del juego."),
    O("throttle", "KESTREL_THROTTLE", "Limitar a tiempo real", "choice", "auto",
      values=[["auto", "Automatico - limita si hay ventana"],
              ["1", "Siempre limitado a 59.94 campos/s"],
              ["0", "Sin limite - a tope"]],
      help="El ancla es el campo de video, no el reloj de CPU: con overclock el juego hace "
           "mas trabajo por campo, pero los campos siguen saliendo a 59.94 Hz."),
    O("viticks", "KESTREL_VITICKS", "Subdivisiones del campo de video", "int", 16, adv=True,
      min=1, max=256,
      help="Con cuanta precision cae la interrupcion del VI dentro del campo. Subirlo afina "
           "los efectos de rastreo y cuesta vueltas de bucle."),
  ]),

  # ============================================================ video
  dict(id="video", label="Video", icon="display",
       desc="Rasterizador, ventana y presentacion.", options=[
    O("plugin", None, "Rasterizador (plugin grafico)", "choice", "auto",
      values=[["auto", "Automatico - la compilacion disponible"],
              ["soft", "SoftRDP - rasterizador propio en CPU, determinista"],
              ["prdp", "paraLLEl-RDP - LLE por Vulkan en GPU"]],
      help="paraLLEl-RDP necesita una compilacion con KESTREL_PRDP=ON. El lanzador elige el "
           "ejecutable adecuado y activa la variable."),
    O("video", "KESTREL_VIDEO", "Abrir ventana", "bool", True,
      "Apagado = sin ventana (modo lote / captura)."),
    O("winscale", "KESTREL_WINSCALE", "Escala de ventana", "choice", "2",
      values=[["1", "1x - 320x240"], ["2", "2x - 640x480"], ["3", "3x - 960x720"],
              ["4", "4x - 1280x960"], ["5", "5x - 1600x1200"], ["6", "6x - 1920x1440"],
              ["8", "8x - 2560x1920"]]),
    O("winsize", "KESTREL_WINSIZE", "Tamano exacto WxH", "text", "",
      help="Vacio = usar la escala. Ejemplo: 1600x900. Manda sobre la escala."),
    O("fullscreen", "KESTREL_FULLSCREEN", "Pantalla completa", "bool", False,
      "Usa el modo actual del monitor primario; no cambia la resolucion del escritorio.", tri=True),
    O("aspect", "KESTREL_ASPECT", "Relacion de aspecto", "choice", "4:3",
      values=[["4:3", "4:3 - la senal que saca el VI (fiel)"],
              ["16:9", "16:9 - estirar la imagen anamorfica"],
              ["estirar", "Llenar la ventana (deforma)"]],
      help="El N64 saca SIEMPRE 4:3. 16:9 no ensancha el campo de vision -- eso solo lo "
           "puede hacer el juego -- sino que estira la imagen que ya generan aplastada los "
           "juegos con modo panoramico propio (Perfect Dark, GoldenEye, Turok, Rush 2)."),
    O("upscale", "KESTREL_UPSCALE", "Escalado interno (paraLLEl-RDP)", "choice", "1",
      values=[["1", "1x - resolucion nativa"], ["2", "2x"], ["4", "4x"], ["8", "8x"]],
      help="Rasteriza a N veces la resolucion del N64 dentro de la GPU. Solo con "
           "paraLLEl-RDP; el SoftRDP va siempre a 1x. Lo que el juego lee de su propio "
           "framebuffer sigue siendo 1x, asi que no rompe los efectos que releen la imagen."),
    O("ssaa", "KESTREL_SSAA", "Supermuestreo al volcar a 1x", "bool", False, adv=True,
      help="Con escalado interno, al devolver la imagen ampliada al framebuffer del juego "
           "promedia las NxN muestras en vez de coger una. Antialiasing gratis en los "
           "efectos que releen el framebuffer, a cambio de una pasada mas."),
    O("hud", "KESTREL_HUD_OFF", "HUD de telemetria sobre la imagen", "bool", True, invert=True),
    O("noaa", "KESTREL_NOAA", "Antialiasing del RDP", "bool", True, invert=True,
      help="SoftRDP. Apagarlo sube el relleno y cambia el borde de los poligonos."),
    O("nofilter", "KESTREL_NOFILTER", "Filtrado de texturas", "bool", True, invert=True),
    O("noblend", "KESTREL_NOBLEND", "Mezclador (blender)", "bool", True, invert=True),
    O("noraster", "KESTREL_NORASTER", "Rasterizado", "bool", True, invert=True, adv=True,
      help="Apagado no dibuja nada: sirve para medir cuanto cuesta el rasterizador."),
    O("prdp_stats", "KESTREL_PRDP_STATS", "Estadisticas de paraLLEl-RDP", "bool", False, adv=True),
    O("video_test", "KESTREL_VIDEO_TEST", "Patron de prueba", "bool", False, adv=True),
    O("video_dump", "KESTREL_VIDEO_DUMP", "Volcar cada campo a BMP", "path", "", adv=True),
  ]),

  # ============================================================ audio
  dict(id="audio", label="Audio", icon="audio", desc="Salida de sonido del anfitrion.",
       options=[
    O("audio", "KESTREL_AUDIO", "Audio activado", "bool", True, tri=True),
    O("volume", "KESTREL_VOLUME", "Volumen (%)", "int", 100, min=0, max=100,
      help="Atenuacion aplicada a las muestras antes de mandarlas al anfitrion. No toca el modelo del AI: el juego sigue viendo el mismo audio."),
    O("audio_trace", "KESTREL_AUDIO_TRACE", "Traza de audio", "bool", False, adv=True),
    O("audiohook", "KESTREL_AUDIOHOOK", "Enganche de audio", "bool", False, adv=True),
  ]),

  # ============================================================ guardado
  dict(id="save", label="Cartucho", icon="cart",
       desc="Dispositivo de guardado del cartucho.", options=[
    O("savetype", "KESTREL_SAVETYPE", "Tipo de guardado", "choice", "auto",
      values=[["auto", "Automatico - por ID de cartucho"],
              ["none", "Ninguno"],
              ["eeprom4k", "EEPROM 4 kbit"], ["eeprom16k", "EEPROM 16 kbit"],
              ["sram256k", "SRAM 256 kbit"], ["sram768k", "SRAM 768 kbit"],
              ["flash1m", "FlashRAM 1 Mbit"]],
      help="Se resuelve por ID de cartucho; esto lo fuerza cuando la ROM no esta en la tabla."),
    O("tvtype", "KESTREL_TVTYPE", "Norma de television", "choice", "auto",
      values=[["auto", "Automatico - por region del cartucho"],
              ["ntsc", "NTSC (60 Hz)"], ["pal", "PAL (50 Hz)"], ["mpal", "PAL-M (60 Hz)"]],
      help="Lo que el juego lee en osTvType, y de donde sale el ritmo de campo. En la consola "
           "de verdad lo fija la maquina, y la region del cartucho coincide con ella; algunos "
           "juegos se niegan a funcionar con la norma equivocada."),
    O("rdram", "KESTREL_RDRAM", "Memoria RDRAM", "choice", "8",
      values=[["8", "8 MB - con Expansion Pak"], ["4", "4 MB - consola de serie"]],
      help="La N64 trae 4 MB y el Expansion Pak la sube a 8. Los juegos lo leen en "
           "osMemSize y algunos cambian de comportamiento: reservan menos buferes o "
           "bajan la resolucion con 4 MB, y Donkey Kong 64 y el modo de un jugador de "
           "Perfect Dark EXIGEN los 8. La RDRAM se dimensiona una sola vez, asi que "
           "cambiarlo pide relanzar, y un estado guardado con un tamano no se puede "
           "cargar con el otro."),
    O("cheats", "KESTREL_CHEATS", "Fichero de trucos (.cht)", "path", "",
      help="Codigos tipo GameShark, aplicados en cada campo de video igual que el cartucho "
           "de verdad. Sin fichero se usa el .cht que haya al lado de la ROM con su mismo "
           "nombre. Formato: [Nombre] abre un truco ([-Nombre] lo deja apagado) y debajo van "
           "las lineas AAAAAAAA VVVV tal como se publican."),
  ]),

  # ============================================================ peliculas (TAS)
  dict(id="movie", label="Peliculas", icon="cart",
       desc="Grabacion y reproduccion de entradas (.k64m). Lo que se graba no es lo que "
            "aprieta el jugador sino lo que el JUEGO LEE en cada lectura del joybus, que es "
            "la unica frontera que el invitado percibe.", options=[
    O("movie_rec", "KESTREL_MOVIE_REC", "Grabar entradas en", "path", "",
      help="Fichero .k64m donde apuntar cada lectura de botones. Se graba desde el arranque "
           "en frio: una repeticion vale desde el encendido, no desde media partida."),
    O("movie_play", "KESTREL_MOVIE_PLAY", "Reproducir entradas de", "path", "",
      help="Sustituye el mando por el de la pelicula. Se comprueban los CRC del cartucho y "
           "no se reproduce una pelicula de otro juego. Al acabar la cinta vuelve a mandar "
           "el mando del anfitrion. Con las dos casillas puestas manda esta."),
  ]),

  # ============================================================ rebobinado
  dict(id="rewind", label="Rebobinado", icon="cart",
       desc="Deshacer lo que acaba de pasar. Cuesta CPU y memoria: cada foto obliga a parar "
            "el RCP y a recorrer el estado entero, asi que viene apagado y solo se paga si "
            "se enciende.", options=[
    O("rewind", "KESTREL_REWIND", "Activar rebobinado", "bool", False,
      "Con la tecla de retroceso el juego va hacia atras mientras se mantenga apretada. "
      "Apagado no cuesta nada; encendido, el emulador fotografia la maquina cada pocos "
      "campos (parando el RDP y el RSP en cada foto) y guarda solo las diferencias."),
    O("rewind_fields", "KESTREL_REWIND_FIELDS", "Campos entre fotos", "int", 2, min=1, max=60,
      help="Cada cuantos campos de video se toma una foto. Menos = rebobinado mas fino y "
           "mas caro; mas = mas barato y a saltos mas gordos. 2 son unas 30 fotos por "
           "segundo, que es el paso con el que se juega."),
    O("rewind_mb", "KESTREL_REWIND_MB", "Memoria para la cinta (MB)", "int", 256, min=8, max=4096,
      help="Tope de memoria de las diferencias. Al llenarse se tira el pasado LEJANO, que "
           "es lo que no se va a pedir. Cuanto dura depende del juego: lo que ocupa una "
           "foto es lo que el juego cambia entre foto y foto."),
  ]),

  # ============================================================ telemetria / MCP
  dict(id="mcp", label="Telemetria y MCP", icon="probe",
       desc="Servidor TCP+JSON propio del emulador. Es la via por la que el MCP lee "
            "registros, memoria, framebuffer y perfiles.", options=[
    O("port", None, "Puerto del servidor", "int", 9128, min=1, max=65535),
    O("paused", None, "Arrancar en pausa", "bool", False,
      "Arranca detenido para poder poner puntos de ruptura antes de la primera instruccion. "
      "Sin esto el emulador arranca en marcha libre (--run)."),
    O("heartbeat", "KESTREL_HEARTBEAT", "Latido en consola", "bool", False,
      "Imprime velocidad, MIPS y campos cada poco."),
    O("hostprof", "KESTREL_HOSTPROF", "Perfilador del anfitrion", "bool", False, adv=True,
      help="Muestrea donde gasta el tiempo el proceso del emulador, no el juego."),
    O("hostprof_out", "KESTREL_HOSTPROF_OUT", "Fichero de salida del perfilador", "path", "",
      adv=True),
    O("hostprof_who", "KESTREL_HOSTPROF_WHO", "Hilos a perfilar", "text", "", adv=True),
  ]),

  # ============================================================ limites / lote
  dict(id="batch", label="Limites y lote", icon="clock",
       desc="Topes de ejecucion. Imprescindibles para no dejar nunca un proceso colgado.",
       options=[
    O("maxinsn", "KESTREL_MAXINSN", "Tope de instrucciones", "int", 0,
      min=0, max=99999999999, help="0 = sin tope. El emulador termina al llegar."),
    O("maxflips", "KESTREL_MAXFLIPS", "Tope de intercambios de framebuffer", "int", 0,
      min=0, max=1000000),
    O("maxsyncs", "KESTREL_MAXSYNCS", "Tope de DP full-sync", "int", 0, min=0, max=1000000,
      adv=True),
    O("stable", "KESTREL_STABLE", "Parar cuando la imagen se estabilice", "int", 0,
      min=0, max=100000, adv=True),
    O("fbdump", "KESTREL_FBDUMP", "Volcar framebuffer al salir", "path", "", adv=True),
    O("fielddump", "KESTREL_FIELDDUMP", "Volcar campos", "path", "", adv=True),
    O("fieldhash", "KESTREL_FIELDHASH", "Hash por campo", "bool", False, adv=True),
    O("watchdog", "KESTREL_WATCHDOG", "Perro guardian (s)", "int", 0, min=0, max=100000,
      adv=True, help="Mata el proceso si no progresa. 0 = apagado."),
    O("hangdog", "KESTREL_HANGDOG", "Detector de cuelgue", "bool", False, adv=True),
  ]),

  # ============================================================ depuracion
  dict(id="debug", label="Depuracion", icon="bug",
       desc="Trazas y trampas. Todas cuestan rendimiento; ninguna esta activa de fabrica.",
       options=[
    O("bp", "KESTREL_BP", "Punto de ruptura (PC hex)", "hex", ""),
    O("bptrace", "KESTREL_BPTRACE", "Traza en el punto de ruptura", "bool", False),
    O("watch", "KESTREL_WATCH", "Punto de vigilancia de escritura (addr)", "hex", ""),
    O("watchlen", "KESTREL_WATCHLEN", "Longitud vigilada (bytes)", "int", 0, min=0, max=1048576),
    O("watchp", "KESTREL_WATCHP", "Vigilancia por D-cache", "bool", False, adv=True),
    O("dis", "KESTREL_DIS", "Desensamblado", "bool", False, adv=True),
    O("exctrace", "KESTREL_EXCTRACE", "Traza de excepciones", "bool", False),
    O("exctail", "KESTREL_EXCTAIL", "Cola de excepciones", "bool", False, adv=True),
    O("faulttrace", "KESTREL_FAULTTRACE", "Traza de fallos", "bool", False, adv=True),
    O("faultstop", "KESTREL_FAULTSTOP", "Parar en el primer fallo", "bool", False, adv=True),
    O("intlog", "KESTREL_INTLOG", "Registro de interrupciones", "bool", False, adv=True),
    O("irqtrace", "KESTREL_IRQTRACE", "Traza de IRQ del MI", "bool", False, adv=True),
    O("noint", "KESTREL_NOINT", "Sin interrupciones", "bool", False, adv=True),
    O("vilog", "KESTREL_VILOG", "Registro del VI", "bool", False, adv=True),
    O("silog", "KESTREL_SILOG", "Registro del SI (joybus)", "bool", False, adv=True),
    O("rsptrace", "KESTREL_RSPTRACE", "Traza del RSP", "bool", False, adv=True),
    O("rdptrace", "KESTREL_RDPTRACE", "Traza del RDP", "bool", False, adv=True),
    O("rdpops", "KESTREL_RDPOPS", "Contador de ordenes del RDP", "bool", False, adv=True),
    O("citrace", "KESTREL_CITRACE", "Traza de imagen de color", "bool", False, adv=True),
    O("tridbg", "KESTREL_TRIDBG", "Depuracion de triangulos", "bool", False, adv=True),
    O("dpsynclog", "KESTREL_DPSYNCLOG", "Registro de DP sync", "bool", False, adv=True),
    O("vrdplog", "KESTREL_VRDPLOG", "Registro de paraLLEl-RDP", "bool", False, adv=True),
    O("evlog", "KESTREL_EVLOG", "Registro de eventos del bus", "bool", False, adv=True),
    O("pcring", "KESTREL_PCRING", "Anillo de PCs", "bool", False, adv=True),
    O("pcsample", "KESTREL_PCSAMPLE", "Muestreo de PC", "int", 0, min=0, max=100000000, adv=True),
    O("guestthreads", "KESTREL_GUESTTHREADS", "Hilos del guest (libultra)", "bool", False,
      adv=True),
    O("threadscan", "KESTREL_THREADSCAN", "Barrido de hilos del guest", "bool", False, adv=True),
    O("qchk", "KESTREL_QCHK", "Invariante de cola del kernel (addr)", "hex", "", adv=True),
    O("qchkn", "KESTREL_QCHKN", "Periodo de comprobacion (insn)", "int", 0, min=0,
      max=100000000, adv=True),
    O("trapzero", "KESTREL_TRAPZERO", "Trampa: escritura a r0", "bool", False, adv=True),
    O("trapwild", "KESTREL_TRAPWILD", "Trampa: acceso salvaje", "bool", False, adv=True),
    O("trapri", "KESTREL_TRAPRI", "Trampa: registros RI", "bool", False, adv=True),
    O("trapspreg", "KESTREL_TRAPSPREG", "Trampa: registros SP", "bool", False, adv=True),
    O("halt_unimpl", "KESTREL_HALT_UNIMPL", "Parar en instruccion no implementada", "bool",
      False, adv=True),
    O("fporacle", "KESTREL_FPORACLE", "Oraculo de coma flotante", "bool", False, adv=True),
    O("fptrace", "KESTREL_FPTRACE", "Traza de FPU", "bool", False, adv=True),
    O("vustat", "KESTREL_VUSTAT", "Estadisticas de la VU", "bool", False, adv=True),
    O("dcwt", "KESTREL_DCWT", "Traza de write-through de D-cache", "bool", False, adv=True),
  ]),
]


# ---------------------------------------------------------------- mando N64
# Nombres de boton tal y como los ve el emulador (bits del joybus, ver Memory::padButtons)
# y el nombre GLFW por defecto que hoy tiene cableado present.cpp. El lanzador escribe un
# fichero de mapeo que lee el emulador; estos son los valores de fabrica.
PAD_BUTTONS = [
  dict(id="A",      label="A",         bit=0x8000, key="X",     gp="A",           group="face"),
  dict(id="B",      label="B",         bit=0x4000, key="C",     gp="B",           group="face"),
  dict(id="Z",      label="Z",         bit=0x2000, key="SPACE", gp="LEFT_TRIGGER", group="trigger"),
  dict(id="START",  label="Start",     bit=0x1000, key="ENTER", gp="START",       group="face"),
  dict(id="DU",     label="Cruz arriba",   bit=0x0800, key="UP",    gp="DPAD_UP",    group="dpad"),
  dict(id="DD",     label="Cruz abajo",    bit=0x0400, key="DOWN",  gp="DPAD_DOWN",  group="dpad"),
  dict(id="DL",     label="Cruz izquierda", bit=0x0200, key="LEFT", gp="DPAD_LEFT",  group="dpad"),
  dict(id="DR",     label="Cruz derecha",  bit=0x0100, key="RIGHT", gp="DPAD_RIGHT", group="dpad"),
  dict(id="L",      label="L",         bit=0x0020, key="Q",     gp="LEFT_BUMPER",  group="trigger"),
  dict(id="R",      label="R",         bit=0x0010, key="E",     gp="RIGHT_BUMPER", group="trigger"),
  dict(id="CU",     label="C arriba",  bit=0x0008, key="I",     gp="",            group="c"),
  dict(id="CD",     label="C abajo",   bit=0x0004, key="K",     gp="",            group="c"),
  dict(id="CL",     label="C izquierda", bit=0x0002, key="J",   gp="",            group="c"),
  dict(id="CR",     label="C derecha", bit=0x0001, key="L",     gp="",            group="c"),
]

PAD_AXES = [
  dict(id="SX+", label="Stick derecha",  key="D", group="stick"),
  dict(id="SX-", label="Stick izquierda", key="A", group="stick"),
  dict(id="SY+", label="Stick arriba",   key="W", group="stick"),
  dict(id="SY-", label="Stick abajo",    key="S", group="stick"),
]


def flat():
    """Todas las opciones en un dict id -> registro (con la categoria dentro)."""
    out = {}
    for c in CATEGORIES:
        for o in c["options"]:
            r = dict(o)
            r["cat"] = c["id"]
            out[o["id"]] = r
    return out


def defaults():
    d = {o["id"]: o["default"] for o in flat().values()}
    d["pad"] = {b["id"]: dict(key=b["key"], gp=b["gp"]) for b in PAD_BUTTONS}
    d["pad"].update({a["id"]: dict(key=a["key"], gp="") for a in PAD_AXES})
    return d


def to_env(profile):
    """Perfil (dict id->valor) -> (env dict, argv extra).

    Reglas: un bool normal exporta "1" cuando esta encendido y no exporta nada cuando no.
    Un bool `invert` (KESTREL_NOxxx) exporta "1" cuando el control esta APAGADO. Los
    numericos y textos solo se exportan si difieren del valor de fabrica y no estan vacios.
    """
    env, argv = {}, []
    f = flat()
    dfl = {k: v["default"] for k, v in f.items()}
    p = dict(dfl)
    p.update(profile or {})

    for oid, spec in f.items():
        envname = spec.get("env")
        if not envname:
            continue
        val = p.get(oid, spec["default"])
        t = spec["type"]
        if t == "bool":
            on = bool(val)
            if spec.get("invert"):
                # KESTREL_NOxxx: la variable DESACTIVA. Se exporta cuando el control esta
                # apagado, y nunca cuando esta encendido (presencia = desactivar).
                if not on:
                    env[envname] = "1"
            elif spec.get("tri"):
                # Lee el VALOR (envFlag o v[0]=='0'): se puede escribir explicito en ambos
                # sentidos, y asi el perfil manda aunque cambie el default del emulador.
                env[envname] = "1" if on else "0"
            elif on:
                # El resto se activan por PRESENCIA. Exportar "0" las ENCENDERIA, asi que
                # apagado = no exportar nada.
                env[envname] = "1"
        elif t in ("int",):
            n = int(val or 0)
            if n != int(spec["default"] or 0):
                env[envname] = str(n)
        elif t in ("float",):
            x = float(val if val not in (None, "") else spec["default"])
            if abs(x - float(spec["default"])) > 1e-9:
                env[envname] = ("%.4f" % x).rstrip("0").rstrip(".")
        elif t == "choice":
            s = str(val)
            if s and s != "auto" and s != str(spec["default"]):
                env[envname] = s
            elif s and s != "auto" and s == str(spec["default"]):
                env[envname] = s
        else:  # text / path / hex
            s = str(val or "").strip()
            if s:
                env[envname] = s

    # El overclock ligado usa un solo multiplicador global; desligado, los tres sueltos.
    if p.get("oc_link", True):
        for k in ("KESTREL_OC_CPU", "KESTREL_OC_RSP", "KESTREL_OC_RDRAM"):
            env.pop(k, None)
    else:
        env.pop("KESTREL_OC", None)

    # "auto" en el limitador significa "no digas nada y deja decidir al emulador".
    if str(p.get("throttle", "auto")) == "auto":
        env.pop("KESTREL_THROTTLE", None)

    # Fiel a consola: ni multiplicadores ni CPI a mano viajan en el entorno. El emulador ya
    # los ignora en este modo, pero dejarlos puestos haria creer que siguen valiendo.
    if str(p.get("speedmode", "libre")) == "hw":
        for k in ("KESTREL_OC", "KESTREL_OC_CPU", "KESTREL_OC_RSP", "KESTREL_OC_RDRAM",
                  "KESTREL_CPI"):
            env.pop(k, None)
        env["KESTREL_THROTTLE"] = "1"

    # El plugin grafico decide EJECUTABLE (compilacion), no solo variable. "auto" no dice
    # nada y deja el defecto del binario, que es la GPU cuando el backend esta compilado;
    # "soft" tiene que decir "0" EXPLICITO, porque quitar la variable ya no apaga nada.
    if str(p.get("plugin", "auto")) == "prdp":
        env["KESTREL_PRDP"] = "1"
    elif str(p.get("plugin", "auto")) == "soft":
        env["KESTREL_PRDP"] = "0"

    # KESTREL_VIDEO / --run: sin --run el emulador arranca en pausa esperando al MCP.
    if not p.get("paused", False):
        argv.append("--run")
        # --run implica lote (sin ventana) salvo que se pida ventana explicitamente.
        if p.get("video", True):
            env["KESTREL_VIDEO"] = "1"
        else:
            env.pop("KESTREL_VIDEO", None)
            env["KESTREL_NOVIDEO"] = "1"
    else:
        if not p.get("video", True):
            env["KESTREL_NOVIDEO"] = "1"

    port = int(p.get("port", 9128) or 9128)
    argv += ["--port", str(port)]
    return env, argv
