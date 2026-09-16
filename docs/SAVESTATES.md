# Estado guardado (`src/core/savestate.{hpp,cpp}`)

Foto completa de la maquina: CPU (con TLB y las dos caches), RCP (MI/SP/DPC/VI/AI/PI/RI/SI),
el nucleo del RSP entero, el estado de pipeline del RDP, RDRAM, DMEM/IMEM, PIF RAM y el medio
de guardado del cartucho. Ranuras `0..9` junto a la ROM: `<rom>.st0` ... `<rom>.st9`.

Teclas (ventana): **F5** guarda, **F7** carga, **F6** cambia de ranura.
Telemetria/MCP: `state.save {slot}` y `state.load {slot}`.

## Una sola descripcion, recorrida en dos direcciones

`StateIO` lleva una bandera `writing`; todo lo demas (`visitCpu`, `StateVisitor::rsp`, ...)
esta escrito **una vez** y sirve para guardar y para cargar. Con dos funciones separadas,
cualquier campo anadido a una y olvidado en la otra corrompe el estado en silencio y solo
asoma como un cuelgue raro tres partidas despues.

La lectura se valida sobre la marcha: marcas de seccion de 4 bytes (`CPU `, `RSP `, `RAM `,
`END `) y longitudes delante de cada bloque grande. Cualquier desajuste pone `bad` y aborta
la carga entera en vez de repartir bytes desplazados por toda la maquina. La cabecera lleva
magia, version, CRC1/CRC2 de la ROM y el tamano de RDRAM: un estado de OTRO juego se rechaza
antes de tocar nada.

La version va por **5**. La 5 anadio la seccion `MOVI`: el sondeo por el que va la pelicula
de entradas, para que cargar un estado rebobine la cinta con el juego (ver `docs/TAS.md`).
El campo va aunque no haya pelicula -- el formato no puede depender de una variable de
entorno. Los estados de la 4 no se cargan.

## Por que se guardan las caches

La VR4300 **no tiene coherencia de cache**. Una linea sucia de D-cache es un dato que solo
existe ahi: no esta en RDRAM y no llegara hasta que la desalojen. Sin las caches en el
fichero, cargar un estado perderia esas escrituras. Se guardan las dos: D-cache (512 lineas
de 16 B, con `tagv`/`dirty`) e I-cache (512 de 32 B, con su sello de relleno).

## Por que se toma con el RCP quieto

La peticion no la atiende quien pulsa la tecla. `Presenter` deja el numero de ranura en un
buzon (`System::stateSaveReq` / `stateLoadReq`) y lo atiende `System::serviceStateReq()`, al
principio de una vuelta del bucle de ejecucion **y tambien en pausa**, que es cuando mas se
guarda. Ahi, y solo ahi, se puede aquietar el RCP:

- `rdpDrain()` — con comandos a medias, el estado guardaria un FIFO que apunta a una lista
  de visualizacion que ya no existe.
- `rspAwaitIdle()` y, en Lockstep, terminar la tarea del RSP que lleva este mismo hilo. El
  microcodigo se guarda con el nucleo **parado a proposito**: en modo hilos una tarea a
  medias vive en el worker y nadie la reanudaria tras cargar. Terminandola, el mismo fichero
  vale en Lockstep y en Threaded — comprobado en `test/state_test.py`, fase [5].

## Al cargar

`afterLoad` deja el emulador coherente despues de reescribirle la memoria por debajo:

- vacia los **dos** dynarecs (el codigo compilado describe la RDRAM/IMEM que habia, no la que
  hay: un bloque superviviente ejecutaria instrucciones del estado anterior);
- invalida las memoizaciones de traduccion (`jitTlbValid`, la linea de fetch, `bumpXlat`),
  porque el TLB y el modo de la CPU acaban de cambiar de golpe;
- pone a cero la contabilidad diferida de la cadena de bloques enlazados;
- vuelve a atar los punteros del RSP a DMEM/IMEM;
- rederiva el bit de modo repeticion de MI en la guardia de store del dynarec;
- marca el medio de guardado como sucio para que llegue al disco.

El medio de guardado (EEPROM/SRAM/FlashRAM, con el estado de la maquina de FlashRAM) va
**dentro** del fichero. Si no, cargar una partida vieja dejaria la EEPROM con el contenido de
la nueva y el juego veria un fichero de guardado del futuro.

## Lo que NO se guarda

- Estado interno de **parallel-rdp** (su cola de comandos en GPU). No hace falta: lo que ve
  el juego son los pixeles en RDRAM, y eso si va. El estado se toma con el RDP drenado, asi
  que no hay trabajo en vuelo que perder. Los tiles/TMEM del RDP por software si se guardan,
  y ademas los juegos los reprograman al principio de cada lista.
- Codigo compilado por los dynarecs — se recompila solo.
- Contadores de telemetria del host (perfilador, ventanas de velocidad).

## Prueba

`python test/state_test.py <rom>` (por defecto SM64). Levanta el emulador, corre 40
intercambios de buffer, guarda, deja avanzar otros 40, carga, y compara **todo** lo
observable por telemetria — registros de CPU, RCP y RSP (con los vectoriales), cuatro trozos
de 64 KB de RDRAM y el md5 del framebuffer — contra la foto tomada al guardar. Despues
reanuda y comprueba que el VI vuelve a intercambiar buffer: un estado que restaura los bytes
pero deja el RCP muerto pasaria la primera mitad. La fase [5] repite la carga en el otro modo
de RCP.

Pasa con interprete y con `KESTREL_JIT=1`.

    [1] guardado en ...st9 (8.4 MB)
    [2] la maquina avanzo (el estado difiere)
    [3] estado restaurado identico (CPU+RCP+RSP+RDRAM+framebuffer)
    [4] sigue corriendo tras cargar
    [5] estado de Threaded cargado en Lockstep: identico y corriendo
    ALL PASS
