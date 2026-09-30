/* models.js -- geometria propia: mando, cartucho y caja de N64.
 *
 * Todo se genera en codigo. Las MEDIDAS del mando y del cartucho salen de dos escaneos de
 * carcasas para impresora 3D (fuentes y licencias en docs/LAUNCHER.md, "Los modelos 3D"):
 * de ahi se sacaron el contorno, la altura de la costura, las alturas de las caras de
 * arriba y de abajo en una rejilla de 0,5 cm y el centro y tamano de cada agujero de
 * boton. Con esos numeros se levanta aqui una geometria nueva; ninguna malla descargada
 * entra en el programa.
 *
 * Medidas en centimetros, tal cual el aparato: el mando mide 16.0 x 15.4 cm visto desde
 * arriba, el cartucho 11.6 x 7.64 x 1.84 y la caja de carton 19.0 x 13.3 x 2.8.
 *
 * Ejes: X derecha, Y arriba, Z hacia el jugador.
 */
"use strict";

const MODELS = (() => {

const G = (typeof GL !== "undefined") ? GL : require("./gl.js");
const { M4, Builder, roundedBox, cylinder, extrude, roundPrism, polyInset, chartArea } = G;

const C = {
  body:    [0.760, 0.752, 0.723],   // el gris hueso del mando original
  well:    [0.205, 0.210, 0.228],   // el hueco oscuro alrededor de cada boton
  dpad:    [0.255, 0.262, 0.285],
  stick:   [0.320, 0.328, 0.350],
  a:       [0.145, 0.290, 0.700],
  b:       [0.075, 0.500, 0.270],
  c:       [0.870, 0.720, 0.130],
  start:   [0.720, 0.130, 0.130],
  cart:    [0.290, 0.298, 0.320],
  cartLbl: [0.880, 0.880, 0.870],
  box:     [0.180, 0.190, 0.230],
  boxEdge: [0.860, 0.200, 0.140],
};

// Rectangulo en el plano XY mirando a +Z, con UV completo. Para etiquetas y portadas.
function plane(w, h) {
  return {
    verts: [-w / 2, -h / 2, 0, w / 2, -h / 2, 0, w / 2, h / 2, 0, -w / 2, h / 2, 0],
    norms: [0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1],
    uvs:   [0, 1, 1, 1, 1, 0, 0, 0],
    tris:  [0, 1, 2, 0, 2, 3],
  };
}

// Un brazo de la cruceta: rectangulo que arranca en el centro y sale hacia (dx, dz).
function dpadArm(len, wide, dx, dz) {
  const p = [];
  if (dx) {                                   // brazo horizontal
    const s = Math.sign(dx);
    p.push([0, -wide / 2], [s * len, -wide / 2], [s * len, wide / 2], [0, wide / 2]);
    if (s < 0) p.reverse();
  } else {
    const s = Math.sign(dz);
    p.push([-wide / 2, 0], [-wide / 2, s * len], [wide / 2, s * len], [wide / 2, 0]);
    if (s > 0) p.reverse();
  }
  return p;
}

/* ------------------------------------------------------- utilidades de malla libre */
// Cierra una malla cualquiera: la orienta hacia fuera por su volumen firmado (negativo =
// triangulos al reves) y le calcula normales suaves ponderadas por area.
function solid(verts, tris) {
  let vol = 0;
  const P = i => [verts[i * 3], verts[i * 3 + 1], verts[i * 3 + 2]];
  for (let i = 0; i < tris.length; i += 3) {
    const a = P(tris[i]), b = P(tris[i + 1]), c = P(tris[i + 2]);
    vol += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
           a[2] * (b[0] * c[1] - b[1] * c[0]);
  }
  if (vol < 0) for (let i = 0; i < tris.length; i += 3) {
    const t = tris[i + 1]; tris[i + 1] = tris[i + 2]; tris[i + 2] = t;
  }
  const norms = new Array(verts.length).fill(0);
  for (let i = 0; i < tris.length; i += 3) {
    const ia = tris[i] * 3, ib = tris[i + 1] * 3, ic = tris[i + 2] * 3;
    const ux = verts[ib] - verts[ia], uy = verts[ib + 1] - verts[ia + 1], uz = verts[ib + 2] - verts[ia + 2];
    const vx = verts[ic] - verts[ia], vy = verts[ic + 1] - verts[ia + 1], vz = verts[ic + 2] - verts[ia + 2];
    const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    for (const k of [ia, ib, ic]) { norms[k] += nx; norms[k + 1] += ny; norms[k + 2] += nz; }
  }
  for (let i = 0; i < norms.length; i += 3) {
    const l = Math.hypot(norms[i], norms[i + 1], norms[i + 2]);
    if (l > 1e-12) { norms[i] /= l; norms[i + 1] /= l; norms[i + 2] /= l; }
    else { norms[i] = 0; norms[i + 1] = 1; norms[i + 2] = 0; }
  }
  return { verts, norms, uvs: new Array(verts.length / 3 * 2).fill(0), tris };
}

// Tubo cerrado a partir de anillos del mismo numero de puntos, con tapas en abanico. Da
// igual hacia donde avance: `solid` lo orienta.
function tube(rings) {
  const m = rings.length, n = rings[0].length, verts = [], tris = [];
  for (const r of rings) for (const p of r) verts.push(p[0], p[1], p[2]);
  for (let j = 0; j < m - 1; j++) for (let i = 0; i < n; i++) {
    const i2 = (i + 1) % n, q = j * n, w = (j + 1) * n;
    tris.push(q + i, q + i2, w + i, q + i2, w + i2, w + i);
  }
  for (const [j, s] of [[0, -1], [m - 1, 1]]) {
    const c = [0, 0, 0];
    for (const p of rings[j]) { c[0] += p[0] / n; c[1] += p[1] / n; c[2] += p[2] / n; }
    const ci = verts.length / 3;
    verts.push(c[0], c[1], c[2]);
    for (let i = 0; i < n; i++) {
      const a = j * n + i, b = j * n + (i + 1) % n;
      if (s > 0) tris.push(ci, a, b); else tris.push(ci, b, a);
    }
  }
  return solid(verts, tris);
}

// Triangulacion de Delaunay (Bowyer-Watson) de puntos [x, z]. Devuelve los triangulos
// siempre en sentido antihorario en el plano (x, z).
function delaunay(P) {
  const n = P.length;
  let x0 = Infinity, z0 = Infinity, x1 = -Infinity, z1 = -Infinity;
  for (const p of P) { x0 = Math.min(x0, p[0]); x1 = Math.max(x1, p[0]); z0 = Math.min(z0, p[1]); z1 = Math.max(z1, p[1]); }
  const cx = (x0 + x1) / 2, cz = (z0 + z1) / 2, d = Math.max(x1 - x0, z1 - z0) * 20;
  const pts = P.concat([[cx - d, cz - d], [cx + d, cz - d], [cx, cz + d]]);
  const mk = (a, b, c) => {
    const A = pts[a], B = pts[b], Cc = pts[c];
    const D = 2 * (A[0] * (B[1] - Cc[1]) + B[0] * (Cc[1] - A[1]) + Cc[0] * (A[1] - B[1]));
    if (Math.abs(D) < 1e-12) return { a, b, c, x: 0, z: 0, r2: Infinity };
    const a2 = A[0] * A[0] + A[1] * A[1], b2 = B[0] * B[0] + B[1] * B[1], c2 = Cc[0] * Cc[0] + Cc[1] * Cc[1];
    const x = (a2 * (B[1] - Cc[1]) + b2 * (Cc[1] - A[1]) + c2 * (A[1] - B[1])) / D;
    const z = (a2 * (Cc[0] - B[0]) + b2 * (A[0] - Cc[0]) + c2 * (B[0] - A[0])) / D;
    return { a, b, c, x, z, r2: (A[0] - x) ** 2 + (A[1] - z) ** 2 };
  };
  let tris = [mk(n, n + 1, n + 2)];
  for (let i = 0; i < n; i++) {
    const px = pts[i][0], pz = pts[i][1], keep = [], edges = [], cnt = new Map();
    for (const t of tris) {
      if ((px - t.x) ** 2 + (pz - t.z) ** 2 < t.r2) edges.push([t.a, t.b], [t.b, t.c], [t.c, t.a]);
      else keep.push(t);
    }
    const key = e => e[0] < e[1] ? e[0] * 1e6 + e[1] : e[1] * 1e6 + e[0];
    for (const e of edges) cnt.set(key(e), (cnt.get(key(e)) || 0) + 1);
    tris = keep;
    for (const e of edges) if (cnt.get(key(e)) === 1) tris.push(mk(e[0], e[1], i));
  }
  const out = [];
  for (const t of tris) {
    if (t.a >= n || t.b >= n || t.c >= n) continue;
    const A = pts[t.a], B = pts[t.b], Cc = pts[t.c];
    const cr = (B[0] - A[0]) * (Cc[1] - A[1]) - (B[1] - A[1]) * (Cc[0] - A[0]);
    out.push(cr > 0 ? [t.a, t.b, t.c] : [t.a, t.c, t.b]);
  }
  return out;
}

function insidePoly(p, poly) {
  let inside = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const a = poly[i], b = poly[j];
    if ((a[1] > p[1]) !== (b[1] > p[1]) &&
        p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
  }
  return inside;
}

function distPoly(p, poly) {
  let best = Infinity;
  for (let i = 0; i < poly.length; i++) {
    const a = poly[i], b = poly[(i + 1) % poly.length];
    const dx = b[0] - a[0], dz = b[1] - a[1], l2 = dx * dx + dz * dz || 1;
    const t = Math.max(0, Math.min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dz) / l2));
    best = Math.min(best, Math.hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dz));
  }
  return best;
}

// Catmull-Rom cerrada sobre puntos de cualquier dimension (aqui [x, z, altura]).
function catmull(pts, per) {
  const n = pts.length, out = [];
  for (let i = 0; i < n; i++) {
    const p0 = pts[(i + n - 1) % n], p1 = pts[i], p2 = pts[(i + 1) % n], p3 = pts[(i + 2) % n];
    for (let k = 0; k < per; k++) {
      const t = k / per, t2 = t * t, t3 = t2 * t;
      out.push(p1.map((_, c) => 0.5 * ((2 * p1[c]) + (-p0[c] + p2[c]) * t +
        (2 * p0[c] - 5 * p1[c] + 4 * p2[c] - p3[c]) * t2 +
        (-p0[c] + 3 * p1[c] - 3 * p2[c] + p3[c]) * t3)));
    }
  }
  return out;
}

/* ------------------------------------------------------------------- el mando */
// Media silueta vista desde arriba (x >= 0), del centro de atras al centro de delante:
// [x, z, altura de la costura entre las dos carcasas]. Medida sobre el escaneo; la mitad
// izquierda es su espejo (se promediaron los dos lados). Se lee el tridente real: el canto
// de atras con los huecos de L y R (x 3.4-6.3, donde la costura baja), las dos palas
// laterales que acaban en z = 4.4 y la central en z = 7.6, con los entrantes entre ellas
// a la altura de z = 0.
const PAD_HALF = [
  [0.00, -7.76, 1.81], [1.95, -7.57, 1.82], [2.70, -7.33, 1.80], [3.08, -7.05, 1.85],
  [3.22, -6.65, 1.85], [3.45, -6.45, 1.45], [4.85, -6.25, 1.30], [5.90, -5.90, 1.30],
  [6.25, -5.72, 1.35], [7.08, -4.90, 2.15], [7.50, -3.90, 2.27], [7.62, -2.75, 2.39],
  [7.93, -0.95, 2.36], [8.02,  0.70, 2.07], [7.83,  2.55, 1.53], [7.60,  3.70, 1.12],
  [7.45,  4.05, 1.00], [6.95,  4.43, 0.80], [6.40,  4.43, 0.80], [5.92,  4.05, 0.95],
  [5.68,  3.50, 1.18], [5.30,  2.35, 1.60], [4.72,  0.25, 2.20], [4.40, -0.06, 2.30],
  [3.80,  0.00, 2.35], [3.00,  0.10, 2.52], [2.50,  0.55, 2.68],
  [1.72,  4.55, 2.55], [1.17,  6.55, 2.08], [0.83,  7.20, 1.87], [0.60,  7.48, 1.79],
  [0.25,  7.63, 1.74], [0.00,  7.64, 1.73],
];

// Alturas (cm x 100) de la cara de arriba y de la de abajo, en una rejilla de 0,5 cm:
// fila = x de 0 a 8, columna = z de -8 a 8. Medidas sobre el escaneo con los agujeros de
// los botones rellenos y suavizadas; la cara de abajo ignora la bahia del Controller Pak,
// que se modela aparte. Fuera del contorno los valores no se usan.
const PAD_TOP = [
  [277,287,300,312,324,333,339,349,358,358,365,369,383,388,390,390,387,384,381,380,378,380,369,363,355,347,333,331,312,289,265,261,261],
  [280,288,298,312,323,333,338,349,357,364,369,374,381,386,390,390,388,385,382,379,377,369,359,355,348,346,333,328,306,289,263,262,262],
  [284,289,295,311,322,331,337,348,354,364,369,374,377,382,387,390,390,388,385,379,371,369,359,355,348,341,331,317,304,282,276,264,272],
  [288,289,295,308,321,329,336,346,350,360,365,370,376,378,382,387,390,388,385,379,371,366,356,349,346,339,324,307,299,288,277,277,280],
  [289,290,293,304,313,324,335,344,348,358,365,369,370,376,381,381,387,387,383,371,366,356,349,340,333,324,316,307,299,292,288,287,287],
  [290,290,290,295,307,319,333,342,344,353,362,367,369,370,376,376,377,346,345,345,341,337,333,328,323,316,310,307,302,299,295,293,292],
  [291,290,290,289,297,313,321,334,344,351,356,363,367,369,372,372,343,326,326,326,323,320,317,315,312,310,307,304,302,302,300,298,295],
  [293,290,287,282,287,297,316,326,336,344,352,356,363,366,367,365,326,313,309,307,305,302,300,299,299,300,301,302,302,302,302,300,298],
  [295,290,282,273,273,293,309,322,327,339,344,351,356,362,365,354,313,295,291,288,285,282,281,281,283,287,291,296,299,302,302,301,300],
  [296,291,283,273,273,287,299,317,326,335,336,343,350,353,354,351,295,277,273,268,263,259,259,259,263,270,279,288,296,299,302,301,301],
  [297,292,285,274,274,283,297,305,325,327,335,335,342,347,347,341,279,273,261,250,244,237,230,230,236,248,265,279,288,296,300,301,301],
  [298,294,287,278,274,276,293,299,307,325,326,327,329,335,335,327,279,260,245,244,240,230,216,201,201,225,248,265,281,292,298,301,301],
  [299,297,291,282,276,276,283,297,304,311,320,322,324,327,324,310,269,251,245,244,237,217,202,195,180,201,225,254,274,288,297,301,301],
  [301,299,294,287,281,278,281,288,297,303,307,312,318,319,310,282,263,250,241,240,237,219,202,177,168,168,213,246,269,285,295,301,302],
  [302,301,297,292,286,281,282,284,289,294,301,305,308,308,288,280,263,253,251,239,230,219,199,175,168,168,213,243,267,284,295,301,303],
  [303,302,299,295,290,286,284,286,287,290,292,293,289,288,281,275,262,253,241,237,227,208,187,176,175,185,215,243,266,283,294,301,303],
  [304,303,300,296,292,288,286,286,287,290,290,289,288,283,280,275,267,256,246,234,219,202,187,178,178,194,219,244,266,283,294,301,304],
];
const PAD_BOT = [
  [105,100,100,100,100,100,100,100,100,102,147,147,138,107,74,40,40,85,91,91,71,54,34,32,32,35,35,38,46,56,75,113,113],
  [105,100,100,100,100,100,100,100,100,103,149,150,141,108,74,46,46,86,93,98,87,71,54,34,35,38,45,46,56,75,113,113,113],
  [105,100,100,100,100,100,100,100,100,103,150,151,151,120,108,74,86,92,104,108,98,87,81,75,75,79,89,103,122,128,124,119,114],
  [105,100,100,100,100,100,100,100,100,104,151,164,165,151,120,108,104,110,125,138,141,141,144,150,158,158,155,146,137,133,128,119,113],
  [104,101,100,100,100,100,100,100,100,104,154,167,168,171,163,160,160,173,192,192,192,182,172,163,163,158,155,146,137,128,119,114,110],
  [104,101,100,100,100,100,100,100,100,105,160,168,172,174,180,190,199,200,200,192,192,182,172,163,154,147,139,132,125,119,114,109,107],
  [105,104,101,100,100,100,100,100,100,107,160,168,174,180,188,197,199,200,186,175,165,156,148,141,135,129,124,119,115,111,109,107,106],
  [108,110,110,101,100,100,100,100,107,135,161,167,173,180,186,193,197,186,162,149,139,132,125,120,117,113,111,108,106,105,105,106,106],
  [111,114,126,113,100,96,100,100,130,146,159,161,167,174,180,186,179,161,135,122,113,108,104,101,100,99,99,99,99,99,102,104,106],
  [116,126,145,145,113,87,96,125,142,151,154,158,156,153,149,149,149,120,101,92,87,84,83,83,84,87,89,91,93,96,99,102,107],
  [116,128,147,150,142,98,98,125,143,147,151,140,116,81,52,37,37,38,49,56,59,60,61,65,70,75,80,85,89,93,96,101,107],
  [116,128,147,161,161,98,98,122,142,142,140,116,81,48,23,-8,-40,-40,-40,-28,-7,18,35,45,57,68,75,80,85,89,94,100,105],
  [115,125,141,161,161,107,107,122,139,139,136,103,53,23,-8,-40,-50,-65,-71,-57,-29,-24,-19,14,45,57,68,76,82,87,92,98,103],
  [112,119,132,143,147,143,122,127,138,139,136,103,53,40,-3,-41,-52,-65,-71,-57,-48,-37,-24,-2,31,55,65,74,80,86,91,96,100],
  [109,114,124,133,143,137,136,136,140,146,146,135,103,53,14,-21,-41,-52,-56,-48,-29,-23,-19,14,47,55,65,73,79,85,90,95,99],
  [106,111,119,127,134,136,137,137,146,148,154,148,135,106,76,51,34,23,17,17,23,32,40,47,54,59,66,73,79,84,89,94,98],
  [104,109,116,123,129,134,137,141,147,154,155,154,146,135,114,110,107,93,77,69,62,56,54,54,56,61,67,73,79,84,89,93,98],
];

// Altura de una tabla en (x, z), bicubica (Catmull-Rom) y con espejo en x = 0.
function tabAt(T, x, z) {
  const u = Math.abs(x) / 0.5, v = (z + 8) / 0.5, nu = T.length, nv = T[0].length;
  const iu = Math.floor(u), iv = Math.floor(v), fu = u - iu, fv = v - iv;
  const g = (i, j) => T[Math.min(Math.abs(i), nu - 1)][Math.max(0, Math.min(j, nv - 1))] / 100;
  const cr = (p0, p1, p2, p3, t) => 0.5 * ((2 * p1) + (-p0 + p2) * t +
    (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
  const row = k => cr(g(iu - 1, k), g(iu, k), g(iu + 1, k), g(iu + 2, k), fu);
  return cr(row(iv - 1), row(iv), row(iv + 1), row(iv + 2), fv);
}
const padTop = (x, z) => tabAt(PAD_TOP, x, z);
const padBot = (x, z) => tabAt(PAD_BOT, x, z);

// Las bisectrices de polyInset cambian de golpe en los entrantes cerrados (la esquina
// entre la pala central y las laterales): ahi el canto se doblaba sobre si mismo y salian
// picos. Promediarlas con sus vecinas reparte el giro en varios puntos.
function smoothDirs(ins) {
  const n = ins.dir.length, dir = [], mit = [];
  for (let i = 0; i < n; i++) {
    let x = 0, z = 0, m = 0;
    for (let k = -2; k <= 2; k++) {
      const j = (i + k + n) % n, w = 3 - Math.abs(k);
      x += ins.dir[j][0] * w; z += ins.dir[j][1] * w; m += ins.mit[j] * w;
    }
    const l = Math.hypot(x, z) || 1;
    dir.push([x / l, z / l]); mit.push(m / 9);
  }
  return { dir, mit };
}

// Contorno completo [x, z, costura], suavizado y en sentido horario en el plano (x, z)
// (el que deja las paredes mirando hacia fuera al coser anillos de abajo arriba).
function padOutline3() {
  const back = PAD_HALF.slice(1, -1).reverse().map(p => [-p[0], p[1], p[2]]);
  const o = catmull(PAD_HALF.concat(back), 5);
  return chartArea(o) > 0 ? o.reverse() : o;
}
function padOutline() { return padOutline3().map(p => [p[0], p[1]]); }

// La carcasa entera, en una pieza: el canto redondeado que sale de la costura hacia la
// cara de arriba y hacia la de abajo, y las dos caras como superficies de altura sobre
// una triangulacion del contorno. Los mangos, el lomo y los valles salen de las tablas.
function padShell() {
  const O = padOutline3(), n = O.length, R = 0.30;
  const ins = smoothDirs(polyInset(O.map(p => [p[0], p[1]])));
  const verts = [], tris = [];
  const inner = [], eT = [], eB = [], off = [];
  for (let i = 0; i < n; i++) {
    const p = O[i], d = ins.dir[i], m = Math.min(ins.mit[i], 1.6);
    const q = [p[0] + d[0] * R * m, p[1] + d[1] * R * m];
    inner.push(q); off.push(R * m);
    eT.push(Math.max(padTop(q[0], q[1]), p[2] + 0.12));
    eB.push(Math.min(padBot(q[0], q[1]), p[2] - 0.12));
  }
  // Siete anillos: tres de canto por abajo, la costura, tres de canto por arriba.
  const A = [-3, -2, -1, 0, 1, 2, 3].map(k => k * Math.PI / 6);
  for (const a of A) {
    const s = Math.sin(Math.abs(a)), c = Math.cos(a);
    for (let i = 0; i < n; i++) {
      const p = O[i], d = ins.dir[i], k = off[i] * (1 - c);
      verts.push(p[0] + d[0] * k, p[2] + ((a > 0 ? eT[i] : eB[i]) - p[2]) * s, p[1] + d[1] * k);
    }
  }
  for (let j = 0; j < A.length - 1; j++) for (let i = 0; i < n; i++) {
    const i2 = (i + 1) % n, q = j * n, w = (j + 1) * n;
    tris.push(q + i, q + i2, w + i, q + i2, w + i2, w + i);
  }
  // Tapas: el contorno ya encogido mas una rejilla por dentro, triangulados una vez y
  // levantados dos veces (arriba y abajo). La rejilla va desplazada un pelo para que
  // Delaunay no tenga cuatro puntos en la misma circunferencia.
  const pts = inner.slice();
  for (let x = -8.1; x <= 8.1; x += 0.3) for (let z = -7.9; z <= 7.9; z += 0.3) {
    const p = [x + 0.011 * Math.sin(z * 7.3), z + 0.011 * Math.cos(x * 5.1)];
    if (insidePoly(p, inner) && distPoly(p, inner) > 0.13) pts.push(p);
  }
  // Delaunay triangula el casco convexo: se quedan los triangulos con el centro dentro.
  const T = delaunay(pts).filter(t => {
    const c = [0, 1].map(k => (pts[t[0]][k] + pts[t[1]][k] + pts[t[2]][k]) / 3);
    return insidePoly(c, inner);
  });
  const topRing = (A.length - 1) * n, idxT = [], idxB = [];
  for (let i = 0; i < pts.length; i++) {
    if (i < n) { idxT.push(topRing + i); idxB.push(i); continue; }
    const [x, z] = pts[i];
    idxT.push(verts.length / 3); verts.push(x, padTop(x, z), z);
    idxB.push(verts.length / 3); verts.push(x, padBot(x, z), z);
  }
  // Antihorario en (x, z) mira hacia -Y: la cara de abajo tal cual, la de arriba al reves.
  for (const t of T) {
    tris.push(idxB[t[0]], idxB[t[1]], idxB[t[2]]);
    tris.push(idxT[t[0]], idxT[t[2]], idxT[t[1]]);
  }
  return solid(verts, tris);
}

// Seccion de rectangulo redondeado en el plano XY a la altura z (para la bahia).
function roundRectRing(hw, yb, yt, r, z, per) {
  const out = [];
  const cs = [[hw - r, yt - r], [-hw + r, yt - r], [-hw + r, yb + r], [hw - r, yb + r]];
  cs.forEach(([cx, cy], q) => {
    for (let k = 0; k < per; k++) {
      const a = (q + k / (per - 1)) * Math.PI / 2;
      out.push([cx + Math.cos(a) * r, cy + Math.sin(a) * r, z]);
    }
  });
  return out;
}

// Bahia del Controller Pak: el cajon que cuelga bajo el lomo de atras. Secciones medidas
// [z, medio ancho, fondo]: la pared trasera baja en rampa desde el canto, el suelo se
// inclina hacia delante (de -2.8 a -2.0) y el frente se recoge contra el mango central.
const BAY = [
  [-7.40, 1.00, -0.05], [-7.20, 2.10, -0.60], [-6.95, 2.80, -1.30], [-6.65, 3.05, -2.05],
  [-6.35, 3.20, -2.65], [-6.10, 3.35, -2.82], [-5.00, 3.45, -2.58], [-4.40, 3.22, -2.42],
  [-3.80, 2.85, -2.24], [-3.50, 2.20, -2.02], [-3.35, 1.40, -1.30],
];

// Posiciones de los botones medidas en los agujeros de la carcasa: [x, z, radio del
// agujero]. El boton es un poco menor que su agujero y el hueco se ve oscuro alrededor.
const PAD_HOLES = {
  A:  [4.09, -1.49, 0.58], B:  [2.99, -2.58, 0.57], START: [0.04, -2.47, 0.51],
  CU: [5.27, -4.42, 0.43], CD: [5.28, -2.63, 0.43], CL: [4.33, -3.53, 0.43], CR: [6.23, -3.51, 0.43],
};
const PAD_DPAD = [-4.82, -2.94, 2.55];          // centro y ancho total de la cruz
const PAD_STICK = [0.07, 0.58, 1.59];           // centro y radio de la boca del stick
const PAD_Z = [0.01, -1.17];                    // hueco del gatillo Z, en la cara de abajo
const PAD_Y0 = 1.0;                             // se baja todo para centrarlo en Y

// Los ids son los mismos del esquema de opciones (options.py): A, B, START, Z, L, R,
// CU/CD/CL/CR, DU/DD/DL/DR y STICK. Si aqui falta uno, en la interfaz no se puede pinchar.
function buildController() {
  const b = new Builder();
  b.push(M4.trans(0, -PAD_Y0, 0));

  // Hueco oscuro de un control: un disco que asoma justo por encima del punto mas alto de
  // la carcasa en su borde, para que en las pendientes se lea como un circulo entero y no
  // como una media luna enterrada. Devuelve esa altura, que es sobre la que va el control.
  const rimTop = (x, z, r) => {
    let hi = -1e9, lo = 1e9;
    for (let k = 0; k < 16; k++) {
      const a = k / 16 * Math.PI * 2, y = padTop(x + Math.cos(a) * r, z + Math.sin(a) * r);
      hi = Math.max(hi, y); lo = Math.min(lo, y);
    }
    return [hi, lo];
  };
  const well = (x, z, r) => {
    const [hi, lo] = rimTop(x, z, r), h = hi - lo + 0.25;
    b.part(null, C.well);
    b.push(M4.trans(x, hi + 0.02 - h / 2, z)).add(cylinder(r, h, 32)).pop();
    return hi;
  };

  b.part(null, C.body);
  b.add(padShell());
  b.add(tube(BAY.map(([z, hw, yb]) =>
    roundRectRing(hw, yb, 1.2, Math.min(0.55, hw * 0.45, (1.2 - yb) * 0.45), z, 8))));

  // Boca del conector del Pak en el suelo de la bahia, siguiendo su inclinacion.
  b.part(null, C.well);
  b.push(M4.mul(M4.trans(0, -2.72, -5.53), M4.rotX(-0.25)))
   .add(roundedBox(3.10, 0.10, 0.80, 0.04, 1)).pop();

  // Cable: sale del centro del canto de atras, con su funda (mas gruesa contra el mando).
  b.part(null, [0.17, 0.175, 0.195]);
  b.push(M4.mul(M4.trans(0, 2.15, -8.05), M4.rotX(Math.PI / 2))).add(cylinder(0.27, 0.8, 18, 0.42)).pop();
  b.push(M4.mul(M4.trans(0, 2.15, -9.10), M4.rotX(Math.PI / 2))).add(cylinder(0.22, 1.5, 14)).pop();

  // --- cruceta ---------------------------------------------------------------
  const [dx, dz, dw] = PAD_DPAD, dy = padTop(dx, dz);
  b.part(null, C.well);
  for (const [ax, az] of [[1, 0], [-1, 0], [0, 1], [0, -1]])
    b.push(M4.trans(dx, dy - 0.10, dz)).add(extrude(dpadArm(dw / 2 + 0.05, 0.98, ax, az), 0.30)).pop();
  b.part(null, C.dpad);
  b.push(M4.trans(dx, dy + 0.20, dz)).add(roundedBox(0.86, 0.34, 0.86, 0.10, 2)).pop();
  const arms = [["DU", 0, -1], ["DD", 0, 1], ["DL", -1, 0], ["DR", 1, 0]];
  for (const arm of arms) {
    b.part(arm[0], C.dpad);
    b.push(M4.trans(dx, dy + 0.22, dz)).add(extrude(dpadArm(dw / 2 - 0.05, 0.84, arm[1], arm[2]), 0.34)).pop();
  }

  // --- stick: boca oscura, vastago y seta con el hueco del pulgar -------------
  const [sx, sz, sr] = PAD_STICK, sy = well(sx, sz, sr) - 0.08;
  b.part("STICK", C.stick);
  b.push(M4.trans(sx, sy + 0.35, sz)).add(cylinder(0.44, 0.90, 20, 0.40)).pop();
  b.push(M4.trans(sx, sy + 0.90, sz)).add(cylinder(1.02, 0.32, 32, 0.95)).pop();
  b.part("STICK", [0.235, 0.242, 0.262]);
  b.push(M4.trans(sx, sy + 1.07, sz)).add(cylinder(0.70, 0.04, 28)).pop();

  // --- A, B, C y Start --------------------------------------------------------
  const btn = (id, col, h) => {
    const [x, z, r] = PAD_HOLES[id], y = well(x, z, r) - 0.08;
    b.part(id, col);
    b.push(M4.trans(x, y + h / 2 - 0.08, z)).add(cylinder(r - 0.07, h, 26, (r - 0.07) * 0.93)).pop();
  };
  btn("A", C.a, 0.44);
  btn("B", C.b, 0.44);
  for (const id of ["CU", "CD", "CL", "CR"]) btn(id, C.c, 0.38);
  btn("START", C.start, 0.34);

  // --- gatillos -------------------------------------------------------------
  // L y R ocupan los huecos del canto de atras (donde la costura se hunde): una pestana
  // que sigue el contorno, sale un poco hacia fuera y asoma por encima del lomo.
  const O = padOutline3(), ins = polyInset(O.map(p => [p[0], p[1]]));
  for (const [id, s] of [["L", -1], ["R", 1]]) {
    const sel = [];
    for (let i = 0; i < O.length; i++) {
      const x = O[i][0] * s;
      if (x > 3.40 && x < 6.35 && O[i][1] < -5.0) sel.push(i);
    }
    sel.sort((i, j) => O[i][0] * s - O[j][0] * s);
    const outer = sel.map(i => [O[i][0] - ins.dir[i][0] * 0.40, O[i][1] - ins.dir[i][1] * 0.40]);
    const innerP = sel.map(i => [O[i][0] + ins.dir[i][0] * 0.35, O[i][1] + ins.dir[i][1] * 0.35]);
    b.part(id, C.body);
    b.push(M4.trans(0, 2.40, 0)).add(roundPrism(outer.concat(innerP.reverse()), 1.30, 0.22, 3)).pop();
  }

  // Z: en la cara de abajo, en su hueco detras del stick, donde llega el indice.
  const [zx, zz] = PAD_Z;
  b.part("Z", C.dpad);
  b.push(M4.mul(M4.trans(zx, padBot(zx, zz) - 0.22, zz), M4.rotX(0.30)))
   .add(roundedBox(1.10, 0.74, 1.40, 0.30, 4)).pop();

  b.pop();
  return b.build();
}

/* ---------------------------------------------------------------- el cartucho */
// Medidas de las piezas, en centimetros y tomadas del objeto real. Salen del modulo porque
// quien cuelga una imagen necesita saber a que proporcion recortarla.
const BOX = { w: 19.0, h: 13.3, d: 2.8 };      // caja de carton NTSC/PAL
const CART = { w: 11.6, h: 7.64, d: 1.84 };    // cartucho: apaisado, mas ancho que alto

// Media silueta frontal del cartucho [x, altura desde la base]: costados rectos y el techo
// en arco, medido cada milimetro de altura sobre el escaneo.
const CART_HALF = [
  [0.00, 0.00], [5.62, 0.00], [5.78, 0.08], [5.80, 0.30], [5.80, 6.40], [5.79, 6.50],
  [5.63, 6.60], [5.36, 6.70], [5.09, 6.80], [4.79, 6.90], [4.48, 7.00], [4.13, 7.10],
  [3.76, 7.20], [3.33, 7.30], [2.86, 7.40], [2.27, 7.50], [1.45, 7.60], [0.00, 7.64],
];
// Rebaje de la pegatina en la cara de delante: 5,5 x 6,4 cm, centrado en x, de 0,55 a
// 6,95 cm desde la base. Mas alta que ancha -- nada que ver con la caratula apaisada de la
// caja, que es justo por lo que son dos imagenes distintas.
const LABEL = { w: 5.5, h: 6.4, y0: 0.55 };

function buildCart(opts) {
  opts = opts || {};
  const b = new Builder();
  const h = CART.h, d = CART.d;
  const half = CART_HALF.map(p => [p[0], p[1] - h / 2]);
  const poly = half.concat(half.slice(1, -1).reverse().map(p => [-p[0], p[1]]));

  // La silueta se extruye a lo hondo: roundPrism extruye en Y, y el giro lleva ese eje a Z
  // dejando la altura de la silueta en Y.
  b.part(null, opts.shell || C.cart);
  b.push(M4.rotX(-Math.PI / 2)).add(roundPrism(poly, d, 0.40, 4)).pop();

  // Boca del conector en la base.
  b.part(null, [0.10, 0.10, 0.12]);
  b.push(M4.trans(0, -h / 2 + 0.01, 0)).add(roundedBox(9.2, 0.04, 1.0, 0.02, 1)).pop();

  // Pegatina, un pelo por delante de la cara.
  const ly = LABEL.y0 + LABEL.h / 2 - h / 2;
  b.part("LABEL", C.cartLbl, opts.labelTex || "label");
  b.push(M4.trans(0, ly, d / 2 + 0.012)).add(plane(LABEL.w, LABEL.h)).pop();
  return b.build();
}

/* -------------------------------------------------------------------- la caja */
// Caja de carton del juego. La de Norteamerica y Europa es APAISADA: 190 x 133 x 28 mm,
// mas ancha que alta, al reves que la de SNES o la de Game Boy. Los escaneos de caratula
// que se descargan tienen esa misma forma (1,37-1,43 de ancho por alto segun quien midiera
// los margenes), asi que la caja lleva las medidas reales y la caratula entra sin deformar.
// La portada va de textura en la cara frontal y el lomo lleva la franja de color.
function buildBox(opts) {
  opts = opts || {};
  const w = opts.w || BOX.w, h = opts.h || BOX.h, d = opts.d || BOX.d;
  const b = new Builder();

  b.part(null, opts.color || C.box);
  b.add(roundedBox(w, h, d, 0.14, 2));
  b.part(null, opts.edge || C.boxEdge);              // franja del canto superior
  b.push(M4.trans(0, h / 2 - 0.55, 0)).add(roundedBox(w + 0.02, 1.0, d + 0.02, 0.06, 2)).pop();

  // Sin caratula el plano se queda con este color: blanco puro se leeria como un fallo de
  // carga, y este gris azulado se ve como "esta caja no tiene portada".
  b.part("COVER", opts.coverPlain || [0.22, 0.24, 0.30], opts.coverTex || "cover");
  b.push(M4.trans(0, 0, d / 2 + 0.02)).add(plane(w - 0.3, h - 0.3)).pop();
  b.part("BACK", [0.62, 0.62, 0.64], opts.backTex || null);
  b.push(M4.mul(M4.trans(0, 0, -d / 2 - 0.02), M4.rotY(Math.PI))).add(plane(w - 0.3, h - 0.3)).pop();
  return b.build();
}

// COVER y LABEL son las caras donde se pega la imagen; su proporcion es la que hay que
// pedirle al recorte para que nada se estire.
const COVER = { w: BOX.w - 0.3, h: BOX.h - 0.3 };

return { buildController, buildCart, buildBox, plane, padOutline,
         COLORS: C, BOX, CART, COVER, LABEL };
})();

if (typeof module !== "undefined") module.exports = MODELS;
