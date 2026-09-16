/* kestrel64 launcher — ficha de juego.
   Elegir un juego ya seleccionado (clic, Intro, clic en la caja 3D del centro) abre esta
   ficha en vez de arrancarlo, salvo que el perfil diga "jugar directo". Doble clic y el
   boton Jugar del muelle siguen arrancando sin pasar por aqui.
   Todo lo que ensena sale de /api/game, que mira los ficheros con los MISMOS nombres que
   usa el emulador (ver tools/launcher/gamecard.py). */
"use strict";

let CARD = null, CARD_TAB = "info", CARD_MAN = 0;

const esc = s => String(s == null ? "" : s).replace(/[&<>"']/g,
  c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;"}[c]));

function fmtSize(n) {
  if (n < 1024) return n + " B";
  if (n < 1048576) return (n / 1024).toFixed(n < 10240 ? 1 : 0) + " KB";
  return (n / 1048576).toFixed(1) + " MB";
}
function fmtDate(t) {
  if (!t) return "nunca";
  const d = new Date(t * 1000);
  return d.toLocaleDateString("es-ES") + " " + d.toLocaleTimeString("es-ES", {hour: "2-digit", minute: "2-digit"});
}
function fmtTime(s) {
  s = Math.round(s || 0);
  if (s < 60) return s + " s";
  const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60;
  return h ? `${h} h ${m} min` : `${m} min`;
}

/* Lo que hace "elegir" el juego ya seleccionado. */
function activate() {
  if (CFG.select_action === "play") launch();
  else openCard();
}

async function openCard(tab) {
  const r = filtered()[SEL];
  if (!r) return;
  CARD_TAB = tab || CARD_TAB || "info";
  CARD_MAN = 0;
  $("#card-title").textContent = r.title;
  $("#card-body").innerHTML = `<p class="hint pad">Leyendo la ficha...</p>`;
  $("#card-art").src = artUrl(r);
  $("#card-art").onerror = () => $("#card-art").removeAttribute("src");
  openModal("#m-game");
  await cardLoad(r.path);
}

async function cardLoad(path) {
  const c = await api("/api/game?rom=" + encodeURIComponent(path));
  if (c.error) { $("#card-body").innerHTML = `<p class="hint pad">${esc(c.error)}</p>`; return; }
  CARD = c;
  cardSide();
  cardTabs();
}

function cardSide() {
  const m = CARD.meta || {};
  $("#card-stats").innerHTML = `
    <div><b>${m.plays || 0}</b><span>partidas</span></div>
    <div><b>${fmtTime(m.seconds)}</b><span>jugado</span></div>
    <div class="wide"><b>${fmtDate(m.last)}</b><span>ultima vez</span></div>`;
  const fav = $("#card-fav");
  fav.classList.toggle("on", !!m.favorite);
  fav.textContent = m.favorite ? "★ Favorito" : "☆ Favorito";
  $("#card-running").hidden = !CARD.running;
  $("#card-path").textContent = CARD.path;
  $("#card-direct").checked = CFG.select_action === "play";
  const n = CARD.cheats.list.filter(x => x.on && x.codes.length).length;
  $$("#cardtabs button").forEach(b => {
    const t = b.dataset.t;
    const cnt = t === "manual" ? CARD.manuals.length : t === "cheats" ? CARD.cheats.list.length
              : t === "saves" ? CARD.saves.length : 0;
    b.classList.toggle("on", t === CARD_TAB);
    const s = $("small", b);
    if (s) s.textContent = t === "cheats" && cnt ? `${n}/${cnt}` : cnt ? String(cnt) : "";
  });
}

function cardTabs() {
  $$("#cardtabs button").forEach(b => b.classList.toggle("on", b.dataset.t === CARD_TAB));
  ({info: cardInfo, manual: cardManual, cheats: cardCheats, saves: cardSaves}[CARD_TAB] || cardInfo)();
}

/* ------------------------------------------------------------------ informacion */
const META = [
  ["year", "Anio", "1996"], ["developer", "Desarrollo", "Nintendo EAD"],
  ["publisher", "Distribuidor", "Nintendo"], ["genre", "Genero", "Plataformas 3D"],
  ["players", "Jugadores", "1"],
];
function cardInfo() {
  const h = CARD.header || {}, m = CARD.meta || {};
  const rows = [
    ["Nombre interno", h.name], ["ID de cartucho", h.cart || "-"],
    ["Region", h.region_label ? `${h.region_label} (${h.region})` : "-"],
    ["CRC de cabecera", h.crc], ["Volcado", (h.fmt || "?") + " · " + fmtSize(h.size || 0)],
    ["Fichero base", CARD.base],
  ];
  $("#card-body").innerHTML = `
    <div class="cgrid">
      <section><h5>Cartucho</h5>
        <table class="kv">${rows.map(([k, v]) => `<tr><th>${k}</th><td>${esc(v)}</td></tr>`).join("")}</table>
        <p class="note0">Datos de la cabecera de la propia ROM, en el orden de bytes del cartucho.</p>
      </section>
      <section><h5>Ficha</h5>
        ${META.map(([k, l, ph]) => `<label class="fld"><span>${l}</span>
          <input data-k="${k}" value="${esc(m[k] || "")}" placeholder="${esc(ph)}"></label>`).join("")}
        <label class="fld col"><span>Notas</span>
          <textarea data-k="notes" rows="5" placeholder="Lo que quieras recordar de este juego: contrasenas, por donde ibas, que truco usar...">${esc(m.notes || "")}</textarea></label>
        <p class="note0" id="card-saved">Se guarda solo, por CRC: renombrar o comprimir la ROM no pierde la ficha.</p>
      </section>
    </div>`;
  $$("#card-body [data-k]").forEach(el => el.oninput = () => cardMetaSave());
}

function cardMetaSave() {
  clearTimeout(cardMetaSave._t);
  cardMetaSave._t = setTimeout(async () => {
    const meta = {};
    $$("#card-body [data-k]").forEach(el => meta[el.dataset.k] = el.value);
    const r = await api("/api/game/meta", {rom: CARD.path, meta});
    if (!r.ok) { toast(r.error, true); return; }
    CARD.meta = Object.assign(CARD.meta || {}, r.meta);
    const s = $("#card-saved");
    if (s) s.textContent = "Guardado.";
  }, 450);
}

/* ------------------------------------------------------------------ manual */
function cardManual() {
  const ms = CARD.manuals;
  const url = i => "/api/manual?rom=" + encodeURIComponent(CARD.path) + "&i=" + i;
  let view = `<div class="mempty">
      <b>Sin manual</b>
      <p>Pon el manual al lado de la ROM o en su carpeta <code>manuals/</code>, con un nombre que
         empiece como el fichero (<code>${esc(CARD.base)}...</code>) o como el nombre interno.
         Vale PDF, HTML, texto o imagen. O sueltalo aqui.</p></div>`;
  if (ms.length) {
    const m = ms[Math.min(CARD_MAN, ms.length - 1)];
    view = /^(png|jpe?g|webp)$/.test(m.ext)
      ? `<div class="mimg"><img src="${url(m.i)}" alt=""></div>`
      : `<iframe class="mframe" src="${url(m.i)}" title="manual"></iframe>`;
  }
  $("#card-body").innerHTML = `
    <div class="mbar">
      ${ms.map(m => `<button class="mchip${m.i === CARD_MAN ? " on" : ""}" data-i="${m.i}">
          ${esc(m.file)} <small>${fmtSize(m.size)}</small></button>`).join("")}
      <span class="grow"></span>
      ${ms.length ? `<a class="btn tiny ghost" target="_blank" rel="noopener" href="${url(Math.min(CARD_MAN, ms.length - 1))}">Abrir aparte</a>` : ""}
      <label class="btn tiny">Anadir manual<input type="file" id="man-file" hidden
        accept=".pdf,.html,.htm,.txt,.md,.png,.jpg,.jpeg,.webp"></label>
    </div>
    <div class="mview" id="mview">${view}</div>`;
  $$("#card-body .mchip").forEach(b => b.onclick = () => { CARD_MAN = +b.dataset.i; cardManual(); });
  $("#man-file").onchange = e => e.target.files[0] && manualUpload(e.target.files[0]);
  const v = $("#mview");
  v.ondragover = e => { e.preventDefault(); v.classList.add("drop"); };
  v.ondragleave = () => v.classList.remove("drop");
  v.ondrop = e => {
    e.preventDefault(); v.classList.remove("drop");
    if (e.dataTransfer.files[0]) manualUpload(e.dataTransfer.files[0]);
  };
}

async function manualUpload(file) {
  const u = "/api/game/manual?rom=" + encodeURIComponent(CARD.path) + "&name=" + encodeURIComponent(file.name);
  const r = await (await fetch(u, {method: "POST", body: file})).json();
  if (!r.ok) { toast(r.error || "no se pudo guardar", true); return; }
  toast("Manual guardado: " + r.file);
  await cardLoad(CARD.path);
  const m = CARD.manuals.find(x => x.file === r.file);
  if (m) { CARD_MAN = m.i; cardManual(); }
}

/* ------------------------------------------------------------------ trucos */
function cardCheats() {
  const ch = CARD.cheats;
  const warn = ch.override
    ? `<p class="cwarn">El perfil fuerza otro fichero de trucos (<code>${esc(ch.override)}</code>,
         Ajustes &rarr; Cartucho). Mientras este puesto, el emulador NO lee este.</p>` : "";
  const live = CARD.running
    ? `<p class="cwarn soft">El juego esta en marcha: los cambios cuentan desde el proximo arranque.</p>` : "";
  const rows = ch.list.map(c => `
    <div class="cheat${c.on ? " on" : ""}${c.codes.length ? "" : " void"}">
      <label class="switch"><input type="checkbox" data-id="${c.id}" ${c.on ? "checked" : ""}
        ${c.codes.length ? "" : "disabled"}><span></span></label>
      <div class="cname"><b>${esc(c.name)}</b>
        <small>${c.codes.length} ${c.codes.length === 1 ? "linea" : "lineas"}
          ${c.inert ? ` · <em>${c.inert} sin efecto aqui</em>` : ""}
          ${c.bad ? ` · <em>${c.bad} ilegibles</em>` : ""}</small></div>
      <code class="ccodes" title="${esc(c.codes.join("\n"))}">${esc(c.codes.slice(0, 2).join("  "))}${c.codes.length > 2 ? " …" : ""}</code>
      <button class="cdel" data-del="${c.id}" title="Borrar este truco del fichero">&times;</button>
    </div>`).join("");
  $("#card-body").innerHTML = `
    ${warn}${live}
    <div class="cbar">
      <span>${ch.exists ? `<code>${esc(ch.file)}</code>, al lado de la ROM: el emulador lo carga solo.`
                        : `No hay <code>${esc(ch.file)}</code> todavia. El primer truco que anadas lo crea.`}</span>
      ${ch.list.length ? `<button class="btn tiny ghost" id="ch-none">Apagar todos</button>` : ""}
    </div>
    <div class="cheats">${rows || `<p class="hint pad">Sin trucos para este juego.</p>`}</div>
    <details class="cadd"${ch.list.length ? "" : " open"}><summary>Anadir truco GameShark</summary>
      <div class="caddf">
        <input id="ch-name" placeholder="Nombre (p. ej. Vidas infinitas)">
        <textarea id="ch-codes" rows="4" placeholder="8033B21D 0064&#10;81xxxxxx yyyy"></textarea>
        <div class="caddb"><span>Familias 80/81/A0/A1/D0-D3/50. Ver docs/CHEATS.md.</span>
          <button class="btn tiny" id="ch-add">Anadir</button></div>
      </div>
    </details>`;
  $$("#card-body .cheat input").forEach(i => i.onchange = () =>
    cheatSet({[i.dataset.id]: i.checked}));
  $$("#card-body [data-del]").forEach(b => b.onclick = async () => {
    const c = ch.list[+b.dataset.del];
    if (!confirm(`Borrar "${c.name}" de ${ch.file}?`)) return;
    cheatResult(await api("/api/game/cheat/del", {rom: CARD.path, id: c.id}));
  });
  const none = $("#ch-none");
  if (none) none.onclick = () => cheatSet(Object.fromEntries(ch.list.map(c => [c.id, false])));
  $("#ch-add").onclick = async () => cheatResult(await api("/api/game/cheat/add",
    {rom: CARD.path, name: $("#ch-name").value, codes: $("#ch-codes").value}), true);
}

async function cheatSet(states) {
  cheatResult(await api("/api/game/cheats", {rom: CARD.path, states}));
}
function cheatResult(r, added) {
  if (!r.ok) { toast(r.error || "no se pudo escribir el fichero de trucos", true); cardCheats(); return; }
  CARD.cheats = Object.assign(r.cheats, {override: CARD.cheats.override});
  if (added) toast("Truco anadido");
  cardSide(); cardCheats();
}

/* ------------------------------------------------------------------ partidas */
function cardSaves() {
  const s = CARD.saves;
  $("#card-body").innerHTML = s.length ? `
    <table class="tbl">
      <thead><tr><th>Tipo</th><th>Fichero</th><th>Tamano</th><th>Modificado</th></tr></thead>
      <tbody>${s.map(x => `<tr><td>${esc(x.label)}</td><td><code>${esc(x.file)}</code></td>
        <td>${fmtSize(x.size)}</td><td>${fmtDate(x.mtime)}</td></tr>`).join("")}</tbody>
    </table>
    <p class="note0 pad">Los mismos ficheros que abre el emulador al lado de la ROM: partida del
       cartucho (.eep/.sra/.fla), Controller Pak por mando (.mpk) y estados rapidos (.st0-.st9).</p>`
    : `<p class="hint pad">Este juego no tiene partidas ni estados guardados todavia.</p>`;
}

/* ------------------------------------------------------------------ cableado */
function cardWire() {
  $$("#cardtabs button").forEach(b => b.onclick = () => { CARD_TAB = b.dataset.t; cardTabs(); });
  $("#card-play").onclick = () => { closeModal($("#m-game")); launch(); };
  $("#card-fav").onclick = async () => {
    const r = await api("/api/game/meta", {rom: CARD.path, meta: {favorite: !(CARD.meta || {}).favorite}});
    if (r.ok) { CARD.meta = Object.assign(CARD.meta || {}, r.meta); cardSide(); }
  };
  $("#card-direct").onchange = e => {
    CFG.select_action = e.target.checked ? "play" : "card"; touch();
  };
  $("#dock-card").onclick = () => openCard();
}
