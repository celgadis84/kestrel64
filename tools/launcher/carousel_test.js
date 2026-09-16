/* carousel_test.js -- comprobacion sin navegador del carrusel 3D de la biblioteca.
 *
 *   node tools/launcher/carousel_test.js
 *
 * El carrusel dibuja K cajas y recicla las casillas por modulo, que es lo que permite tener
 * mil juegos con veinticinco cajas. Dos cosas rompen eso en silencio y no se ven en una
 * captura: que dos casillas muestren el mismo juego (una caja duplicada en pantalla) y que
 * el juego elegido no le toque a ninguna casilla (el centro del carrusel vacio). Aqui se
 * comprueban las dos para toda la lista, y de paso que la malla de K cajas sigue estando
 * troceada por casillas con su textura propia.
 */
"use strict";

const path = require("path");
const GL = require(path.join(__dirname, "web", "gl.js"));
const MODELS = require(path.join(__dirname, "web", "models.js"));
const SCENE3D = require(path.join(__dirname, "web", "scene3d.js"));

let fails = 0;
const ok = (cond, msg) => { if (!cond) { console.error("FALLO: " + msg); fails++; } };

/* ------------------------------------------------- reparto de juegos por casilla */
const KMAX = 25;
for (const n of [0, 1, 2, 7, 24, 25, 26, 60, 397]) {
  const K = Math.max(1, Math.min(n || 1, KMAX));
  let bad = 0, miss = 0, dup = 0, holes = 0;
  for (let c = 0; c < n; c++) {
    const seen = new Map();
    let hit = false;
    for (let s = 0; s < K; s++) {
      const i = SCENE3D.slotItem(n, K, s, c);
      if (i < 0) continue;
      if (i >= n) bad++;
      if (seen.has(i)) dup++;
      seen.set(i, s);
      if (i === c) hit = true;
    }
    if (!hit) miss++;
    // La ventana tiene que ser un tramo seguido: si tuviera huecos se verian claros en el
    // carrusel al desplazarse.
    const idx = [...seen.keys()].sort((a, b) => a - b);
    for (let j = 1; j < idx.length; j++) if (idx[j] !== idx[j - 1] + 1) holes++;
    // Y tiene que cubrir todo lo que se puede ver a cada lado.
    const want = Math.min(n, K);
    if (idx.length !== want && c >= K && c < n - K) ok(false, `n=${n} c=${c}: ${idx.length} casillas vivas, se esperaban ${want}`);
  }
  ok(bad === 0, `n=${n}: ${bad} indices fuera de la lista`);
  ok(dup === 0, `n=${n}: ${dup} juegos repartidos a dos casillas`);
  ok(miss === 0, `n=${n}: ${miss} centros sin casilla (el juego elegido no se dibujaria)`);
  ok(holes === 0, `n=${n}: ${holes} huecos en la ventana`);
}

// Con la lista vacia no hay casilla que valga.
ok(SCENE3D.slotItem(0, 25, 0, 0) === -1, "lista vacia: deberia devolver -1");
// Un centro fraccionario (el carrusel a medio camino) reparte como el entero mas cercano.
for (let s = 0; s < 25; s++)
  ok(SCENE3D.slotItem(60, 25, s, 30.4) === SCENE3D.slotItem(60, 25, s, 30),
     `centro fraccionario: la casilla ${s} cambia de juego a mitad de animacion`);

/* --------------------------------------------------------- la malla de K cajas */
const K = 25;
const b = new GL.Builder();
const slots = [];
for (let s = 0; s < K; s++) {
  const p0 = b.parts.length;
  b.merge(MODELS.buildBox({ coverTex: "cov" + s }), "s" + s + ":");
  slots.push({ s, p0, p1: b.parts.length });
}
const mesh = b.build();

ok(mesh.pos.length / 3 <= 65536, `${mesh.pos.length / 3} vertices: no caben en Uint16 (WebGL1)`);
let maxIdx = 0;
for (let i = 0; i < mesh.idx.length; i++) maxIdx = Math.max(maxIdx, mesh.idx[i]);
ok(maxIdx < mesh.pos.length / 3, `indice ${maxIdx} fuera de rango`);
ok(mesh.parts.length === slots[K - 1].p1, "tramos perdidos al juntar las cajas");

slots.forEach(sl => {
  const parts = mesh.parts.slice(sl.p0, sl.p1);
  ok(parts.length > 0, `casilla ${sl.s}: sin tramos`);
  const cover = parts.filter(p => p.tex === "cov" + sl.s);
  ok(cover.length === 1, `casilla ${sl.s}: ${cover.length} tramos con su caratula, se esperaba 1`);
  ok(cover.length === 1 && cover[0].id === "s" + sl.s + ":COVER",
     `casilla ${sl.s}: la portada no lleva el id de su casilla (seleccion por color rota)`);
  // Ningun tramo de esta casilla puede pisar el rango de indices de otra: cada caja se
  // dibuja con su matriz, y si los rangos se solapasen se colocarian juntas.
  parts.forEach(p => {
    ok(p.start >= 0 && p.start + p.count <= mesh.idx.length, `casilla ${sl.s}: rango invalido`);
  });
});

// Los rangos de indices de las casillas van seguidos y sin solapar.
let cursor = 0, overlap = 0;
mesh.parts.forEach(p => { if (p.start < cursor) overlap++; cursor = p.start + p.count; });
ok(overlap === 0, `${overlap} tramos solapados`);

/* ----------------------------------------------------------- matriz por tramo */
// Lo que hace posible el carrusel sin rehacer geometria: cada tramo puede llevar su matriz.
mesh.parts[0].mat = GL.M4.trans(3, 0, 0);
ok(mesh.parts[0].mat[12] === 3, "la matriz por tramo no se queda puesta");

console.log(`${K} cajas: ${mesh.pos.length / 3} vertices, ${mesh.idx.length / 3} triangulos, ` +
            `${mesh.parts.length} tramos`);
console.log(fails ? `\n${fails} fallos` : "\nTODO BIEN");
process.exit(fails ? 1 : 0);
