/* models_test.js -- comprobacion sin navegador de la geometria del lanzador.
 *
 *   node tools/launcher/models_test.js
 *
 * No dibuja: valida lo que un fallo de geometria rompe en silencio -- que estan todos los
 * ids de control que la interfaz espera poder pinchar, que los indices caben en Uint16
 * (WebGL1 no tiene mas), que las normales son unitarias, que los arrays cuadran entre si y
 * que ningun triangulo sale degenerado o del volumen razonable de la pieza.
 */
"use strict";

const path = require("path");
const GL = require(path.join(__dirname, "web", "gl.js"));
const MODELS = require(path.join(__dirname, "web", "models.js"));

let fails = 0;
const ok = (cond, msg) => { if (!cond) { console.error("FALLO: " + msg); fails++; } };

function checkMesh(name, m, expectIds, bound) {
  const nv = m.pos.length / 3;
  ok(m.nrm.length === nv * 3, `${name}: normales descuadradas`);
  ok(m.col.length === nv * 3, `${name}: colores descuadrados`);
  ok(m.uv.length === nv * 2, `${name}: uv descuadrados`);
  ok(nv > 0 && m.idx.length > 0, `${name}: malla vacia`);
  ok(nv <= 65536, `${name}: ${nv} vertices, no caben en Uint16`);

  let maxIdx = 0;
  for (let i = 0; i < m.idx.length; i++) maxIdx = Math.max(maxIdx, m.idx[i]);
  ok(maxIdx < nv, `${name}: indice ${maxIdx} fuera de rango (${nv} vertices)`);
  ok(m.idx.length % 3 === 0, `${name}: indices no multiplo de 3`);

  let badN = 0;
  for (let i = 0; i < nv; i++) {
    const l = Math.hypot(m.nrm[i * 3], m.nrm[i * 3 + 1], m.nrm[i * 3 + 2]);
    if (!(Math.abs(l - 1) < 1e-3)) badN++;
    if (!Number.isFinite(m.pos[i * 3]) || !Number.isFinite(m.pos[i * 3 + 1]) || !Number.isFinite(m.pos[i * 3 + 2]))
      badN += 1000;
  }
  ok(badN === 0, `${name}: ${badN} normales no unitarias o vertices no finitos`);

  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < nv; i++) for (let k = 0; k < 3; k++) {
    lo[k] = Math.min(lo[k], m.pos[i * 3 + k]); hi[k] = Math.max(hi[k], m.pos[i * 3 + k]);
  }
  const size = [hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]];
  for (let k = 0; k < 3; k++)
    ok(Math.abs(size[k] - bound[k]) < bound[k] * 0.15 + 0.3,
       `${name}: eje ${"XYZ"[k]} mide ${size[k].toFixed(2)}, se esperaba ~${bound[k]}`);

  // Cobertura de partes: todos los tramos suman exactamente el total de indices y no se
  // pisan entre si (un solape significaria que un tramo se cerro mal).
  let sum = 0, prevEnd = 0, lastId = null;
  const seen = new Set();
  for (const p of m.parts) {
    ok(p.start === prevEnd, `${name}: hueco o solape de tramos en ${p.id || "(sin id)"}`);
    ok(p.count > 0, `${name}: tramo vacio ${p.id}`);
    prevEnd = p.start + p.count; sum += p.count;
    // Un control puede ocupar varios tramos seguidos (el stick son tres colores), pero si un
    // id reaparece MAS TARDE es que se ha reutilizado por error en otro control.
    if (p.id) {
      ok(!seen.has(p.id) || p.id === lastId, `${name}: id repetido ${p.id}`);
      seen.add(p.id);
    }
    lastId = p.id;
  }
  ok(sum === m.idx.length, `${name}: los tramos suman ${sum} de ${m.idx.length} indices`);

  for (const id of expectIds) ok(seen.has(id), `${name}: falta el id ${id}`);

  console.log(`${name}: ${nv} vertices, ${m.idx.length / 3} triangulos, ${m.parts.length} tramos, ` +
              `caja ${size.map(v => v.toFixed(1)).join(" x ")} cm`);
}

// Los mismos ids que options.py declara para el mando.
const PAD_IDS = ["A", "B", "START", "Z", "L", "R", "CU", "CD", "CL", "CR",
                 "DU", "DD", "DL", "DR", "STICK"];

checkMesh("mando",    MODELS.buildController(), PAD_IDS, [17.6, 9.4, 7.3]);
checkMesh("cartucho", MODELS.buildCart(),       ["LABEL"], [9.3, 11.8, 2.5]);
checkMesh("caja",     MODELS.buildBox(),        ["COVER", "BACK"], [13.5, 19.0, 3.0]);

// El estante junta caja y cartucho en una sola malla: si `merge` descoloca un tramo, la
// seleccion por color senala la pieza equivocada, asi que se comprueba igual que las demas.
{
  const bb = new GL.Builder();
  bb.push(GL.M4.trans(-2.6, 0, 0)).merge(MODELS.buildBox()).pop();
  bb.push(GL.M4.trans(5.4, -3.4, 3.2)).merge(MODELS.buildCart(), "cart:").pop();
  checkMesh("estante", bb.build(), ["COVER", "BACK", "cart:LABEL"], [19.4, 19.0, 5.9]);
}

/* Volumen firmado: si una primitiva tiene los triangulos al reves, el culling se come esa
   cara y la pieza sale hueca o a medias (asi salieron los botones como medias lunas hasta
   que se vio que las tapas del cilindro miraban hacia dentro). Cerrada y bien orientada =
   volumen positivo y parecido al analitico. */
function volume(m) {
  const V = m.verts || m.pos, T = m.tris || m.idx;
  let v = 0;
  for (let i = 0; i < T.length; i += 3) {
    const p = [0, 1, 2].map(k => [V[T[i + k] * 3], V[T[i + k] * 3 + 1], V[T[i + k] * 3 + 2]]);
    const u = [p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]];
    const w = [p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]];
    const n = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]];
    v += (p[0][0] * n[0] + p[0][1] * n[1] + p[0][2] * n[2]) / 6;
  }
  return v;
}

{
  const PI = Math.PI;
  const cases = [
    ["cilindro",   GL.cylinder(1, 2, 32),                          2 * PI],
    ["cono",       GL.cylinder(1, 3, 32, 0),                       PI],
    ["esfera",     GL.sphere(1, 32),                               4 / 3 * PI],
    ["extrude",    GL.extrude([[-1, -1], [1, -1], [1, 1], [-1, 1]], 1), 4],
    ["roundedBox", GL.roundedBox(2, 2, 2, 0.001, 1),               8],
    ["roundPrism", GL.roundPrism([[-1, -1], [1, -1], [1, 1], [-1, 1]], 2, 0.001, 2), 8],
    ["loft",       GL.loft([-1, 1].map(y => {
                     const ring = [];
                     for (let i = 0; i < 24; i++) {
                       const a = i / 24 * 2 * PI;
                       ring.push([Math.cos(a), y, Math.sin(a)]);
                     }
                     return ring;
                   }), { capStart: true, capEnd: true }), 2 * PI],
  ];
  for (const [name, mesh, want] of cases) {
    const v = volume(mesh);
    ok(v > 0, `${name}: volumen ${v.toFixed(2)} <= 0, los triangulos estan al reves`);
    ok(Math.abs(v - want) < want * 0.25,
       `${name}: volumen ${v.toFixed(2)}, se esperaba ~${want.toFixed(2)}`);
  }
}

// Las piezas del lanzador tambien cierran hacia fuera.
for (const [name, mesh] of [["mando", MODELS.buildController()], ["cartucho", MODELS.buildCart()],
                            ["caja", MODELS.buildBox()]])
  ok(volume(mesh) > 0, `${name}: volumen negativo, alguna parte esta del reves`);

console.log(fails ? `\n${fails} fallos` : "\nOK");
process.exit(fails ? 1 : 0);
