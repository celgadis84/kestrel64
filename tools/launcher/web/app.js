/* kestrel64 launcher — frontend */
"use strict";

const $  = (s, r) => (r || document).querySelector(s);
const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
const api = async (p, body) => {
  const o = body ? {method: "POST", headers: {"Content-Type": "application/json"},
                    body: JSON.stringify(body)} : {};
  const r = await fetch(p, o);
  return r.json();
};

let SCHEMA = null, CFG = {}, BUILDS = [], ROMS = [], SEL = -1, MODE = "cover", DIRTY = false;
/* Carrusel 3D de la biblioteca: vive entre repintados mientras la vista siga siendo
   una de las suyas, para no tirar y rehacer las texturas de las caratulas. */
let LIB3D = null, LIB3D_KEY = "";
let PADGL = null, PAD_SEL = null, SHELF = null;

function toast(msg, bad) {
  const t = $("#toast");
  t.textContent = msg;
  t.className = "on" + (bad ? " bad" : "");
  clearTimeout(toast._t);
  toast._t = setTimeout(() => (t.className = ""), 2600);
}

/* El perfil se guarda solo: el usuario no deberia acordarse de pulsar "guardar". */
function touch() {
  DIRTY = true;
  clearTimeout(touch._t);
  touch._t = setTimeout(async () => { await api("/api/config", CFG); DIRTY = false; }, 350);
  syncMini();
}

/* ==================================================================== arranque */
(async function boot() {
  SCHEMA = await api("/api/schema");
  CFG = await api("/api/config");
  const b = await api("/api/builds");
  BUILDS = b.builds || [];
  $("#buildinfo").textContent = BUILDS.length
    ? BUILDS.map(x => x.label).join(" + ")
    : "sin compilar (falta kestrel64.exe)";
  applyTheme(CFG.theme);
  buildConfig();
  buildOC();
  buildVideo();
  buildPad();
  wire();
  syncMini();
  if (CFG.romdir) loadRoms(CFG.romdir); else renderStage();
  setInterval(pollStatus, 1200);
})();

/* ==================================================================== ajustes */
function buildConfig() {
  const list = $("#catlist"), panes = $("#catpanes");
  list.innerHTML = ""; panes.innerHTML = "";
  SCHEMA.categories.forEach((c, i) => {
    const b = document.createElement("button");
    b.textContent = c.label;
    b.className = i === 0 ? "on" : "";
    b.onclick = () => {
      $$("#catlist button").forEach(x => x.classList.remove("on"));
      b.classList.add("on");
      $$(".pane").forEach(x => x.classList.remove("on"));
      $("#pane-" + c.id).classList.add("on");
    };
    list.appendChild(b);

    const p = document.createElement("div");
    p.className = "pane" + (i === 0 ? " on" : "");
    p.id = "pane-" + c.id;
    p.innerHTML = `<h2>${c.label}</h2><p class="desc">${c.desc || ""}</p>`;
    c.options.forEach(o => p.appendChild(optRow(o)));
    panes.appendChild(p);
  });

  const t = document.createElement("div");
  t.className = "advtoggle";
  t.innerHTML = `<label class="switch"><input type="checkbox" id="showadv"><span></span>
                 Mostrar opciones avanzadas</label>`;
  list.appendChild(t);
  $("#showadv").onchange = e => document.body.classList.toggle("showadv", e.target.checked);
}

function optRow(o) {
  const d = document.createElement("div");
  d.className = "opt" + (o.adv ? " adv" : "");
  const env = o.env ? `<code>${o.env}</code>` : "";
  d.innerHTML = `<div class="meta"><b>${o.label}${o.adv ? ' <span class="badge advtag">avanzada</span>' : ""}</b>
                 <span>${o.help || ""} ${env}</span></div>
                 <div class="ctl"></div>`;
  $(".ctl", d).appendChild(control(o));
  return d;
}

function control(o) {
  const cur = CFG[o.id] !== undefined ? CFG[o.id] : o.default;
  if (o.type === "bool") {
    const l = document.createElement("label");
    l.className = "switch";
    l.innerHTML = `<input type="checkbox" ${cur ? "checked" : ""}><span></span>`;
    $("input", l).onchange = e => { CFG[o.id] = e.target.checked; touch(); };
    return l;
  }
  if (o.type === "choice") {
    const s = document.createElement("select");
    (o.values || []).forEach(([v, lab]) => {
      const op = document.createElement("option");
      op.value = v; op.textContent = lab;
      if (String(v) === String(cur)) op.selected = true;
      s.appendChild(op);
    });
    s.onchange = e => { CFG[o.id] = e.target.value; touch(); render(); };
    return s;
  }
  const i = document.createElement("input");
  if (o.type === "int" || o.type === "float") {
    i.type = "number";
    if (o.min !== undefined) i.min = o.min;
    if (o.max !== undefined) i.max = o.max;
    if (o.step) i.step = o.step;
  } else {
    i.type = "text";
    if (o.type === "hex") i.placeholder = "0x80000000";
    if (o.type === "path") i.placeholder = "ruta...";
  }
  i.value = cur === null || cur === undefined ? "" : cur;
  i.oninput = e => {
    const v = e.target.value;
    CFG[o.id] = (o.type === "int") ? (parseInt(v, 10) || 0)
              : (o.type === "float") ? (parseFloat(v) || 0) : v;
    touch();
  };
  return i;
}

/* Refresca los controles cuando algo cambia desde otro sitio (modal de OC, etc). */
function render() {
  buildConfig();
  document.body.classList.toggle("showadv", !!($("#showadv") && $("#showadv").checked));
  buildOC(); buildVideo(); syncMini();
}

/* ==================================================================== overclock */
const OC_DOMAINS = [
  {id: "oc_all",   label: "Todos los dominios", hz: null,   base: null},
  {id: "oc_cpu",   label: "CPU",   hz: "R4300i",   base: 93.75},
  {id: "oc_rsp",   label: "RSP",   hz: "vector",   base: 62.5},
  {id: "oc_rdram", label: "RDRAM", hz: "DDR",      base: 250},
];
const OC_PRESETS = [0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0];

function buildOC() {
  const wrap = $("#oc-dials");
  const link = CFG.oc_link !== false;
  $("#oc-link").checked = link;
  $("#oc-throttle").value = String(CFG.throttle || "auto");
  wrap.innerHTML = "";
  OC_DOMAINS.forEach(d => {
    const isAll = d.id === "oc_all";
    if (isAll ? !link : link) return;
    const v = Number(CFG[d.id] !== undefined ? CFG[d.id] : 1.0);
    const el = document.createElement("div");
    el.className = "dial";
    el.innerHTML = `
      <div class="hd"><b>${d.label}</b><small>${d.hz || "cpu + rsp + rdram a la vez"}</small>
        <span class="val">${v.toFixed(2)}x</span></div>
      <input type="range" min="0.25" max="4" step="0.05" value="${v}">
      <div class="presets">${OC_PRESETS.map(p =>
        `<button data-v="${p}" class="${Math.abs(p - v) < 1e-6 ? "on" : ""}">${
          p === 1 ? "1x real" : p + "x"}</button>`).join("")}</div>
      <div class="hz">${d.base ? (d.base * v).toFixed(2) + " MHz efectivos (base " + d.base + ")" : ""}</div>`;
    const set = nv => {
      nv = Math.max(0.25, Math.min(4, nv));
      if (isAll) { CFG.oc_all = nv; CFG.oc_cpu = CFG.oc_rsp = CFG.oc_rdram = nv; }
      else CFG[d.id] = nv;
      touch(); buildOC();
    };
    $("input", el).oninput = e => {
      const nv = parseFloat(e.target.value);
      $(".val", el).textContent = nv.toFixed(2) + "x";
      if (d.base) $(".hz", el).textContent =
        (d.base * nv).toFixed(2) + " MHz efectivos (base " + d.base + ")";
    };
    $("input", el).onchange = e => set(parseFloat(e.target.value));
    $$(".presets button", el).forEach(b => b.onclick = () => set(parseFloat(b.dataset.v)));
    wrap.appendChild(el);
  });
}

function syncMini() {
  const link = CFG.oc_link !== false;
  const v = link ? Number(CFG.oc_all || 1)
                 : Math.max(Number(CFG.oc_cpu || 1), Number(CFG.oc_rsp || 1));
  const unl = String(CFG.throttle) === "0";
  const m = $("#ocmini");
  m.textContent = unl ? "sin limite" : v.toFixed(2) + "x";
  m.style.display = (unl || Math.abs(v - 1) > 1e-6) ? "" : "none";
}

/* ==================================================================== video */
function buildVideo() {
  const cards = $("#plugin-cards");
  const have = new Set(BUILDS.map(b => b.id));
  const defs = [
    {id: "auto", label: "Automatico", desc: "Usa la mejor compilacion disponible.", ok: true},
    {id: "soft", label: "SoftRDP",
     desc: "Rasterizador propio en CPU. Determinista y sin GPU: es el oraculo de correccion.",
     ok: have.has("soft"), tag: "build/"},
    {id: "prdp", label: "paraLLEl-RDP",
     desc: "RDP a bajo nivel sobre Vulkan, en la GPU. Mas rapido y mas exacto en subpixel.",
     ok: have.has("prdp"), tag: "build-prdp/"},
  ];
  const cur = CFG.plugin || "auto";
  cards.innerHTML = "";
  defs.forEach(d => {
    const c = document.createElement("div");
    c.className = "pcard" + (cur === d.id ? " on" : "") + (d.ok ? "" : " off");
    c.innerHTML = `<b>${d.label}</b><span>${d.desc}</span>` +
      (d.ok ? (d.tag ? `<div class="tag">${d.tag}</div>` : "")
            : `<div class="tag">no compilado — cmake -DKESTREL_PRDP=ON</div>`);
    if (d.ok) c.onclick = () => { CFG.plugin = d.id; touch(); buildVideo(); render(); };
    cards.appendChild(c);
  });

  const box = $("#video-opts");
  box.innerHTML = "";
  const cat = SCHEMA.categories.find(c => c.id === "video");
  cat.options.filter(o => o.id !== "plugin").forEach(o => box.appendChild(optRow(o)));
}

/* ==================================================================== mando */
const PAD_LABEL = {};
function padDefaults() { return JSON.parse(JSON.stringify(SCHEMA.defaults.pad)); }

function buildPad() {
  if (!CFG.pad) CFG.pad = padDefaults();
  SCHEMA.pad_buttons.concat(SCHEMA.pad_axes).forEach(b => PAD_LABEL[b.id] = b.label);
  $$("#pad3d [data-id]").forEach(el => {
    el.onclick = ev => { ev.stopPropagation(); selectPad(el.dataset.id, el); };
  });
  $("#pad-reset").onclick = () => {
    CFG.pad = padDefaults(); touch(); PAD_SEL = null;
    if (PADGL) PADGL.select(null);
    drawer(null); toast("Mando restaurado");
  };
  // El stick no es un boton: abre las cuatro direcciones a la vez.
  $(".pb.stick").onclick = () => selectPad("STICK");
  mountPadGL();
}

/* El mando de verdad, en WebGL. Si el navegador no da contexto 3D se queda el de CSS, que
   mapea exactamente los mismos ids, asi que la funcionalidad no depende de esto. */
function mountPadGL() {
  if (PADGL || typeof SCENE3D === "undefined") return;
  PADGL = SCENE3D.mountPad($("#padgl"), {
    onPick: id => selectPad(id),
    pressed: padPressed,
  });
  if (!PADGL) return;
  $("#padgl").classList.add("on");
  $("#pad3d").classList.add("off");
  padChips();
}

/* Tira de fichas debajo del modelo. El Z vive en la cara de abajo del mando y hay botones
   que quedan tapados segun como se gire: con esto SIEMPRE hay manera de llegar a todos. */
function padChips() {
  const row = document.createElement("div");
  row.className = "padchips";
  SCHEMA.pad_buttons.forEach(b => {
    const c = document.createElement("button");
    c.textContent = b.id; c.title = b.label; c.dataset.chip = b.id;
    c.onclick = () => selectPad(b.id);
    row.appendChild(c);
  });
  const st = document.createElement("button");
  st.textContent = "STICK"; st.title = "Stick analogico"; st.dataset.chip = "STICK";
  st.onclick = () => selectPad("STICK");
  row.appendChild(st);
  $("#padgl").appendChild(row);
}

/* Que controles estan pulsados ahora mismo, leyendo el gamepad por el MISMO mapa que se le
   pasa al emulador: si aqui se enciende el boton equivocado, el mapa esta mal. */
function padPressed() {
  const gp = (navigator.getGamepads && navigator.getGamepads()[0]) || null;
  if (!gp || !CFG.pad) return [];
  const on = [];
  for (const id in CFG.pad) {
    const g = CFG.pad[id] && CFG.pad[id].gp;
    if (!g) continue;
    const i = GLFW_GP.indexOf(g);
    const b = i >= 0 ? gp.buttons[i] : gp.buttons[Number((g.match(/^BUTTON_(\d+)$/) || [])[1])];
    if (b && b.pressed) on.push(id);
  }
  // El stick izquierdo va directo al emulador, sin mapa: se enciende solo con moverlo.
  if (Math.hypot(gp.axes[0] || 0, gp.axes[1] || 0) > 0.35) on.push("STICK");
  // Las cuatro direcciones del stick comparten pieza en el modelo (ids SX+/SX-/SY+/SY-).
  if (on.some(k => k[0] === "S" && k.length === 3)) on.push("STICK");
  return on;
}

function selectPad(id, el) {
  PAD_SEL = id;
  $$("#pad3d .sel").forEach(x => x.classList.remove("sel"));
  if (!el) el = $(`#pad3d [data-id="${id}"]`);
  if (el) el.classList.add("sel");
  if (PADGL) PADGL.select(id);
  $$("#padgl .padchips button").forEach(c => c.classList.toggle("on", c.dataset.chip === id));
  drawer(id);
}

const GLFW_GP = ["A", "B", "X", "Y", "LEFT_BUMPER", "RIGHT_BUMPER", "LEFT_TRIGGER",
  "RIGHT_TRIGGER", "BACK", "START", "LEFT_THUMB", "RIGHT_THUMB",
  "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"];

function drawer(id) {
  const d = $("#pad-drawer");
  if (!id) { d.innerHTML = `<p class="hint">Ningun control seleccionado.</p>`; return; }
  const ids = id === "STICK" ? SCHEMA.pad_axes.map(a => a.id) : [id];
  const title = id === "STICK" ? "Stick analogico" : (PAD_LABEL[id] || id);
  d.innerHTML = `<h4>${title}</h4>
    <p class="sub">${id === "STICK"
      ? "Cuatro direcciones. En gamepad el stick izquierdo va directo, sin mapear."
      : "Una tecla y, si quieres, un boton de gamepad."}</p>`;
  ids.forEach(k => {
    const cur = (CFG.pad && CFG.pad[k]) || {key: "", gp: ""};
    const f = document.createElement("div");
    f.className = "fld";
    f.innerHTML = `<label>${PAD_LABEL[k] || k}</label>
      <button class="cap" data-k="${k}" data-w="key">${cur.key || "sin asignar"}</button>
      <button class="clr" data-k="${k}">borrar tecla</button>`;
    if (id !== "STICK") {
      f.innerHTML += `<label style="margin-top:10px">Gamepad</label>
        <button class="cap" data-k="${k}" data-w="gp">${cur.gp || "sin asignar"}</button>`;
    }
    d.appendChild(f);
  });
  $$(".cap", d).forEach(b => b.onclick = () => capture(b));
  $$(".clr", d).forEach(b => b.onclick = () => {
    CFG.pad[b.dataset.k].key = ""; touch(); drawer(id);
  });
}

let capturing = null;
function capture(btn) {
  if (capturing) capturing.el.classList.remove("listen");
  capturing = {el: btn, k: btn.dataset.k, w: btn.dataset.w};
  btn.classList.add("listen");
  btn.textContent = btn.dataset.w === "gp" ? "pulsa un boton del gamepad..." : "pulsa una tecla...";
  if (btn.dataset.w === "gp") pollGamepad();
}

document.addEventListener("keydown", e => {
  if (!capturing || capturing.w !== "key") return;
  e.preventDefault();
  const g = glfwKey(e);
  if (!g) return;
  CFG.pad[capturing.k].key = g;
  touch();
  capturing.el.classList.remove("listen");
  capturing = null;
  drawer(PAD_SEL);
});

function glfwKey(e) {
  const c = e.code;
  if (/^Key[A-Z]$/.test(c)) return c.slice(3);
  if (/^Digit[0-9]$/.test(c)) return c.slice(5);
  if (/^Numpad[0-9]$/.test(c)) return "KP_" + c.slice(6);
  if (/^F[0-9]{1,2}$/.test(c)) return c;
  const map = {
    Space: "SPACE", Enter: "ENTER", NumpadEnter: "KP_ENTER", Escape: "ESCAPE", Tab: "TAB",
    Backspace: "BACKSPACE", ArrowUp: "UP", ArrowDown: "DOWN", ArrowLeft: "LEFT",
    ArrowRight: "RIGHT", ShiftLeft: "LEFT_SHIFT", ShiftRight: "RIGHT_SHIFT",
    ControlLeft: "LEFT_CONTROL", ControlRight: "RIGHT_CONTROL", AltLeft: "LEFT_ALT",
    AltRight: "RIGHT_ALT", Minus: "MINUS", Equal: "EQUAL", Comma: "COMMA", Period: "PERIOD",
    Slash: "SLASH", Semicolon: "SEMICOLON", Quote: "APOSTROPHE", BracketLeft: "LEFT_BRACKET",
    BracketRight: "RIGHT_BRACKET", Backslash: "BACKSLASH", Backquote: "GRAVE_ACCENT",
    Home: "HOME", End: "END", PageUp: "PAGE_UP", PageDown: "PAGE_DOWN", Insert: "INSERT",
    Delete: "DELETE",
  };
  return map[c] || null;
}

function pollGamepad() {
  if (!capturing || capturing.w !== "gp") return;
  const gp = (navigator.getGamepads && navigator.getGamepads()[0]) || null;
  if (gp) for (let i = 0; i < gp.buttons.length; i++) {
    if (gp.buttons[i].pressed) {
      CFG.pad[capturing.k].gp = GLFW_GP[i] || ("BUTTON_" + i);
      touch();
      capturing.el.classList.remove("listen");
      capturing = null;
      drawer(PAD_SEL);
      return;
    }
  }
  requestAnimationFrame(pollGamepad);
}

/* ==================================================================== biblioteca */
async function loadRoms(dir) {
  $("#rom-dir").textContent = dir;
  const r = await api("/api/roms?dir=" + encodeURIComponent(dir));
  ROMS = r.roms || [];
  SEL = ROMS.length ? 0 : -1;
  CFG.romdir = dir; touch();
  renderStage();
}

function filtered() {
  const q = ($("#search").value || "").toLowerCase();
  if (!q) return ROMS;
  return ROMS.filter(r => (r.title + " " + r.file).toLowerCase().includes(q));
}

function artUrl(r) {
  return "/api/boxart?id=" + encodeURIComponent(r.id) +
         "&name=" + encodeURIComponent((r.header && r.header.name) || "") +
         "&region=" + encodeURIComponent((r.header && r.header.region) || "");
}

function renderStage() {
  const st = $("#stage");
  const list = filtered();
  if (!list.length) {
    st.innerHTML = `<div class="empty"><b>${ROMS.length ? "Ningun resultado" : "Sin ROMs"}</b>
      <p>${ROMS.length ? "Prueba otro filtro." : "Elige la carpeta donde tengas los cartuchos."}</p></div>`;
    updateDock(); return;
  }
  if (SEL >= list.length) SEL = 0;
  if (MODE !== "shelf" && SHELF) { SHELF.destroy(); SHELF = null; }
  if (!LAYOUT3D[MODE] && LIB3D) { LIB3D.destroy(); LIB3D = null; LIB3D_KEY = ""; }
  if (LAYOUT3D[MODE]) r3D(st, list, LAYOUT3D[MODE]);
  else ({rows: rRows, list: rList, wmrows: rWonder, shelf: rShelf}[MODE] || rRows)(st, list);
  updateDock();
}

function pick(i) { SEL = i; renderStage(); }

/* ------------------------------------------------------------- caratulas en 3D */
/* Coverflow, rueda y rejilla son la MISMA escena con otra colocacion: cajas de carton con
   volumen, no laminas. El selector de vista solo cambia donde se pone cada caja, asi que
   pasar de una a otra no recarga ni una textura. Sin WebGL se cae a las de CSS de antes. */
const LAYOUT3D = { cover: "flow", wheel: "ring", grid: "wall" };

function r3D(st, list, layout) {
  const key = list.map(r => r.id).join("|");
  if (LIB3D && $(".lib3d", st)) {                 // ya montado: recolocar, no reconstruir
    if (key !== LIB3D_KEY) { LIB3D.setItems(items3d(list), SEL); LIB3D_KEY = key; }
    else LIB3D.setIndex(SEL, false);
    LIB3D.setMode(layout);
    return;
  }
  if (LIB3D) { LIB3D.destroy(); LIB3D = null; }
  st.innerHTML = `<div class="lib3d"></div>`;
  const host = $(".lib3d", st);
  if (typeof SCENE3D !== "undefined")
    LIB3D = SCENE3D.mountCarousel(host, {
      items: items3d(list), mode: layout, index: SEL,
      onSelect: i => { SEL = i; if (LIB3D) LIB3D.setIndex(i); updateDock(); },
      onLaunch: () => activate(),
    });
  if (!LIB3D) {                                   // navegador sin WebGL
    LIB3D_KEY = "";
    ({ flow: rCover, ring: rWheel, wall: rGrid }[layout] || rCover)(st, list);
    return;
  }
  LIB3D_KEY = key;
}

function items3d(list) {
  return list.map(r => ({ art: artUrl(r), title: r.title }));
}

function card(r, i, list) {
  const d = document.createElement("div");
  d.className = "card" + (i === SEL ? " sel" : "");
  d.innerHTML = `<div class="art"><img alt=""></div>
    <div class="nm">${r.title}<small>${r.header ? r.header.region_label + " · " + r.header.mb + " MB" : r.file}</small></div>`;
  const img = $("img", d);
  img.src = artUrl(r);
  img.onerror = () => { img.outerHTML = `<div class="ph">${r.title}</div>`; };
  d.onclick = () => pick(list.indexOf(r));
  d.ondblclick = launch;
  return d;
}

function rGrid(st, list) {
  st.innerHTML = `<div class="grid"></div>`;
  const g = $(".grid", st);
  list.forEach((r, i) => g.appendChild(card(r, i, list)));
}

function rRows(st, list) {
  /* Filas por region + una de "recientes por tamano", que es lo que hace que la vista
     estilo Netflix tenga sentido: agrupar, no solo apilar. */
  const by = {};
  list.forEach(r => {
    const k = r.header ? r.header.region_label : "Desconocida";
    (by[k] = by[k] || []).push(r);
  });
  st.innerHTML = `<div class="rows"></div>`;
  const w = $(".rows", st);
  const secs = [["Todos", list]].concat(Object.entries(by).sort());
  secs.forEach(([nm, rs]) => {
    const s = document.createElement("div");
    s.className = "rowsec";
    s.innerHTML = `<h4>${nm} <span style="color:var(--dim2);font-weight:400">${rs.length}</span></h4>
                   <div class="rowstrip"></div>`;
    const strip = $(".rowstrip", s);
    rs.forEach(r => strip.appendChild(card(r, list.indexOf(r), list)));
    w.appendChild(s);
  });
}

function rCover(st, list) {
  st.innerHTML = `<div class="cover-scene"><div class="cover-track"></div></div>`;
  const tr = $(".cover-track", st);
  list.forEach((r, i) => {
    const d = document.createElement("div");
    d.className = "cv";
    d.innerHTML = `<div class="face"></div><div class="refl"></div>`;
    const im = new Image();
    im.src = artUrl(r);
    im.onload = () => { $(".face", d).appendChild(im); $(".refl", d).appendChild(im.cloneNode()); };
    im.onerror = () => { $(".face", d).innerHTML = `<div class="ph">${r.title}</div>`; };
    d.onclick = () => (i === SEL ? activate() : pick(i));
    tr.appendChild(d);
  });
  layoutCover(list);
  st.onwheel = e => {
    e.preventDefault();
    const n = SEL + (e.deltaY > 0 ? 1 : -1);
    if (n >= 0 && n < list.length) { SEL = n; layoutCover(list); updateDock(); }
  };
}

function layoutCover(list) {
  const cvs = $$(".cv");
  cvs.forEach((d, i) => {
    const k = i - SEL, a = Math.abs(k);
    if (a > 7) { d.style.opacity = 0; d.style.transform = "translateZ(-2600px)"; return; }
    const x = k === 0 ? 0 : Math.sign(k) * (128 + (a - 1) * 62);
    const rot = k === 0 ? 0 : -Math.sign(k) * 58;
    const z = k === 0 ? 130 : -a * 62;
    d.style.opacity = 1 - a * 0.1;
    d.style.zIndex = 100 - a;
    d.style.transform = `translateX(${x}px) translateZ(${z}px) rotateY(${rot}deg) scale(${k === 0 ? 1.14 : 1})`;
    d.classList.toggle("sel", k === 0);
  });
}

function rWheel(st, list) {
  st.innerHTML = `<div class="wheel"><div class="wheel-track"></div></div>`;
  const tr = $(".wheel-track", st);
  list.forEach((r, i) => {
    const d = document.createElement("div");
    d.className = "wl";
    d.textContent = r.title;
    d.onclick = () => (i === SEL ? activate() : pick(i));
    tr.appendChild(d);
  });
  layoutWheel();
  st.onwheel = e => {
    e.preventDefault();
    const n = SEL + (e.deltaY > 0 ? 1 : -1);
    if (n >= 0 && n < list.length) { SEL = n; layoutWheel(); updateDock(); }
  };
}

function layoutWheel() {
  /* Arco: cada entrada se separa en angulo desde la seleccionada, como el carrusel de
     Hyperspin. El desplazamiento en X sale del coseno, asi que las lejanas se meten. */
  $$(".wl").forEach((d, i) => {
    const k = i - SEL, a = Math.abs(k);
    if (a > 9) { d.style.opacity = 0; return; }
    const ang = k * 9;
    const rad = ang * Math.PI / 180;
    d.style.opacity = Math.max(0, 1 - a * 0.1);
    d.style.top = `calc(50% + ${Math.sin(rad) * 420}px - 22px)`;
    d.style.transform = `translateX(${(1 - Math.cos(rad)) * 340}px) scale(${k === 0 ? 1.08 : 1})`;
    d.classList.toggle("sel", k === 0);
  });
}

function rList(st, list) {
  st.innerHTML = `<div class="tbl"><table><thead><tr>
    <th>Titulo</th><th>Fichero</th><th>ID</th><th>Region</th><th>Formato</th>
    <th>MB</th><th>CRC</th></tr></thead><tbody></tbody></table></div>`;
  const tb = $("tbody", st);
  list.forEach((r, i) => {
    const h = r.header || {};
    const tr = document.createElement("tr");
    if (i === SEL) tr.className = "sel";
    tr.innerHTML = `<td>${r.title}</td><td>${r.file}</td><td>${h.cart || "?"}</td>
      <td>${h.region_label || "?"}</td><td>${h.fmt || "?"}</td><td>${h.mb || "?"}</td>
      <td style="color:var(--dim2)">${h.crc || ""}</td>`;
    tr.onclick = () => pick(i);
    tr.ondblclick = launch;
    tb.appendChild(tr);
  });
}

/* Lista de filas altas al estilo WonderMenu (el menu de flashcart de lmcd): una fila por
   cartucho, caratula pequena a la izquierda, titulo grande, subtitulo a media tinta y los
   datos del cartucho a la derecha. Lo elegido no se pinta con un borde sino con una
   pastilla maciza que SOBRESALE por los dos lados y crece un poco de alto, que es
   justo lo que hace el original. Todo sale de los tokens (`--selbg`, `--seltxt`, `--selr`,
   `--font-display`), asi que la misma vista vale con el tema de fabrica y con el de
   WonderMenu sin una sola regla duplicada. No se copia ni un recurso del proyecto
   original: es AGPLv3 y aqui solo se replica la DISPOSICION. */
function rWonder(st, list) {
  st.innerHTML = `<div class="wrows"></div>`;
  const w = $(".wrows", st);
  list.forEach((r, i) => {
    const h = r.header || {};
    const d = document.createElement("div");
    d.className = "wrow" + (i === SEL ? " sel" : "");
    d.innerHTML = `<div class="wart"></div>
      <div class="wtxt"><b>${r.title}</b><i>${r.file}</i></div>
      <div class="wmeta"><span>${h.region_label || "?"}</span>
        <span>${h.fmt || "?"}</span><em>${h.mb || "?"} MB</em></div>`;
    const im = new Image();
    im.src = artUrl(r);
    im.onload = () => $(".wart", d).appendChild(im);
    im.onerror = () => $(".wart", d).classList.add("ph");
    d.onclick = () => (i === SEL ? activate() : pick(i));
    d.ondblclick = launch;
    w.appendChild(d);
  });
  // Con las flechas la fila elegida se sale de la ventana enseguida; se trae a la vista
  // sin animacion porque el repintado es completo y la animacion se veria como un salto.
  const sel = $(".wrow.sel", w);
  if (sel) sel.scrollIntoView({block: "nearest"});
}

/* Estante: la caja de carton y el cartucho del juego elegido, en 3D de verdad, con la
   caratula puesta de textura. Se navega con las flechas como en las demas vistas. */
function rShelf(st, list) {
  const r = list[SEL] || list[0];
  st.innerHTML = `<div class="shelf"><div class="shelf-name"></div></div>`;
  const host = $(".shelf", st);
  $(".shelf-name", host).innerHTML = r
    ? `<b>${r.title}</b>${r.header ? r.header.region_label + " &middot; " + r.header.mb + " MB" : r.file}`
    : "";
  if (SHELF) { SHELF.destroy(); SHELF = null; }
  if (typeof SCENE3D !== "undefined") SHELF = SCENE3D.mountShelf(host, {});
  if (!SHELF) { host.innerHTML = `<p class="hint">Este navegador no da WebGL.</p>`; return; }
  host.insertBefore(SHELF.canvas, $(".shelf-name", host));
  SHELF.setArt(r ? artUrl(r) : null);
  st.onwheel = e => {
    e.preventDefault();
    const n = SEL + (e.deltaY > 0 ? 1 : -1);
    if (n >= 0 && n < list.length) { SEL = n; renderStage(); }
  };
}

function updateDock() {
  const list = filtered(), r = list[SEL];
  const img = $("#dock-img");
  if (!r) {
    $("#dock-title").textContent = ROMS.length ? "Nada seleccionado" : "Elige una carpeta de ROMs";
    $("#dock-sub").textContent =
      "El lanzador lee la cabecera de cada cartucho: nombre interno, ID, region y CRC.";
    $("#dock-badges").innerHTML = "";
    img.removeAttribute("src");
    $("#launch").disabled = true;
    $("#dock-card").disabled = true;
    return;
  }
  const h = r.header || {};
  $("#dock-title").textContent = r.title;
  $("#dock-sub").textContent = r.path;
  img.src = artUrl(r);
  img.onerror = () => img.removeAttribute("src");
  const bl = BUILDS.find(b => b.id === (CFG.plugin === "auto" || !CFG.plugin
    ? (BUILDS.some(x => x.id === "prdp") ? "prdp" : "soft") : CFG.plugin));
  const link = CFG.oc_link !== false;
  const oc = link ? Number(CFG.oc_all || 1) : Number(CFG.oc_cpu || 1);
  $("#dock-badges").innerHTML = [
    h.cart ? `<span class="badge">ID ${h.cart}</span>` : "",
    h.region_label ? `<span class="badge">${h.region_label}</span>` : "",
    h.fmt ? `<span class="badge">${h.fmt}</span>` : "",
    h.mb ? `<span class="badge">${h.mb} MB</span>` : "",
    bl ? `<span class="badge acc">${bl.label}</span>` : `<span class="badge">sin compilacion</span>`,
    CFG.jit === false ? `<span class="badge">interprete</span>` : `<span class="badge">JIT</span>`,
    CFG.threads === false ? `<span class="badge">lockstep</span>` : `<span class="badge">multihilo</span>`,
    Math.abs(oc - 1) > 1e-6 ? `<span class="badge acc">overclock ${oc.toFixed(2)}x</span>` : "",
    String(CFG.throttle) === "0" ? `<span class="badge acc">sin limite</span>` : "",
    CFG.paused ? `<span class="badge acc">arranca en pausa (MCP)</span>` : "",
  ].join("");
  $("#launch").disabled = !BUILDS.length;
  $("#dock-card").disabled = false;
}

/* ==================================================================== lanzar */
async function launch() {
  const r = filtered()[SEL];
  if (!r) return;
  await api("/api/config", CFG);
  const res = await api("/api/launch", {rom: r.path, profile: CFG});
  if (!res.ok) { toast(res.error || "no se pudo lanzar", true); return; }
  toast("Lanzado: " + r.title);
  openModal("#m-console");
}

async function pollStatus() {
  const s = await api("/api/status");
  $("#btn-console").classList.toggle("live", !!s.running);
  $("#stop").hidden = !s.running;
  $("#constate").textContent = s.running
    ? "en marcha · " + s.uptime + " s"
    : (s.code === null || s.code === undefined ? "parado" : "termino con codigo " + s.code);
  if ($("#m-console").classList.contains("on")) {
    $("#cmdline").textContent = (s.cmd || []).join(" ");
    const el = $("#conlog");
    const stick = el.scrollTop + el.clientHeight >= el.scrollHeight - 30;
    el.textContent = (s.log || []).join("\n");
    if (stick) el.scrollTop = el.scrollHeight;
  }
}

/* ==================================================================== navegador */
let fbCur = "";
async function fbGo(d) {
  const r = await api("/api/browse?dir=" + encodeURIComponent(d || ""));
  fbCur = r.cwd || "";
  $("#fb-cwd").textContent = fbCur || "Unidades";
  $("#fb-count").textContent = r.roms ? r.roms + " ROMs en esta carpeta" : "";
  const ul = $("#fb-list");
  ul.innerHTML = "";
  if (r.up !== null && r.up !== undefined) {
    const li = document.createElement("li");
    li.innerHTML = `<b>&uarr;</b> ..`;
    li.onclick = () => fbGo(r.up);
    ul.appendChild(li);
  }
  (r.dirs || []).forEach(d2 => {
    const li = document.createElement("li");
    li.innerHTML = `<b>&#9642;</b> ${d2.name}`;
    li.onclick = () => fbGo(d2.path);
    ul.appendChild(li);
  });
  $("#fb-ok").disabled = !fbCur;
}

/* ==================================================================== telemetria
   Ventana en vivo contra el servidor que el emulador lleva dentro -- el MISMO que usa el
   MCP. El backend solo hace de puente: nada de lo que se ve aqui esta reinterpretado. */
let TELE_TAB = "estado", TELE_BUSY = false;

async function tele(cmd, args, snapshot) {
  let u = "/api/tele?cmd=" + encodeURIComponent(cmd);
  if (args) u += "&args=" + encodeURIComponent(JSON.stringify(args));
  if (snapshot) u += "&snapshot=1";
  try { return await api(u); }
  catch (e) { return {ok: false, error: String(e)}; }
}

/* El puente ya manda como cadena "0x...." todo entero que no cabe en un double, asi que
   aqui no hay que adivinar nada: los numeros son exactos y las cadenas van tal cual. */
const hex = (v, w) => {
  if (typeof v !== "number") return String(v);
  const s = (v < 0 ? (v >>> 0) : v).toString(16);
  return "0x" + (w ? s.padStart(w, "0") : s);
};

/* Parte baja de 32 bits, venga como numero o como "0x<16 digitos>". Es lo que se usa para
   direccionar: el bus del R4300i en modo de 32 bits solo mira ahi. */
const lo32 = v => (typeof v === "number" ? v >>> 0
                   : (parseInt(String(v).slice(-8), 16) || 0) >>> 0);

function teleCards(pairs) {
  return '<div class="telegrid">' + pairs.map(([k, v]) =>
    `<div class="telecard"><b>${k}</b><span>${v}</span></div>`).join("") + "</div>";
}

/* Aplana un objeto anidado (rcp.regs viene por bloques: mi, sp, dpc, vi, ai, pi, si). */
function teleRegs(obj, prefix) {
  let out = "";
  for (const k of Object.keys(obj)) {
    const v = obj[k], name = (prefix ? prefix + "." : "") + k;
    if (Array.isArray(v)) {
      // rsp.regs manda los escalares como array: se indexan.
      v.forEach((x, i) => out += `<div><i>${name}[${i}]</i><span>${
        typeof x === "number" ? hex(x) : String(x)}</span></div>`);
      continue;
    }
    if (v && typeof v === "object") { out += teleRegs(v, name); continue; }
    const val = typeof v === "number" ? hex(v) : String(v);
    out += `<div><i>${name}</i><span>${val}</span></div>`;
  }
  return out;
}

function teleTable(rows, cols) {
  const max = rows.reduce((m, r) => Math.max(m, r.pct || 0), 0) || 1;
  return '<table class="prof"><tr>' + cols.map(c => `<th>${c}</th>`).join("") + "</tr>" +
    rows.map(r => "<tr>" + cols.map(c => {
      if (c === "%") return `<td>${(r.pct || 0).toFixed(2)}<span class="bar" style="width:${
        (100 * (r.pct || 0) / max).toFixed(1)}%"></span></td>`;
      return `<td>${r[c] === undefined ? "" : (typeof r[c] === "number" && c !== "count"
        ? hex(r[c]) : r[c])}</td>`;
    }).join("") + "</tr>").join("") + "</table>";
}

async function teleTick(force) {
  const m = $("#m-tele");
  if (!m.classList.contains("on")) return;
  if (TELE_BUSY && !force) return;
  TELE_BUSY = true;
  const body = $("#telebody");
  try {
    if (TELE_TAB === "dbg") {
      // El depurador NO se refresca solo: cada repintado toma el candado del nucleo y, a
      // marcha libre, ese candado se suelta una vez por campo de video. Refrescar en bucle
      // convertiria la ventana en un freno. Se repinta tras cada accion y con "Releer".
      if (force) await dbgDraw();
      return;
    }
    if (TELE_TAB === "imagen") {
      // La imagen se refresca sola cambiando la marca de tiempo; el resto se repinta.
      let img = $("#tele-img");
      if (!img) {
        body.innerHTML = '<div class="telefb"><img id="tele-img" alt="framebuffer">' +
          '<p class="hint">Framebuffer que el VI esta escaneando, decodificado desde la ' +
          'RDRAM. No es una lectura de vuelta de la GPU.</p></div>';
        img = $("#tele-img");
      }
      img.src = "/api/tele/fb?height=240&t=" + Date.now();
      $("#telestate").textContent = "";
      return;
    }
    let r;
    if (TELE_TAB === "estado") r = await tele("status");
    else if (TELE_TAB === "cpu") r = await tele("cpu.regs");
    else if (TELE_TAB === "rcp") r = await tele("rcp.regs");
    else if (TELE_TAB === "rsp") r = await tele("rsp.regs");
    else r = await tele("prof.cpu", {top: 20}, true);

    if (!r.ok) {
      body.innerHTML = `<div class="teleerr">${r.error}</div>`;
      $("#telestate").textContent = "";
      return;
    }
    const d = r.data;
    if (TELE_TAB === "estado") {
      const sp = d.speed || {}, oc = sp.overclock || {}, oq = sp.occupancy || {};
      const g = d.game || {};
      body.innerHTML =
        teleCards([
          ["estado", d.paused ? "PAUSADO" : "corriendo"],
          ["campos/s", (oq.fps || 0).toFixed(1)],
          ["CPU % de N64 real", (sp.cpuPct || 0).toFixed(1)],
          ["RSP % de N64 real", (sp.rspPct || 0).toFixed(1)],
          ["RDRAM % de N64 real", (sp.rdramPct || 0).toFixed(1)],
          ["instrucciones", (sp.insns || 0).toLocaleString("es")],
        ]) +
        "<h4>Ocupacion de los hilos</h4>" +
        // En modo multihilo el % de CPU sube cuando la CPU gira esperando al RCP: sin esto
        // el estado engañaria. cpuWait alto = el palo largo es un worker, y rdp/rsp dicen cual.
        teleCards([
          ["RDP ocupado", (oq.rdpBusyPct || 0).toFixed(1) + " %"],
          ["RSP ocupado", (oq.rspBusyPct || 0).toFixed(1) + " %"],
          ["CPU esperando", (oq.cpuWaitPct || 0).toFixed(1) + " %"],
        ]) +
        "<h4>Overclock aplicado</h4>" +
        teleCards([
          ["CPU", (oc.cpu || 1).toFixed(2) + "x  (" + (93.75 * (oc.cpu || 1)).toFixed(2) + " MHz)"],
          ["RSP", (oc.rsp || 1).toFixed(2) + "x  (" + (62.5 * (oc.rsp || 1)).toFixed(2) + " MHz)"],
          ["RDRAM", (oc.rdram || 1).toFixed(2) + "x"],
        ]) +
        (g.name ? "<h4>Cartucho</h4>" + teleCards([
          ["nombre", g.name], ["ID", g.cartId || "?"], ["orden", g.romOrder || "?"],
          ["entrada", hex(g.entryPoint, 8)], ["CRC1", hex(g.crc1, 8)], ["CRC2", hex(g.crc2, 8)],
          ["guardado", g.saveType || "?"],
        ]) : "");
    } else if (TELE_TAB === "cpu") {
      body.innerHTML =
        teleCards([["pc", hex(lo32(d.pc), 8)], ["instruccion", d.disasm || ""],
                   ["retiradas", (d.retired || 0).toLocaleString("es")],
                   ["halted", d.halted ? (d.haltReason || "si") : "no"]]) +
        "<h4>Registros generales</h4><div class=\"regs\">" + teleRegs(d.gpr || {}) + "</div>" +
        "<h4>COP0</h4><div class=\"regs\">" + teleRegs(d.cop0 || {}) + "</div>";
    } else if (TELE_TAB === "rcp") {
      body.innerHTML = '<div class="regs">' + teleRegs(d) + "</div>";
    } else if (TELE_TAB === "rsp") {
      const gpr = d.gpr || {};
      delete d.gpr;
      body.innerHTML = '<div class="regs">' + teleRegs(d) + "</div>" +
        "<h4>Registros escalares</h4><div class=\"regs\">" + teleRegs(gpr) + "</div>";
    } else {
      const hot = d.hot || [];
      const head =
        '<div class="telectl" style="margin:0 0 10px">' +
        `<span class="hint" style="margin-right:auto">${d.enabled ? "midiendo" : "parado"} &mdash; ` +
        `${(d.total || 0).toLocaleString("es")} muestras</span>` +
        '<button class="btn tiny" data-prof="prof.start">Arrancar</button>' +
        '<button class="btn tiny" data-prof="prof.stop">Parar</button>' +
        '<button class="btn tiny ghost" data-prof="prof.reset">Vaciar</button></div>';
      body.innerHTML = head + (hot.length
        ? "<p class=\"hint\">Bucket de 16 bytes (4 instrucciones): el codigo KSEG0 y el " +
          "mapeado por TLB que apuntan a la misma RDRAM se funden.</p>" +
          teleTable(hot, ["phys", "kseg0", "count", "%", "disasm"])
        : '<div class="teleerr">Sin muestras todavia. Pulsa "Arrancar".</div>');
      $$("#telebody [data-prof]").forEach(b => b.onclick = async () => {
        const r = await tele(b.dataset.prof);
        if (!r.ok) toast(r.error, true);
        teleTick(true);
      });
    }
    $("#telestate").textContent = d.paused === undefined ? "" :
      (d.paused ? "nucleo pausado" : "nucleo corriendo");
  } finally {
    TELE_BUSY = false;
  }
}

setInterval(() => {
  if ($("#tele-live") && $("#tele-live").checked) teleTick(false);
}, 900);

/* ==================================================================== depurador
   Encima de las mismas ordenes que ya usa el MCP: cpu.disasm / cpu.step /
   cpu.run_until / cpu.bp.* / mem.read. Todo lo que hace falta para depurar ya estaba en
   el servidor; lo unico que faltaba era ensenarlo. */
let DBG_PC = 0, DBG_BPS = [], DBG_FOLLOW = true;

const parseAddr = t => {
  const v = parseInt(String(t).trim().replace(/^0x/i, ""), 16);
  return Number.isFinite(v) ? (v >>> 0) : null;
};

/* Pausar antes de tocar nada: paso e inspeccion sobre un nucleo en marcha no significan
   nada, y ademas el candado se soltaria solo una vez por campo. */
async function dbgPause() {
  const st = await tele("status");
  if (st.ok && st.data && !st.data.paused) await tele("pause");
}

async function dbgAct(fn) {
  const body = $("#telebody");
  try { await fn(); } catch (e) { toast(String(e), true); }
  if (body) await dbgDraw();
}

async function dbgDraw() {
  const body = $("#telebody");
  const st = await tele("status");
  if (!st.ok) { body.innerHTML = `<div class="teleerr">${st.error}</div>`; return; }
  const paused = !!st.data.paused;

  const r = await tele("cpu.regs");
  if (r.ok) DBG_PC = lo32(r.data.pc);
  const bl = await tele("cpu.bp.list");
  if (bl.ok) DBG_BPS = (bl.data.breakpoints || []).map(lo32);

  const back = 0x20;                       // 8 instrucciones de contexto por arriba
  const base = (DBG_FOLLOW ? ((DBG_PC - back) >>> 0) : (parseAddr($("#dbg-at") ? $("#dbg-at").value : "") ?? 0)) & ~3;
  const dis = await tele("cpu.disasm", {addr: base, count: 40});

  const rows = dis.ok ? (dis.data.insns || []).map(i => {
    const a = lo32(i.addr);
    const cls = (a === DBG_PC ? " pc" : "") + (DBG_BPS.includes(a) ? " bp" : "");
    return `<div class="${cls.trim()}" data-a="${a}"><span class="a">${hex(a, 8)}</span>` +
           `<span class="o">${hex(i.op, 8)}</span><span>${i.text}</span></div>`;
  }).join("") : `<div class="teleerr">${dis.error || "sin desensamblado"}</div>`;

  body.innerHTML =
    '<div class="dbgbar">' +
    `<button class="btn tiny" data-d="step" data-n="1"${paused ? "" : " disabled"}>Paso</button>` +
    `<button class="btn tiny" data-d="step" data-n="10"${paused ? "" : " disabled"}>x10</button>` +
    `<button class="btn tiny" data-d="step" data-n="1000"${paused ? "" : " disabled"}>x1000</button>` +
    '<button class="btn tiny ghost" data-d="go">Correr hasta</button>' +
    '<input class="addr" id="dbg-until" placeholder="80000180" value="">' +
    '<button class="btn tiny ghost" data-d="bpadd">Punto de ruptura</button>' +
    '<input class="addr" id="dbg-bp" placeholder="80000180">' +
    '<span style="margin-left:auto"></span>' +
    `<label class="switch tiny"><input type="checkbox" id="dbg-follow"${DBG_FOLLOW ? " checked" : ""}>` +
    '<span></span> Seguir al PC</label>' +
    `<input class="addr" id="dbg-at" placeholder="direccion" value="${DBG_FOLLOW ? "" : hex(base, 8)}"${DBG_FOLLOW ? " disabled" : ""}>` +
    '<button class="btn tiny ghost" data-d="redraw">Releer</button>' +
    '</div>' +
    `<p class="hint">${paused ? "Nucleo PAUSADO." : "Nucleo CORRIENDO: el paso esta " +
      "desactivado y lo que se ve es una foto. Pausa arriba."} ` +
    'Pulsa una linea para poner o quitar un punto de ruptura.</p>' +
    '<div class="dbgcols"><div>' +
    `<div class="dis">${rows}</div>` +
    (DBG_BPS.length
      ? '<div class="bplist">' + DBG_BPS.map(b =>
          `<span data-del="${b}" title="quitar">${hex(b, 8)} &times;</span>`).join("") +
        '<span data-del="all" title="quitar todos">quitar todos</span></div>'
      : '<p class="hint">Sin puntos de ruptura.</p>') +
    '</div><div>' +
    '<div class="dbgbar" style="margin-bottom:6px">' +
    '<select id="dbg-reg">' +
    ["RDRAM", "DMEM", "IMEM", "PIF_RAM", "CART_ROM", "SAVE", "EEPROM"].map(x =>
      `<option>${x}</option>`).join("") + "</select>" +
    '<input class="addr" id="dbg-ma" placeholder="0" value="0">' +
    '<input class="addr" id="dbg-ml" style="width:70px" value="100">' +
    '<label class="switch tiny"><input type="checkbox" id="dbg-coh" checked><span></span>' +
    ' Coherente</label>' +
    '<button class="btn tiny ghost" data-d="mem">Leer</button></div>' +
    '<div class="hexd" id="dbg-hex"><span class="a">Region, direccion y longitud en ' +
    'hexadecimal. "Coherente" lee la RDRAM a traves de la cache de datos de la CPU: sin ' +
    'eso, lo que el nucleo escribio y aun no ha volcado no se ve.</span></div>' +
    '</div></div>';

  const pcRow = $("#telebody .dis .pc");
  if (pcRow) pcRow.scrollIntoView({block: "center"});
  $("#telestate").textContent = paused ? "nucleo pausado" : "nucleo corriendo";
  dbgWire();
}

function dbgWire() {
  $("#dbg-follow").onchange = e => { DBG_FOLLOW = e.target.checked; dbgDraw(); };
  $$("#telebody .dis div[data-a]").forEach(el => el.onclick = () => {
    const a = +el.dataset.a;
    dbgAct(() => tele(DBG_BPS.includes(a) ? "cpu.bp.del" : "cpu.bp.add", {addr: a}));
  });
  $$("#telebody .bplist [data-del]").forEach(el => el.onclick = () => {
    const d = el.dataset.del;
    dbgAct(() => tele("cpu.bp.del", d === "all" ? {} : {addr: +d}));
  });
  $$("#telebody [data-d]").forEach(b => b.onclick = () => {
    const k = b.dataset.d;
    if (k === "redraw") return dbgDraw();
    if (k === "step") return dbgAct(async () => {
      await dbgPause();
      const r = await tele("cpu.step", {count: +b.dataset.n});
      if (!r.ok) toast(r.error, true);
      else if (r.data.halted) toast("CPU detenida: " + (r.data.haltReason || "?"), true);
    });
    if (k === "bpadd") {
      const a = parseAddr($("#dbg-bp").value);
      if (a === null) return toast("Direccion no valida", true);
      return dbgAct(() => tele("cpu.bp.add", {addr: a}));
    }
    if (k === "go") {
      const a = parseAddr($("#dbg-until").value);
      if (a === null) return toast("Direccion no valida", true);
      return dbgAct(async () => {
        // run_until reanuda, espera y vuelve a pausar; el tope de tiempo es suyo, asi que
        // una direccion que no se alcanza no cuelga la ventana.
        const r = await tele("cpu.run_until", {addr: a, timeout_ms: 5000});
        if (!r.ok) return toast(r.error, true);
        toast(r.data.hit ? "Alcanzado " + hex(a, 8)
              : r.data.timedOut ? "Sin llegar en 5 s" : "Parado antes de llegar",
              !r.data.hit);
      });
    }
    if (k === "mem") return dbgMem();
  });
}

async function dbgMem() {
  const box = $("#dbg-hex");
  const addr = parseAddr($("#dbg-ma").value), len = parseAddr($("#dbg-ml").value);
  if (addr === null || len === null) { box.textContent = "Direccion o longitud no valida"; return; }
  const r = await tele("mem.read", {region: $("#dbg-reg").value, addr, len: Math.min(len, 0x1000),
                                    coherent: $("#dbg-coh").checked ? 1 : 0});
  if (!r.ok) { box.innerHTML = `<span class="t">${r.error}</span>`; return; }
  const raw = atob(r.blob || "");
  let out = "";
  for (let i = 0; i < raw.length; i += 16) {
    let hx = "", tx = "";
    for (let j = 0; j < 16; j++) {
      if (i + j >= raw.length) { hx += "   "; continue; }
      const c = raw.charCodeAt(i + j);
      hx += c.toString(16).padStart(2, "0") + (j === 7 ? "  " : " ");
      tx += (c >= 32 && c < 127) ? raw[i + j] : ".";
    }
    out += `<span class="a">${hex(lo32(r.data.addr) + i, 8)}</span>  ${hx} <span class="t">${
      tx.replace(/&/g, "&amp;").replace(/</g, "&lt;")}</span>\n`;
  }
  box.innerHTML = out || '<span class="a">sin datos</span>';
}

/* ==================================================================== modales */
function openModal(sel) { $(sel).classList.add("on"); }
function closeModal(el) { el.classList.remove("on"); if (PADGL) PADGL.hide(); }

/* El tema vive en un atributo del <html> y TODO lo demas sale de las variables de
   :root, asi que cambiarlo no repinta nada: el navegador reevalua los tokens solo.
   Los temas validos estan en style.css; uno desconocido cae al de fabrica. */
const THEMES = ["kestrel", "wonder"];
function applyTheme(name) {
  const th = THEMES.includes(name) ? name : "kestrel";
  document.documentElement.dataset.theme = th;
  const sel = $("#theme");
  if (sel) sel.value = th;
  return th;
}

function wire() {
  $("#theme").onchange = e => { CFG.theme = applyTheme(e.target.value); touch(); };

  $$(".tab").forEach(t => t.onclick = () => {
    $$(".tab").forEach(x => x.classList.remove("on"));
    t.classList.add("on");
    $$(".view").forEach(v => v.classList.remove("on"));
    $("#view-" + t.dataset.view).classList.add("on");
  });

  $$("#modes button").forEach(b => b.onclick = () => {
    $$("#modes button").forEach(x => x.classList.remove("on"));
    b.classList.add("on");
    MODE = b.dataset.mode;
    CFG.viewmode = MODE; touch();
    renderStage();
  });
  if (CFG.viewmode) {
    const b = $(`#modes button[data-mode="${CFG.viewmode}"]`);
    if (b) { $$("#modes button").forEach(x => x.classList.remove("on")); b.classList.add("on"); MODE = CFG.viewmode; }
  }

  $("#search").oninput = () => { SEL = 0; renderStage(); };
  $("#pick-folder").onclick = () => { openModal("#m-folder"); fbGo(CFG.romdir || ""); };
  $("#fb-ok").onclick = () => { closeModal($("#m-folder")); loadRoms(fbCur); };
  $("#btn-oc").onclick = () => { buildOC(); openModal("#m-oc"); };
  $("#btn-video").onclick = () => { buildVideo(); openModal("#m-video"); };
  $("#btn-input").onclick = () => { openModal("#m-input"); if (PADGL) PADGL.show(); };
  $("#btn-console").onclick = () => { openModal("#m-console"); pollStatus(); };
  $("#btn-tele").onclick = () => { openModal("#m-tele"); teleTick(true); };
  $$("#teletabs button").forEach(b => b.onclick = () => {
    $$("#teletabs button").forEach(x => x.classList.remove("on"));
    b.classList.add("on"); TELE_TAB = b.dataset.t; teleTick(true);
  });
  $$("#m-tele [data-run]").forEach(b => b.onclick = async () => {
    const r = await tele(b.dataset.run);
    if (!r.ok) toast(r.error, true); else teleTick(true);
  });
  $("#launch").onclick = launch;
  cardWire();
  $("#stop").onclick = async () => { await api("/api/stop", {}); toast("Emulador parado"); };
  $("#con-stop").onclick = async () => { await api("/api/stop", {}); toast("Emulador parado"); };
  $("#oc-link").onchange = e => {
    CFG.oc_link = e.target.checked;
    if (e.target.checked) CFG.oc_cpu = CFG.oc_rsp = CFG.oc_rdram = Number(CFG.oc_all || 1);
    touch(); buildOC();
  };
  $("#oc-throttle").onchange = e => { CFG.throttle = e.target.value; touch(); render(); };

  $$(".modal").forEach(m => {
    m.onclick = e => { if (e.target === m) closeModal(m); };
    $$(".x, .x2", m).forEach(b => b.onclick = () => closeModal(m));
  });

  document.addEventListener("keydown", e => {
    if (capturing) return;
    if (e.key === "Escape") { $$(".modal.on").forEach(closeModal); return; }
    // Dentro de la ficha, Intro juega (salvo escribiendo en un campo)
    if (e.key === "Enter" && $("#m-game.on") && !/^(INPUT|TEXTAREA|SELECT|BUTTON)$/.test(e.target.tagName)) {
      closeModal($("#m-game")); launch(); return;
    }
    if ($(".modal.on")) return;
    const list = filtered();
    if (e.key === "ArrowRight" || e.key === "ArrowDown") {
      if (SEL + 1 < list.length) { SEL++; softLayout(list); }
    } else if (e.key === "ArrowLeft" || e.key === "ArrowUp") {
      if (SEL > 0) { SEL--; softLayout(list); }
    } else if (e.key === "Enter") activate();
  });
}

/* En las vistas de caratula basta mover el carrusel; en las demas hay que repintar. */
function softLayout(list) {
  if (LIB3D && LAYOUT3D[MODE]) { LIB3D.setIndex(SEL); updateDock(); }
  else if (MODE === "cover") { layoutCover(list); updateDock(); }
  else if (MODE === "wheel") { layoutWheel(); updateDock(); }
  else renderStage();
}
