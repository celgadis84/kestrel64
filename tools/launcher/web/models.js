/* models.js -- geometria propia: mando, cartucho y caja de N64.
 *
 * Todo generado en codigo, sin ningun modelo descargado. Los modelos de N64 que circulan
 * por los repositorios 3D o no son descargables o llevan licencias (Editorial, "todos los
 * derechos reservados") que no permiten meterlos dentro de un programa que se reparte, asi
 * que esto es dibujo nuestro: primitivas de `gl.js` colocadas a mano con las proporciones
 * reales del hardware.
 *
 * Medidas en centimetros, tal cual el aparato: el mando mide 17.3 cm de ancho, el cartucho
 * 8.8 x 11.4 x 2.1 y la caja de carton 13.5 x 19.0 x 3.0.
 *
 * Ejes: X derecha, Y arriba, Z hacia el jugador. La cara de arriba del mando es y = +0.8.
 */
"use strict";

const MODELS = (() => {

const G = (typeof GL !== "undefined") ? GL : require("./gl.js");
const { M4, Builder, roundedBox, cylinder, sphere, extrude } = G;

const C = {
  body:    [0.760, 0.752, 0.723],   // el gris hueso del mando original
  bodyDark:[0.560, 0.556, 0.538],
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

/* ------------------------------------------------------------------- el mando */
// Los ids son los mismos del esquema de opciones (options.py): A, B, START, Z, L, R,
// CU/CD/CL/CR, DU/DD/DL/DR y STICK. Si aqui falta uno, en la interfaz no se puede pinchar.
function buildController() {
  const b = new Builder();
  const TOP = 0.8;                            // cara superior del cuerpo

  // --- cuerpo: tres palas unidas, cada una una caja redondeada -----------------
  b.part(null, C.body);
  b.push(M4.trans(0, 0, 0.1)).add(roundedBox(6.4, 1.6, 5.4, 0.55, 5)).pop();
  for (const s of [-1, 1]) {
    b.push(M4.mul(M4.trans(s * 5.2, -0.05, -0.35), M4.rotY(-s * 0.13)))
     .add(roundedBox(6.0, 1.5, 4.6, 0.6, 5)).pop();
  }
  // Los tres mangos. El central lleva el Z debajo; los laterales caen abiertos.
  b.part(null, C.bodyDark);
  b.push(M4.mul(M4.trans(0, -2.9, 1.30), M4.rotX(0.22)))
   .add(roundedBox(2.4, 5.8, 2.6, 1.0, 5)).pop();
  for (const s of [-1, 1]) {
    b.push(M4.mul(M4.mul(M4.trans(s * 6.5, -2.6, 0.75), M4.rotZ(-s * 0.30)), M4.rotX(0.16)))
     .add(roundedBox(2.4, 5.6, 2.6, 1.0, 5)).pop();
  }

  // --- cruceta (pala izquierda) -----------------------------------------------
  const dcx = -5.15, dcz = -0.15;
  b.part(null, C.dpad);
  b.push(M4.trans(dcx, TOP + 0.06, dcz)).add(roundedBox(1.5, 0.34, 1.5, 0.12, 2)).pop();
  const arms = [["DU", 0, -1], ["DD", 0, 1], ["DL", -1, 0], ["DR", 1, 0]];
  for (const [id, ax, az] of arms) {
    b.part(id, C.dpad);
    b.push(M4.trans(dcx, TOP + 0.16, dcz)).add(extrude(dpadArm(1.05, 0.66, ax, az), 0.34)).pop();
  }

  // --- stick (pala central) ----------------------------------------------------
  b.part(null, C.bodyDark);
  b.push(M4.trans(0, TOP - 0.05, 0.55)).add(cylinder(1.30, 0.30, 28)).pop();
  b.part("STICK", C.stick);
  b.push(M4.trans(0, TOP + 0.35, 0.55)).add(cylinder(0.44, 0.80, 24, 0.38)).pop();
  b.push(M4.trans(0, TOP + 0.82, 0.55)).add(cylinder(0.66, 0.26, 28, 0.60)).pop();

  // --- A, B, C y Start ---------------------------------------------------------
  const rcx = 5.15;
  const btn = (id, col, x, z, r, h) => {
    b.part(id, col);
    b.push(M4.trans(x, TOP + h / 2 - 0.02, z)).add(cylinder(r, h, 26, r * 0.94)).pop();
  };
  btn("A", C.a, rcx - 0.15, 0.95, 0.62, 0.34);
  btn("B", C.b, rcx - 1.35, 0.20, 0.52, 0.32);
  b.part(null, C.bodyDark);                          // isla de las C, hundida
  b.push(M4.trans(rcx + 0.55, TOP - 0.06, -1.15)).add(roundedBox(2.1, 0.30, 2.1, 0.15, 3)).pop();
  const cd = 0.66;
  btn("CU", C.c, rcx + 0.55, -1.15 - cd, 0.34, 0.28);
  btn("CD", C.c, rcx + 0.55, -1.15 + cd, 0.34, 0.28);
  btn("CL", C.c, rcx + 0.55 - cd, -1.15, 0.34, 0.28);
  btn("CR", C.c, rcx + 0.55 + cd, -1.15, 0.34, 0.28);
  // Start va DEBAJO del stick, hacia el jugador, como en el mando real.
  b.part("START", C.start);
  b.push(M4.trans(0, TOP + 0.12, 2.05)).add(cylinder(0.46, 0.30, 24)).pop();

  // --- gatillos: L y R arriba en el canto, Z debajo del mango central ----------
  for (const [id, s] of [["L", -1], ["R", 1]]) {
    b.part(id, C.body);
    b.push(M4.mul(M4.trans(s * 5.1, TOP - 0.30, -2.45), M4.rotX(-0.34)))
     .add(roundedBox(2.0, 0.78, 1.30, 0.32, 4)).pop();
  }
  b.part("Z", C.bodyDark);
  b.push(M4.mul(M4.trans(0, -1.62, 1.62), M4.rotX(0.42)))
   .add(roundedBox(1.55, 0.62, 1.45, 0.28, 4)).pop();

  return b.build();
}

/* ---------------------------------------------------------------- el cartucho */
// 8.8 x 11.4 x 2.1 cm: carcasa, resalte superior para agarrar, etiqueta hundida y la
// ranura del conector abajo. La etiqueta admite textura ("label").
function buildCart(opts) {
  opts = opts || {};
  const b = new Builder();
  const w = 8.8, h = 11.4, d = 2.1;

  b.part(null, opts.shell || C.cart);
  b.push(M4.trans(0, 0, 0)).add(roundedBox(w, h, d, 0.28, 3)).pop();
  b.push(M4.trans(0, h / 2 - 0.35, 0)).add(roundedBox(w + 0.5, 1.5, d + 0.35, 0.35, 3)).pop();
  // Hueco del conector: una tira mas oscura en la base.
  b.part(null, [0.10, 0.10, 0.12]);
  b.push(M4.trans(0, -h / 2 + 0.28, 0)).add(roundedBox(w - 1.6, 0.55, d - 0.5, 0.1, 2)).pop();

  // Etiqueta: rebaje claro + plano con la textura, un pelo por delante.
  b.part(null, C.cartLbl);
  b.push(M4.trans(0, -0.35, d / 2 - 0.02)).add(roundedBox(w - 1.0, h - 3.2, 0.12, 0.06, 2)).pop();
  b.part("LABEL", C.cartLbl, opts.labelTex || "label");
  b.push(M4.trans(0, -0.35, d / 2 + 0.06)).add(plane(w - 1.2, h - 3.4)).pop();
  return b.build();
}

/* -------------------------------------------------------------------- la caja */
// La caja de carton: 13.5 x 19.0 x 3.0 cm. La portada va de textura en la cara frontal, y
// el lomo lleva la franja roja de las cajas europeas de N64.
function buildBox(opts) {
  opts = opts || {};
  const w = opts.w || 13.5, h = opts.h || 19.0, d = opts.d || 3.0;
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

return { buildController, buildCart, buildBox, plane, COLORS: C };
})();

if (typeof module !== "undefined") module.exports = MODELS;
