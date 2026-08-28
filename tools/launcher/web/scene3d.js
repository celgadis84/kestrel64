/* scene3d.js -- pega la geometria de `models.js` a la interfaz del lanzador.
 *
 * Dos usos:
 *   PAD3D   mando de verdad en el cajon de mapeo: se pincha un boton del modelo y se abre
 *           su ficha; el boton que se pulsa en un gamepad real se enciende en vivo.
 *   SHELF3D estante: la caja de carton y el cartucho del juego seleccionado, con la
 *           caratula puesta de textura sobre la cara frontal.
 *
 * Si el navegador no da WebGL, `mount` devuelve null y quien llama se queda con lo de
 * antes (el mando de CSS). Nada aqui es obligatorio para que el lanzador funcione.
 */
"use strict";

const SCENE3D = (() => {

const M4 = GL.M4;

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
    reset() { cam.reset(0, 0.42); },
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

return { mountPad, mountShelf };
})();
