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
const { M4, Builder, roundedBox, cylinder, sphere, extrude, roundPrism, loft, smoothPoly } = G;

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
// Silueta real vista desde arriba: un tridente de tres palas con dos entrantes en el borde
// de delante, de donde salen los tres mangos. Media silueta a mano (de la izquierda del
// centro hacia la derecha y por detras hasta el centro), la otra mitad es su espejo, y una
// Catmull-Rom la suaviza: asi el cuerpo es UNA pieza con su contorno, no cajas solapadas.
//
// Ejes: X a la derecha, Z hacia el jugador. Ancho total 17.4 cm, como el mando de verdad.
const PAD_HALF = [
  [0.00,  3.20], [1.85, 3.05], [2.95, 2.35], [3.55, 1.30],   // pala central y entrante
  [4.25,  1.85], [5.20, 2.20], [6.30, 2.35], [7.50, 2.00],   // frente de la pala derecha
  [8.35,  1.00], [8.68, -0.35], [8.50, -1.85], [7.70, -2.85],// punta y canto de atras
  [6.30, -3.30], [4.60, -3.50], [3.00, -3.85], [1.50, -4.05], [0.00, -4.10],
];

function padOutline() {
  const back = PAD_HALF.slice(1, -1).reverse().map(p => [-p[0], p[1]]);
  return smoothPoly(PAD_HALF.concat(back), 4);
}

// Un mango: secciones de rectangulo redondeado (superelipse, que es la seccion real: el
// mango de N64 no es un tubo) que bajan, se ensanchan un poco al salir del cuerpo, se
// afilan despues y cierran en punta roma.
function handleRings(len, rx, rz, bend, steps, seg) {
  const rings = [], e = 0.66;
  for (let k = 0; k <= steps; k++) {
    const t = k / steps;
    const w = 1 + 0.08 * Math.sin(Math.PI * Math.min(1, t * 1.7)) - 0.20 * t * t;
    const s = w * Math.sqrt(Math.max(0, 1 - Math.pow(t, 4)));
    const y = -len * t, z = bend * t * t;
    const ring = [];
    for (let i = 0; i < seg; i++) {
      const a = i / seg * Math.PI * 2, ca = Math.cos(a), sa = Math.sin(a);
      ring.push([Math.sign(ca) * Math.pow(Math.abs(ca), e) * rx * s, y,
                 z + Math.sign(sa) * Math.pow(Math.abs(sa), e) * rz * s]);
    }
    rings.push(ring);
  }
  return rings;
}

// Los ids son los mismos del esquema de opciones (options.py): A, B, START, Z, L, R,
// CU/CD/CL/CR, DU/DD/DL/DR y STICK. Si aqui falta uno, en la interfaz no se puede pinchar.
function buildController() {
  const b = new Builder();
  // El cuerpo real es DELGADO (unos 2.4 cm) y de canto vivo; lo que abulta son las tres
  // palas. Con 2.8 de grosor y 0.62 de radio la extrusion se comia la cara de arriba y el
  // mando salia con forma de almohada, que es justo lo que no es.
  const TH = 2.40, TOP = TH / 2;              // grosor del cuerpo y cara de arriba (y = 1.2)

  // --- carcasa: una sola extrusion redondeada del contorno ---------------------
  b.part(null, C.body);
  b.add(roundPrism(padOutline(), TH, 0.42, 4));

  // Las tres palas no son planas: cada una es un domo bajo sobre la cara de arriba, y entre
  // ellas quedan los dos valles. Una esfera aplastada hundida hasta que solo asoma el
  // casquete da exactamente ese perfil, y `M4.normal` corrige las normales del escalado no
  // uniforme, asi que la luz sigue siendo correcta.
  const dome = (x, z, rx, ry, rz, rise) => {
    b.push(M4.mul(M4.trans(x, TOP + rise - ry, z), M4.scale(rx, ry, rz)));
    b.add(sphere(1, 28));
    b.pop();
  };
  b.part(null, C.body);
  dome(-5.35, -0.70, 2.85, 1.30, 2.75, 0.62);   // pala izquierda (cruceta)
  dome( 0.00,  0.10, 2.55, 1.45, 2.95, 0.70);   // pala central (stick)
  dome( 5.85, -0.55, 2.60, 1.30, 2.80, 0.62);   // pala derecha (botones)

  // --- los tres mangos ---------------------------------------------------------
  // Mas largos y mas abiertos: en el mando de verdad el mango mide casi lo mismo que el
  // alto del cuerpo y los laterales salen claramente hacia afuera. Con 7.1 cm y 0.33 rad
  // se quedaban en munones y el conjunto parecia una pastilla con tres bultos.
  b.part(null, C.body);
  b.push(M4.trans(0, 0.35, 1.55));
  b.add(loft(handleRings(8.7, 1.44, 1.80, 1.05, 20, 24), { capStart: true }));
  b.pop();
  for (const s of [-1, 1]) {
    b.push(M4.mul(M4.mul(M4.trans(s * 5.95, 0.05, 0.55), M4.rotZ(s * 0.27)), M4.rotX(0.16)));
    b.add(loft(handleRings(8.4, 1.40, 1.74, 0.95, 20, 24), { capStart: true }));
    b.pop();
  }

  // Los controles no van sobre la cara plana del cuerpo sino sobre la CIMA de su pala, que
  // ahora es un domo. Estas tres alturas son esa cima; sin ellas los botones quedaban
  // medio hundidos dentro de la cupula.
  const YL = TOP + 0.62, YC = TOP + 0.70, YR = TOP + 0.62;

  // --- cruceta (pala izquierda) -----------------------------------------------
  const dcx = -5.35, dcz = -0.70;
  b.part(null, C.bodyDark);                   // rebaje donde se apoya la cruceta
  b.push(M4.trans(dcx, YL - 0.12, dcz)).add(cylinder(1.72, 0.30, 28)).pop();
  b.part(null, C.dpad);
  b.push(M4.trans(dcx, YL + 0.06, dcz)).add(roundedBox(1.5, 0.34, 1.5, 0.12, 2)).pop();
  const arms = [["DU", 0, -1], ["DD", 0, 1], ["DL", -1, 0], ["DR", 1, 0]];
  for (const arm of arms) {
    b.part(arm[0], C.dpad);
    b.push(M4.trans(dcx, YL + 0.16, dcz)).add(extrude(dpadArm(1.05, 0.66, arm[1], arm[2]), 0.34)).pop();
  }

  // --- stick (pala central) ----------------------------------------------------
  const scz = 0.35;
  b.part(null, C.bodyDark);
  b.push(M4.trans(0, YC - 0.06, scz)).add(cylinder(1.30, 0.34, 28)).pop();
  b.part("STICK", C.stick);
  b.push(M4.trans(0, YC + 0.42, scz)).add(cylinder(0.50, 0.92, 24, 0.44)).pop();
  b.push(M4.trans(0, YC + 0.98, scz)).add(cylinder(0.78, 0.28, 28, 0.72)).pop();
  b.part("STICK", [0.235, 0.242, 0.262]);            // el hueco del pulgar, hundido
  b.push(M4.trans(0, YC + 1.12, scz)).add(cylinder(0.58, 0.06, 24)).pop();

  // --- A, B, C y Start ---------------------------------------------------------
  // Colocacion del mando real: B arriba a la izquierda del grupo, A abajo y a su derecha,
  // y el rombo de las C a la derecha de los dos. Antes A quedaba delante de B y el grupo
  // entero leia al reves.
  const btn = (id, col, x, z, r, h, y) => {
    b.part(id, col);
    b.push(M4.trans(x, y + h / 2 - 0.06, z)).add(cylinder(r, h, 26, r * 0.94)).pop();
  };
  btn("B", C.b, 4.60, -0.55, 0.60, 0.33, YR);
  btn("A", C.a, 5.60,  0.70, 0.60, 0.33, YR);
  // Las cuatro C van en rombo sobre un rebaje redondo, no sobre una plancha cuadrada.
  const ccx = 7.05, ccz = -0.85, cd = 0.72;
  b.part(null, C.bodyDark);
  b.push(M4.trans(ccx, YR - 0.10, ccz)).add(cylinder(1.34, 0.26, 26)).pop();
  btn("CU", C.c, ccx, ccz - cd, 0.34, 0.28, YR);
  btn("CD", C.c, ccx, ccz + cd, 0.34, 0.28, YR);
  btn("CL", C.c, ccx - cd, ccz, 0.34, 0.28, YR);
  btn("CR", C.c, ccx + cd, ccz, 0.34, 0.28, YR);
  // Start: en el valle entre la pala central y las de los lados, hacia el fondo, sobre su
  // propio rebaje oscuro -- en el mando real es una isla, no un boton suelto en la carcasa.
  b.part(null, C.bodyDark);
  b.push(M4.trans(0, TOP + 0.02, -2.55)).add(cylinder(0.92, 0.24, 26)).pop();
  b.part("START", C.start);
  b.push(M4.trans(0, TOP + 0.20, -2.55)).add(cylinder(0.56, 0.30, 24, 0.50)).pop();

  // --- gatillos: L y R en el canto de atras, Z bajo el mango central -----------
  // En el mando real L y R son lengüetas del propio canto trasero, no dos tacos encima:
  // desde arriba apenas se ven. Mas bajas, mas planas y volcadas sobre el borde.
  for (const t of [["L", -1], ["R", 1]]) {
    b.part(t[0], C.body);
    b.push(M4.mul(M4.trans(t[1] * 6.55, TOP - 0.30, -2.75), M4.rotX(-0.62)))
     .add(roundedBox(2.5, 0.62, 1.15, 0.28, 4)).pop();
  }
  // Bahia del Controller Pak: va en el CANTO DE ATRAS del cuerpo, centrada, como un cajon
  // que sobresale un poco del contorno -- no en la pala central, que es donde estaba. El
  // conector de 32 patillas mira hacia atras y el cable sale justo por encima.
  b.part(null, C.body);
  b.push(M4.trans(0, -0.10, -4.35)).add(roundedBox(3.60, 1.95, 1.30, 0.22, 3)).pop();
  b.part(null, [0.16, 0.165, 0.185]);
  b.push(M4.trans(0, -0.10, -4.72)).add(roundedBox(2.95, 1.35, 0.70, 0.10, 2)).pop();
  b.part(null, [0.72, 0.70, 0.66]);                       // peine del conector, metalico
  b.push(M4.trans(0, -0.10, -4.90)).add(roundedBox(2.35, 0.34, 0.30, 0.05, 1)).pop();

  // Cable: sale por detras, por encima de la bahia. Un tramo corto basta; lo que se lee es
  // de donde sale, y sale del centro del canto trasero, no de un lateral.
  b.part(null, [0.17, 0.175, 0.195]);
  b.push(M4.mul(M4.trans(0, 0.55, -4.60), M4.rotX(Math.PI / 2)))
   .add(cylinder(0.24, 1.60, 16)).pop();

  // Z: en la cara de ABAJO, arriba del todo de la pala central, justo donde arranca del
  // cuerpo -- ahi es donde llega el indice. Estaba media pala mas abajo.
  b.part("Z", C.bodyDark);
  b.push(M4.mul(M4.trans(0, -1.28, 1.40), M4.rotX(0.30)))
   .add(roundedBox(1.45, 0.55, 1.55, 0.26, 4)).pop();

  return b.build();
}

/* ---------------------------------------------------------------- el cartucho */
// 8.8 x 11.4 x 2.1 cm: carcasa, resalte superior para agarrar, etiqueta hundida y la
// ranura del conector abajo. La etiqueta admite textura ("label").
// Medidas de las piezas, en centimetros y tomadas del objeto real. Salen del modulo porque
// quien cuelga una imagen necesita saber a que proporcion recortarla.
const BOX = { w: 19.0, h: 13.3, d: 2.8 };      // caja de carton NTSC/PAL
const CART = { w: 8.8, h: 11.4, d: 2.1 };      // cartucho

function buildCart(opts) {
  opts = opts || {};
  const b = new Builder();
  const w = CART.w, h = CART.h, d = CART.d;

  b.part(null, opts.shell || C.cart);
  b.push(M4.trans(0, 0, 0)).add(roundedBox(w, h, d, 0.28, 3)).pop();
  b.push(M4.trans(0, h / 2 - 0.35, 0)).add(roundedBox(w + 0.5, 1.5, d + 0.35, 0.35, 3)).pop();
  // Hueco del conector: una tira mas oscura en la base.
  b.part(null, [0.10, 0.10, 0.12]);
  b.push(M4.trans(0, -h / 2 + 0.28, 0)).add(roundedBox(w - 1.6, 0.55, d - 0.5, 0.1, 2)).pop();

  // Etiqueta: rebaje claro + plano con la textura, un pelo por delante.
  // Rebaje y pegatina. Medido sobre una foto de cartucho real: la pegatina ocupa el 88 %
  // del ancho y el 77 % del alto, centrada (lo que sobra arriba es el reborde de agarre y
  // abajo el faldon liso). Sale casi cuadrada, 0,88 de ancho por alto -- nada que ver con
  // la caratula apaisada de la caja, que es justo por lo que son dos imagenes distintas.
  b.part(null, C.cartLbl);
  b.push(M4.trans(0, 0.1, d / 2 - 0.02)).add(roundedBox(w - 0.9, h - 2.4, 0.12, 0.06, 2)).pop();
  b.part("LABEL", C.cartLbl, opts.labelTex || "label");
  b.push(M4.trans(0, 0.1, d / 2 + 0.06)).add(plane(w - 1.1, h - 2.6)).pop();
  return b.build();
}

/* -------------------------------------------------------------------- la caja */
// La caja de carton: 13.5 x 19.0 x 3.0 cm. La portada va de textura en la cara frontal, y
// el lomo lleva la franja roja de las cajas europeas de N64.
// Caja de carton del juego. La de Norteamerica y Europa es APAISADA: 190 x 133 x 28 mm,
// mas ancha que alta, al reves que la de SNES o la de Game Boy. Los escaneos de caratula
// que se descargan tienen esa misma forma (1,37-1,43 de ancho por alto segun quien midiera
// los margenes), asi que la caja lleva las medidas reales y la caratula entra sin deformar.
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
const LABEL = { w: CART.w - 1.1, h: CART.h - 2.6 };

return { buildController, buildCart, buildBox, plane, padOutline, handleRings,
         COLORS: C, BOX, CART, COVER, LABEL };
})();

if (typeof module !== "undefined") module.exports = MODELS;
