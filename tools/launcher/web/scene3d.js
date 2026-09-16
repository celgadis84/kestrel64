/* scene3d.js -- pega la geometria de `models.js` a la interfaz del lanzador.
 *
 * Dos usos:
 *   PAD3D   mando de verdad en el cajon de mapeo: se pincha un boton del modelo y se abre
 *           su ficha; el boton que se pulsa en un gamepad real se enciende en vivo.
 *   SHELF3D estante: la caja de carton y el cartucho del juego seleccionado, con la
 *           caratula puesta de textura sobre la cara frontal.
 *   CAROUSEL la biblioteca entera en 3D: una malla con K cajas y una matriz por caja,
 *           con tres colocaciones (anillo, coverflow y pared) que comparten geometria.
 *
 * Si el navegador no da WebGL, `mount` devuelve null y quien llama se queda con lo de
 * antes (el mando de CSS). Nada aqui es obligatorio para que el lanzador funcione.
 */
"use strict";

const SCENE3D = (() => {

// En el navegador GL y MODELS son globales; bajo node llegan por require (las pruebas).
const G3 = (typeof GL !== "undefined") ? GL : require("./gl.js");
const M4 = G3.M4;

// Orbita con el raton: yaw libre, cabeceo limitado para no pasar por los polos.
function orbit(canvas, sc, st, redraw) {
  let drag = null;
  const apply = () => {
    const d = st.dist, cp = Math.cos(st.pitch);
    sc.eye = [st.tx + Math.sin(st.yaw) * cp * d, st.ty + Math.sin(st.pitch) * d,
              st.tz + Math.cos(st.yaw) * cp * d];
    sc.target = [st.tx, st.ty, st.tz];
  };
  canvas.addEventListener("pointerdown", e => {
    drag = { x: e.clientX, y: e.clientY, yaw: st.yaw, pitch: st.pitch, moved: 0 };
    canvas.setPointerCapture(e.pointerId);
  });
  canvas.addEventListener("pointermove", e => {
    if (!drag) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    drag.moved = Math.max(drag.moved, Math.abs(dx) + Math.abs(dy));
    st.yaw = drag.yaw - dx * 0.008;
    st.pitch = Math.max(-1.2, Math.min(1.35, drag.pitch + dy * 0.006));
    apply(); redraw();
  });
  const end = e => {
    const m = drag ? drag.moved : 99;
    drag = null;
    try { canvas.releasePointerCapture(e.pointerId); } catch (_) {}
    return m;
  };
  canvas.addEventListener("pointerup", e => { canvas._lastMove = end(e); });
  canvas.addEventListener("pointercancel", end);
  canvas.addEventListener("wheel", e => {
    e.preventDefault();
    st.dist = Math.max(st.min, Math.min(st.max, st.dist * (e.deltaY > 0 ? 1.1 : 0.91)));
    apply(); redraw();
  }, { passive: false });
  apply();
  return { apply, reset: (y, p) => { st.yaw = y; st.pitch = p; apply(); redraw(); } };
}

/* Bucle de dibujo perezoso: se redibuja cuando algo cambia, y ademas por cuadro solo
   mientras haya una animacion viva (el brillo de un boton subiendo o bajando). */
function pump(sc) {
  let pending = false, anim = false;
  const frame = () => {
    pending = false;
    sc.draw(null);
    if (anim) { pending = true; requestAnimationFrame(frame); }
  };
  return {
    redraw() { if (!pending) { pending = true; requestAnimationFrame(frame); } },
    setAnim(on) { anim = on; if (on && !pending) { pending = true; requestAnimationFrame(frame); } },
  };
}

/* --------------------------------------------------------------------- el mando */
// opts.onPick(id)   -> el usuario ha pinchado un control
// opts.pressed()    -> array de ids pulsados ahora mismo en un gamepad real (opcional)
function mountPad(host, opts) {
  opts = opts || {};
  let sc;
  const canvas = document.createElement("canvas");
  canvas.className = "gl3d";
  host.appendChild(canvas);
  try { sc = new GL.Scene(canvas); } catch (e) { host.removeChild(canvas); return null; }

  canvas._scene = sc;                    // la pagina de pruebas necesita la camara para proyectar
  sc.setMesh(MODELS.buildController());
  sc.gloss = 0.85;
  const st = { yaw: 0, pitch: 0.42, dist: 26, min: 14, max: 46, tx: 0, ty: -0.2, tz: 0 };
  const p = pump(sc);
  const cam = orbit(canvas, sc, st, p.redraw);

  let sel = null, hover = null;
  const press = {};                      // id -> 0..1, sube al pulsar y baja al soltar
  const target = {};                     // id -> nivel al que tiende el brillo

  const recompute = () => {
    sc.highlight = {};
    for (const k in press) if (press[k] > 0.01) sc.highlight[k] = press[k] * 0.9;
    if (hover && hover !== sel) sc.highlight[hover] = Math.max(sc.highlight[hover] || 0, 0.32);
    if (sel) sc.highlight[sel] = 1;
  };

  // Suavizado del encendido: sin esto un boton pulsado un solo cuadro no se llega a ver.
  let ticking = false;
  const tick = () => {
    let alive = false;
    for (const k in target) {
      const t = target[k], v = press[k] || 0;
      const nv = v + (t - v) * 0.28;
      press[k] = Math.abs(nv - t) < 0.01 ? t : nv;
      if (press[k] !== t) alive = true;
      if (press[k] < 0.01 && t === 0) { delete press[k]; delete target[k]; }
    }
    recompute();
    p.setAnim(alive);
    if (!alive) ticking = false;
    else requestAnimationFrame(tick);
  };
  const wake = () => { if (!ticking) { ticking = true; requestAnimationFrame(tick); } };

  canvas.addEventListener("pointermove", e => {
    const h = sc.pick(e.clientX, e.clientY);
    if (h !== hover) { hover = h; canvas.style.cursor = h ? "pointer" : "grab"; recompute(); p.redraw(); }
  });
  canvas.addEventListener("pointerleave", () => { hover = null; recompute(); p.redraw(); });
  canvas.addEventListener("click", e => {
    if ((canvas._lastMove || 0) > 6) return;          // era un arrastre, no un clic
    const id = sc.pick(e.clientX, e.clientY);
    if (id && opts.onPick) opts.onPick(id);
  });

  // Sondeo del gamepad: solo mientras el modal esta a la vista.
  let poll = 0;
  const startPoll = () => {
    if (poll || !opts.pressed) return;
    poll = setInterval(() => {
      const on = new Set(opts.pressed() || []);
      let change = false;
      on.forEach(id => { if (target[id] !== 1) { target[id] = 1; change = true; } });
      for (const k in target) if (!on.has(k) && target[k] !== 0) { target[k] = 0; change = true; }
      if (change) wake();
    }, 60);
  };
  const stopPoll = () => { if (poll) { clearInterval(poll); poll = 0; } };

  const ro = window.ResizeObserver ? new ResizeObserver(() => p.redraw()) : null;
  if (ro) ro.observe(canvas);
  window.addEventListener("resize", p.redraw);
  p.redraw();

  return {
    canvas,
    select(id) { sel = id; recompute(); p.redraw(); },
    show() { startPoll(); p.redraw(); },
    hide() { stopPoll(); },
    reset(yaw, pitch) { cam.reset(yaw === undefined ? 0 : yaw, pitch === undefined ? 0.42 : pitch); },
    destroy() { stopPoll(); if (ro) ro.disconnect(); sc.dispose(); canvas.remove(); },
  };
}

/* -------------------------------------------------------------------- el estante */
// Caja de carton + cartucho del juego elegido. `setArt(url)` cuelga la caratula de la cara
// frontal de la caja; si la imagen no carga, la caja se queda con su color liso.
function mountShelf(host, opts) {
  opts = opts || {};
  let sc;
  const canvas = document.createElement("canvas");
  canvas.className = "gl3d";
  host.appendChild(canvas);
  try { sc = new GL.Scene(canvas); } catch (e) { host.removeChild(canvas); return null; }

  const b = new GL.Builder();
  // La caja de pie y el cartucho apoyado delante y a la derecha, girado un poco.
  b.push(M4.mul(M4.trans(-3.6, 0, 0), M4.rotY(-0.26)));
  b.merge(MODELS.buildBox());
  b.pop();
  b.push(M4.mul(M4.mul(M4.trans(8.2, -3.6, 5.0), M4.rotY(0.50)), M4.rotX(-0.08)));
  b.merge(MODELS.buildCart(), "cart:");
  b.pop();
  sc.setMesh(b.build());
  sc.gloss = 0.6;

  const st = { yaw: -0.30, pitch: 0.16, dist: 44, min: 24, max: 88, tx: 0, ty: 0, tz: 0 };
  const p = pump(sc);
  const cam = orbit(canvas, sc, st, p.redraw);

  let spin = true, last = 0;
  const loop = t => {
    if (!spin) return;
    if (last) { st.yaw += (t - last) * 0.00022; cam.apply(); sc.draw(null); }
    last = t;
    requestAnimationFrame(loop);
  };
  canvas.addEventListener("pointerdown", () => { spin = false; });

  const ro = window.ResizeObserver ? new ResizeObserver(() => p.redraw()) : null;
  if (ro) ro.observe(canvas);
  window.addEventListener("resize", p.redraw);
  p.redraw();
  requestAnimationFrame(loop);

  return {
    canvas,
    // La caratula se usa para la caja y, recortada por el propio UV, tambien de etiqueta.
    setArt(url) {
      if (!url) { sc.tex.cover = null; sc.tex.label = null; p.redraw(); return; }
      const im = new Image();
      im.onload = () => { sc.setTexture("cover", im); sc.setTexture("label", im); p.redraw(); };
      im.onerror = () => { p.redraw(); };
      im.src = url;
    },
    destroy() { spin = false; if (ro) ro.disconnect(); sc.dispose(); canvas.remove(); },
  };
}

/* Juego que le toca a la casilla `s` cuando el carrusel esta centrado en `c`: el unico
   indice congruente con s modulo K que cae en la ventana de K juegos alrededor de c, o -1
   si esa casilla se sale de la lista. Reciclar por modulo es lo que permite tener miles de
   juegos con K cajas: al desplazarse solo cambia de juego (y de textura) la casilla que
   acaba de salir por el otro lado. Puro: lo prueba carousel_test.js. */
function slotItem(n, K, s, c) {
  if (!n) return -1;
  if (n <= K) return s < n ? s : -1;
  const base = Math.round(c) - Math.floor((K - 1) / 2);
  const i = base + (((s - base) % K) + K) % K;
  return i >= 0 && i < n ? i : -1;
}

/* ------------------------------------------------------------------ el carrusel */
// La biblioteca en 3D de verdad, al estilo de USB Loader GX: las caratulas no son laminas
// sino cajas con volumen, y el selector cambia COMO se colocan, no que se dibuja.
//
//   ring   anillo: las cajas van sobre una circunferencia y el anillo gira hasta poner
//          delante la elegida. Es el carrusel clasico.
//   flow   coverflow: fila recta, las de los lados giradas hacia dentro.
//   wall   rejilla: pared de cajas ligeramente inclinada, se sube y baja por filas.
//
// Una sola malla con K cajas identicas en el origen; cada caja se coloca con la matriz de
// su tramo (`part.mat`), asi que cambiar de vista o desplazarse NO reconstruye geometria.
// Con mas de K juegos las casillas se reciclan por modulo: el juego i vive siempre en la
// casilla i % K, y al salir de la ventana esa casilla recibe otro juego y otra textura.
function mountCarousel(host, opts) {
  opts = opts || {};
  let sc;
  const canvas = document.createElement("canvas");
  canvas.className = "gl3d gl3d-lib";
  host.appendChild(canvas);
  try { sc = new GL.Scene(canvas); } catch (e) { host.removeChild(canvas); return null; }

  let items = opts.items || [];
  const KMAX = 25;
  const K = Math.max(1, Math.min(items.length || 1, KMAX));
  const half = (K - 1) / 2;
  const COLS = 5;

  const b = new GL.Builder();
  const slots = [];
  for (let s = 0; s < K; s++) {
    const p0 = b.parts.length;
    b.merge(MODELS.buildBox({ coverTex: "cov" + s }), "s" + s + ":");
    slots.push({ s: s, p0: p0, p1: b.parts.length, item: -1, url: null, parts: null });
  }
  const mesh = b.build();
  slots.forEach(sl => { sl.parts = mesh.parts.slice(sl.p0, sl.p1); });
  sc.setMesh(mesh);
  sc.gloss = 0.45;

  let mode = opts.mode || "flow";
  let sel = Math.max(0, Math.min(items.length - 1, opts.index || 0));
  let cur = sel;                       // posicion suavizada; es lo que se dibuja
  let hover = -1;

  const CAM = {                        // ojo y punto de mira por vista
    ring: { eye: [0, 10, 60], tgt: [0, 1, -7] },
    flow: { eye: [0, 1.5, 66], tgt: [0, 0.5, 0] },
    wall: { eye: [0, 0, 104], tgt: [0, 0, 0] },
  };

  const itemFor = (s, c) => slotItem(items.length, K, s, c);

  // Colocacion de un juego que esta a distancia `d` del centro (d es fraccionario mientras
  // el carrusel se mueve, por eso todo son funciones continuas).
  function place(d, i) {
    const a = Math.abs(d);
    if (mode === "wall") {
      const col = i % COLS, row = Math.floor(i / COLS);
      const dy = row - cur / COLS;
      if (Math.abs(dy) > 2.6) return null;
      let m = GL.M4.mul(GL.M4.trans((col - (COLS - 1) / 2) * 15.8, -dy * 21.5, 0),
                        GL.M4.rotY((col - (COLS - 1) / 2) * 0.07));
      m = GL.M4.mul(m, GL.M4.rotX(dy * 0.05));
      const k = Math.max(0, 1 - a);
      if (k > 0) m = GL.M4.mul(m, GL.M4.scale(1 + k * 0.14, 1 + k * 0.14, 1));
      return { mat: m, dim: Math.max(0.35, 1 - Math.abs(dy) * 0.2 - (a > 0.5 ? 0.12 : 0)),
               z: -Math.abs(dy) };
    }
    if (mode === "ring") {
      const step = 0.50, R = 34;
      const th = d * step;
      if (a > half + 0.5 || Math.abs(th) > 2.0) return null;
      let m = GL.M4.mul(GL.M4.mul(GL.M4.trans(0, 0, -R), GL.M4.rotY(th)), GL.M4.trans(0, 0, R));
      const lift = Math.max(0, 1 - a);                     // la elegida se adelanta y sube
      m = GL.M4.mul(GL.M4.trans(0, lift * 1.1, lift * 1.4), m);
      if (lift > 0) m = GL.M4.mul(m, GL.M4.scale(1 + lift * 0.13, 1 + lift * 0.13, 1));
      return { mat: m, dim: Math.max(0.30, 1 - a * 0.13), z: Math.cos(th) };
    }
    // flow
    if (a > 7.5) return null;
    const t = Math.tanh(d * 1.25);
    const x = d * 6.6 + t * 7.2;
    const zz = -Math.min(a, 7) * 1.9 + (1 - Math.min(a, 1)) * 7.0;
    let m = GL.M4.mul(GL.M4.trans(x, 0, zz), GL.M4.rotY(-t * 1.05));
    const k = Math.max(0, 1 - a);
    if (k > 0) m = GL.M4.mul(m, GL.M4.scale(1 + k * 0.16, 1 + k * 0.16, 1));
    return { mat: m, dim: Math.max(0.28, 1 - a * 0.115), z: zz };
  }

  function setItem(sl, i) {
    if (sl.item === i) return;
    sl.item = i;
    const url = i >= 0 && items[i] ? items[i].art : null;
    sl.url = url;
    const key = "cov" + sl.s;
    if (sc.tex[key]) { sc.gl.deleteTexture(sc.tex[key]); sc.tex[key] = null; }
    if (!url) return;
    const im = new Image();
    im.onload = () => { if (sl.url === url) { sc.setTexture(key, im); pmp.redraw(); } };
    im.onerror = () => { if (sl.url === url) pmp.redraw(); };
    im.src = url;
  }

  // Reparte casillas, textura y matrices, y ordena el dibujo de atras hacia delante para
  // que la caja elegida tape a las vecinas aunque se crucen.
  function apply() {
    const cam = CAM[mode] || CAM.flow;
    sc.eye = cam.eye.slice(); sc.target = cam.tgt.slice();
    const draws = [];
    slots.forEach(sl => {
      const i = itemFor(sl.s, cur);
      setItem(sl, i);
      const pl = i < 0 ? null : place(i - cur, i);
      const on = !!pl;
      sl.parts.forEach(p => {
        p.hidden = !on;
        if (!on) return;
        p.mat = pl.mat;
        p.dim = pl.dim;
      });
      sc.highlight["s" + sl.s + ":COVER"] = on && i === hover && i !== Math.round(cur) ? 0.30 : 0;
      if (on) draws.push({ sl: sl, z: pl.z });
    });
    draws.sort((p, q) => p.z - q.z);
    const ord = [];
    const seen = {};
    draws.forEach(d => { seen[d.sl.s] = 1; for (let k = d.sl.p0; k < d.sl.p1; k++) ord.push(k); });
    slots.forEach(sl => { if (!seen[sl.s]) for (let k = sl.p0; k < sl.p1; k++) ord.push(k); });
    sc.order = ord;
  }

  const pmp = pump(sc);

  // Muelle hacia la seleccion: el carrusel no salta, rueda.
  let ticking = false, last = 0;
  const tick = t => {
    const dt = last ? Math.min(0.05, (t - last) / 1000) : 0.016;
    last = t;
    const d = sel - cur;
    if (Math.abs(d) < 0.002) { cur = sel; ticking = false; last = 0; apply(); sc.draw(null); return; }
    cur += d * Math.min(1, dt * 11);
    apply(); sc.draw(null);
    requestAnimationFrame(tick);
  };
  const wake = () => { if (!ticking) { ticking = true; last = 0; requestAnimationFrame(tick); } };

  function slotOf(id) {
    const m = /^s(\d+):/.exec(id || "");
    return m ? slots[+m[1]] : null;
  }

  canvas.addEventListener("pointermove", e => {
    const sl = slotOf(sc.pick(e.clientX, e.clientY));
    const h = sl ? sl.item : -1;
    if (h !== hover) { hover = h; canvas.style.cursor = h >= 0 ? "pointer" : "default"; apply(); pmp.redraw(); }
  });
  canvas.addEventListener("pointerleave", () => { hover = -1; apply(); pmp.redraw(); });
  canvas.addEventListener("click", e => {
    const sl = slotOf(sc.pick(e.clientX, e.clientY));
    if (!sl || sl.item < 0) return;
    if (sl.item === sel && opts.onLaunch) opts.onLaunch(sl.item);
    else if (opts.onSelect) opts.onSelect(sl.item);
  });
  // La rueda del raton mueve de uno en uno; en la pared, de fila en fila.
  let accum = 0;
  canvas.addEventListener("wheel", e => {
    e.preventDefault();
    accum += e.deltaY;
    const stepPx = 42;
    while (Math.abs(accum) >= stepPx) {
      const dir = accum > 0 ? 1 : -1;
      accum -= dir * stepPx;
      const n = sel + dir * (mode === "wall" ? COLS : 1);
      if (n < 0 || n >= items.length) { accum = 0; break; }
      sel = n;
      if (opts.onSelect) opts.onSelect(n);
    }
    wake();
  }, { passive: false });

  const ro = window.ResizeObserver ? new ResizeObserver(() => pmp.redraw()) : null;
  if (ro) ro.observe(canvas);
  const onres = () => pmp.redraw();
  window.addEventListener("resize", onres);
  apply(); pmp.redraw();

  return {
    canvas: canvas,
    getMode: () => mode,
    setMode(m) { if (m === mode) return; mode = m; cur = sel; apply(); pmp.redraw(); },
    setIndex(i, animate) {
      i = Math.max(0, Math.min(items.length - 1, i));
      if (i === sel) return;
      sel = i;
      if (animate === false) { cur = i; apply(); pmp.redraw(); } else wake();
    },
    setItems(list, i) {
      items = list || [];
      sel = Math.max(0, Math.min(items.length - 1, i || 0));
      cur = sel;
      slots.forEach(sl => { sl.item = -2; });      // fuerza recarga de todas las texturas
      apply(); pmp.redraw();
    },
    destroy() {
      ticking = false;
      if (ro) ro.disconnect();
      window.removeEventListener("resize", onres);
      sc.dispose(); canvas.remove();
    },
  };
}

return { mountPad, mountShelf, mountCarousel, slotItem };
})();

if (typeof module !== "undefined") module.exports = SCENE3D;   // para las pruebas con node
