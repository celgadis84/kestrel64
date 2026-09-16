/* gl.js -- motor 3D minimo para el lanzador (WebGL1, sin dependencias).
 *
 * El resto de la interfaz hace su 3D con `transform-style: preserve-3d`, que vale para
 * caras planas (el coverflow) pero no para un objeto con volumen: el mando, el cartucho y
 * la caja son geometria de verdad, con normales y luz. Esto es lo minimo para dibujarlos:
 * matrices, un programa de sombreado, mallas indexadas, una textura opcional por parte y
 * seleccion por color de parte.
 *
 * No hay biblioteca vendorizada ni modelo descargado: la geometria se genera en
 * `models.js`. Los modelos de N64 que circulan por los repositorios 3D vienen con licencias
 * que no permiten repartirlos dentro de un programa (o directamente no son descargables),
 * asi que el mando, el cartucho y la caja son nuestros.
 */
"use strict";

const GL = (() => {

/* ---------------------------------------------------------------- matrices */
const M4 = {
  ident: () => new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
  mul(a, b) {                       // a*b, las dos en column-major
    const o = new Float32Array(16);
    for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      o[c * 4 + r] = s;
    }
    return o;
  },
  trans(x, y, z) { const m = M4.ident(); m[12] = x; m[13] = y; m[14] = z; return m; },
  scale(x, y, z) { const m = M4.ident(); m[0] = x; m[5] = y; m[10] = z; return m; },
  rotX(a) { const m = M4.ident(), c = Math.cos(a), s = Math.sin(a); m[5] = c; m[6] = s; m[9] = -s; m[10] = c; return m; },
  rotY(a) { const m = M4.ident(), c = Math.cos(a), s = Math.sin(a); m[0] = c; m[2] = -s; m[8] = s; m[10] = c; return m; },
  rotZ(a) { const m = M4.ident(), c = Math.cos(a), s = Math.sin(a); m[0] = c; m[1] = s; m[4] = -s; m[5] = c; return m; },
  persp(fovy, asp, zn, zf) {
    const f = 1 / Math.tan(fovy / 2), m = new Float32Array(16);
    m[0] = f / asp; m[5] = f; m[10] = (zf + zn) / (zn - zf); m[11] = -1; m[14] = 2 * zf * zn / (zn - zf);
    return m;
  },
  lookAt(ex, ey, ez, cx, cy, cz) {           // arriba = (0,1,0)
    let zx = ex - cx, zy = ey - cy, zz = ez - cz;
    let l = Math.hypot(zx, zy, zz) || 1; zx /= l; zy /= l; zz /= l;
    let xx = zz, xy = 0, xz = -zx;           // (0,1,0) x z
    l = Math.hypot(xx, xy, xz) || 1; xx /= l; xy /= l; xz /= l;
    const yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    return new Float32Array([
      xx, yx, zx, 0, xy, yy, zy, 0, xz, yz, zz, 0,
      -(xx * ex + xy * ey + xz * ez), -(yx * ex + yy * ey + yz * ez), -(zx * ex + zy * ey + zz * ez), 1]);
  },
  // Inversa-traspuesta de la 3x3 para las normales.
  normal(m) {
    const a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const det = a * (e * i - f * h) - d * (b * i - c * h) + g * (b * f - c * e) || 1;
    return new Float32Array([
      (e * i - f * h) / det, (g * f - d * i) / det, (d * h - e * g) / det,
      (h * c - b * i) / det, (a * i - g * c) / det, (g * b - a * h) / det,
      (b * f - c * e) / det, (d * c - a * f) / det, (a * e - d * b) / det]);
  },
};

/* --------------------------------------------------------- constructor de mallas */
// Acumula triangulos con una pila de transformaciones. Cada tramo lleva el id de su parte
// (el id del boton, en el mando), que es lo que devuelve la seleccion por color.
class Builder {
  constructor() {
    this.pos = []; this.nrm = []; this.col = []; this.uv = []; this.idx = [];
    this.stack = [M4.ident()]; this.parts = []; this.cur = null;
  }
  get top() { return this.stack[this.stack.length - 1]; }
  push(m) { this.stack.push(M4.mul(this.top, m)); return this; }
  pop() { this.stack.pop(); return this; }
  part(id, color, tex) {            // abre un tramo; id null = geometria no seleccionable
    this.end();
    this.cur = { id: id || null, color: color || [0.72, 0.72, 0.72], tex: tex || null,
                 start: this.idx.length, count: 0 };
    return this;
  }
  end() {
    if (this.cur) { this.cur.count = this.idx.length - this.cur.start; if (this.cur.count) this.parts.push(this.cur); }
    this.cur = null;
    return this;
  }
  add(prim) {                       // {verts, norms, tris, uvs?} en espacio local
    const m = this.top, n = M4.normal(m), base = this.pos.length / 3;
    const c = (this.cur && this.cur.color) || [0.72, 0.72, 0.72];
    const { verts, norms, tris, uvs } = prim;
    for (let i = 0; i < verts.length; i += 3) {
      const x = verts[i], y = verts[i + 1], z = verts[i + 2];
      this.pos.push(m[0] * x + m[4] * y + m[8] * z + m[12],
                    m[1] * x + m[5] * y + m[9] * z + m[13],
                    m[2] * x + m[6] * y + m[10] * z + m[14]);
      const nx = norms[i], ny = norms[i + 1], nz = norms[i + 2];
      const ax = n[0] * nx + n[3] * ny + n[6] * nz,
            ay = n[1] * nx + n[4] * ny + n[7] * nz,
            az = n[2] * nx + n[5] * ny + n[8] * nz;
      const l = Math.hypot(ax, ay, az) || 1;
      this.nrm.push(ax / l, ay / l, az / l);
      this.col.push(c[0], c[1], c[2]);
      const k = (i / 3) * 2;
      this.uv.push(uvs ? uvs[k] : 0, uvs ? uvs[k + 1] : 0);
    }
    for (const t of tris) this.idx.push(base + t);
    return this;
  }
  // Mete una malla ya construida (lo que devuelve build()) bajo la transformacion actual y
  // conserva sus tramos. `prefix` sirve para juntar dos piezas que usen los mismos ids.
  merge(mesh, prefix) {
    this.end();
    const m = this.top, n = M4.normal(m), base = this.pos.length / 3, off = this.idx.length;
    for (let i = 0; i < mesh.pos.length; i += 3) {
      const x = mesh.pos[i], y = mesh.pos[i + 1], z = mesh.pos[i + 2];
      this.pos.push(m[0] * x + m[4] * y + m[8] * z + m[12],
                    m[1] * x + m[5] * y + m[9] * z + m[13],
                    m[2] * x + m[6] * y + m[10] * z + m[14]);
      const nx = mesh.nrm[i], ny = mesh.nrm[i + 1], nz = mesh.nrm[i + 2];
      const ax = n[0] * nx + n[3] * ny + n[6] * nz,
            ay = n[1] * nx + n[4] * ny + n[7] * nz,
            az = n[2] * nx + n[5] * ny + n[8] * nz;
      const l = Math.hypot(ax, ay, az) || 1;
      this.nrm.push(ax / l, ay / l, az / l);
      this.col.push(mesh.col[i], mesh.col[i + 1], mesh.col[i + 2]);
    }
    for (let i = 0; i < mesh.uv.length; i++) this.uv.push(mesh.uv[i]);
    for (let i = 0; i < mesh.idx.length; i++) this.idx.push(base + mesh.idx[i]);
    for (const p of mesh.parts)
      this.parts.push({ id: p.id ? (prefix || "") + p.id : null, color: p.color,
                        tex: p.tex, start: off + p.start, count: p.count });
    return this;
  }
  build() {
    this.end();
    return { pos: new Float32Array(this.pos), nrm: new Float32Array(this.nrm),
             col: new Float32Array(this.col), uv: new Float32Array(this.uv),
             idx: new Uint16Array(this.idx), parts: this.parts };
  }
}

/* ------------------------------------------------------------------ primitivas */
// Caja de esquinas redondeadas: rejilla de cubo llevada hacia dentro y empujada r hacia
// fuera por la direccion de la esquina; salen redondeadas las aristas y las esquinas de
// una vez, con normales suaves. Con r = 0 es una caja recta con UV por cara.
function roundedBox(w, h, d, r, seg) {
  seg = seg || (r > 0 ? 4 : 1);
  const hx = Math.max(w / 2 - r, 0), hy = Math.max(h / 2 - r, 0), hz = Math.max(d / 2 - r, 0);
  const verts = [], norms = [], uvs = [], tris = [], n = Math.max(2, seg + 1);
  //  [u, v, eje, signo] -- las seis caras del cubo
  const faces = [[0, 1, 2, 1], [0, 1, 2, -1], [1, 2, 0, 1], [1, 2, 0, -1], [2, 0, 1, 1], [2, 0, 1, -1]];
  const half = [w / 2, h / 2, d / 2], inner = [hx, hy, hz];
  const out = [];
  for (const [u, v, a, s] of faces) {
    const base = verts.length / 3, face = { start: tris.length, count: 0 };
    for (let j = 0; j < n; j++) for (let i = 0; i < n; i++) {
      const fu = i / (n - 1), fv = j / (n - 1), p = [0, 0, 0];
      p[u] = (fu * 2 - 1) * half[u];
      p[v] = (fv * 2 - 1) * half[v];
      p[a] = s * half[a];
      const c = [Math.max(-inner[0], Math.min(inner[0], p[0])),
                 Math.max(-inner[1], Math.min(inner[1], p[1])),
                 Math.max(-inner[2], Math.min(inner[2], p[2]))];
      let dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
      const l = Math.hypot(dx, dy, dz) || 1; dx /= l; dy /= l; dz /= l;
      verts.push(c[0] + r * dx, c[1] + r * dy, c[2] + r * dz);
      norms.push(dx, dy, dz);
      uvs.push(s > 0 ? fu : 1 - fu, 1 - fv);
    }
    for (let j = 0; j < n - 1; j++) for (let i = 0; i < n - 1; i++) {
      const q = base + j * n + i;
      if (s > 0) tris.push(q, q + 1, q + n, q + 1, q + n + 1, q + n);
      else       tris.push(q, q + n, q + 1, q + 1, q + n, q + n + 1);
    }
    face.count = tris.length - face.start;
    out.push(face);
  }
  return { verts, norms, uvs, tris, faces: out };
}

// Cilindro sobre el eje Y, centrado en el origen. r1 = radio de arriba (conos truncados).
function cylinder(r, h, seg, r1) {
  seg = seg || 24; r1 = (r1 === undefined) ? r : r1;
  const verts = [], norms = [], uvs = [], tris = [];
  const slope = (r - r1) / h, nl = Math.hypot(1, slope);
  for (let i = 0; i <= seg; i++) {
    const a = i / seg * Math.PI * 2, ca = Math.cos(a), sa = Math.sin(a);
    verts.push(ca * r, -h / 2, sa * r); norms.push(ca / nl, slope / nl, sa / nl); uvs.push(i / seg, 0);
    verts.push(ca * r1, h / 2, sa * r1); norms.push(ca / nl, slope / nl, sa / nl); uvs.push(i / seg, 1);
  }
  for (let i = 0; i < seg; i++) {
    const q = i * 2;
    tris.push(q, q + 1, q + 2, q + 1, q + 3, q + 2);
  }
  for (const cap of [[-h / 2, r, -1], [h / 2, r1, 1]]) {
    const [y, rr, ny] = cap, base = verts.length / 3;
    verts.push(0, y, 0); norms.push(0, ny, 0); uvs.push(0.5, 0.5);
    for (let i = 0; i <= seg; i++) {
      const a = i / seg * Math.PI * 2;
      verts.push(Math.cos(a) * rr, y, Math.sin(a) * rr); norms.push(0, ny, 0);
      uvs.push(0.5 + Math.cos(a) * 0.5, 0.5 + Math.sin(a) * 0.5);
    }
    for (let i = 0; i < seg; i++) {
      if (ny > 0) tris.push(base, base + 2 + i, base + 1 + i);
      else        tris.push(base, base + 1 + i, base + 2 + i);
    }
  }
  return { verts, norms, uvs, tris };
}

function sphere(r, seg) {
  seg = seg || 16;
  const verts = [], norms = [], uvs = [], tris = [], rings = Math.max(3, seg >> 1);
  for (let j = 0; j <= rings; j++) {
    const phi = j / rings * Math.PI;
    for (let i = 0; i <= seg; i++) {
      const th = i / seg * Math.PI * 2;
      const x = Math.sin(phi) * Math.cos(th), y = Math.cos(phi), z = Math.sin(phi) * Math.sin(th);
      verts.push(x * r, y * r, z * r); norms.push(x, y, z); uvs.push(i / seg, 1 - j / rings);
    }
  }
  for (let j = 0; j < rings; j++) for (let i = 0; i < seg; i++) {
    const q = j * (seg + 1) + i;
    tris.push(q, q + 1, q + seg + 1, q + 1, q + seg + 2, q + seg + 1);
  }
  return { verts, norms, uvs, tris };
}

// Extrusion de un poligono XZ (en sentido horario visto desde +Y) a lo alto de Y.
function extrude(poly, h) {
  const verts = [], norms = [], uvs = [], tris = [], n = poly.length;
  for (const s of [1, -1]) {
    const base = verts.length / 3;
    for (const p of poly) { verts.push(p[0], s * h / 2, p[1]); norms.push(0, s, 0); uvs.push(0.5, 0.5); }
    for (let i = 1; i < n - 1; i++) {
      if (s > 0) tris.push(base, base + i + 1, base + i);
      else       tris.push(base, base + i, base + i + 1);
    }
  }
  for (let i = 0; i < n; i++) {
    const p0 = poly[i], p1 = poly[(i + 1) % n];
    let nx = p1[1] - p0[1], nz = -(p1[0] - p0[0]);
    const l = Math.hypot(nx, nz) || 1; nx /= l; nz /= l;
    const base = verts.length / 3;
    verts.push(p0[0], -h / 2, p0[1], p1[0], -h / 2, p1[1], p1[0], h / 2, p1[1], p0[0], h / 2, p0[1]);
    for (let k = 0; k < 4; k++) { norms.push(nx, 0, nz); uvs.push(0.5, 0.5); }
    tris.push(base, base + 2, base + 1, base, base + 3, base + 2);
  }
  return { verts, norms, uvs, tris };
}

/* --------------------------------------------------- poligonos y cuerpos lofteados */
// Triangula un poligono XZ que puede ser concavo (la silueta del mando tiene dos entrantes
// entre las palas, y un abanico desde el centro los cruzaria).
function earClip(p) {
  const n = p.length, out = [], v = [];
  for (let i = 0; i < n; i++) v.push(i);
  const cross = (a, b, c) => (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
  let sum = 0;
  for (let i = 0; i < n; i++) { const a = p[i], b = p[(i + 1) % n]; sum += a[0] * b[1] - b[0] * a[1]; }
  const ccw = sum > 0;
  const inside = (a, b, c, q) => {
    const d1 = cross(a, b, q), d2 = cross(b, c, q), d3 = cross(c, a, q);
    return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
  };
  let guard = 0;
  while (v.length > 3 && guard++ < n * n) {
    let cut = false;
    for (let k = 0; k < v.length; k++) {
      const ia = v[(k + v.length - 1) % v.length], ib = v[k], ic = v[(k + 1) % v.length];
      const a = p[ia], b = p[ib], c = p[ic], cr = cross(a, b, c);
      if (ccw ? cr <= 0 : cr >= 0) continue;                  // vertice reflejo: no es oreja
      let clean = true;
      for (const w of v) {
        if (w === ia || w === ib || w === ic) continue;
        if (inside(a, b, c, p[w])) { clean = false; break; }
      }
      if (!clean) continue;
      out.push(ia, ib, ic); v.splice(k, 1); cut = true; guard = 0; break;
    }
    if (!cut) break;                                          // poligono raro: se cierra en abanico
  }
  for (let i = 1; i < v.length - 1; i++) out.push(v[0], v[i], v[i + 1]);
  return { tris: out, ccw };
}

// Area firmada de un contorno en el plano (x, z). Positiva = antihorario en ese plano, que
// visto desde +Y es horario: de aqui sale hacia donde mira cada cara.
function chartArea(poly) {
  let a = 0;
  for (let i = 0; i < poly.length; i++) {
    const p = poly[i], q = poly[(i + 1) % poly.length];
    a += p[0] * q[1] - q[0] * p[1];
  }
  return a / 2;
}

// Normal hacia dentro de cada vertice (bisectriz) y su factor de inglete, para poder encoger
// el contorno sin que las esquinas se despeguen. El lado de dentro sale del sentido de giro
// del poligono entero, NO de mirar al centro: en un entrante la bisectriz se aleja del centro
// y ese criterio la voltearia, que es como salen faldones colgando del canto.
function polyInset(poly) {
  const n = poly.length, dir = [], mit = [];
  let sum = 0;
  for (let i = 0; i < n; i++) { const a = poly[i], b = poly[(i + 1) % n]; sum += a[0] * b[1] - b[0] * a[1]; }
  const ccw = sum > 0;
  const edgeN = (a, b) => {                       // normal interior de la arista a->b
    const dx = b[0] - a[0], dz = b[1] - a[1], l = Math.hypot(dx, dz) || 1;
    return ccw ? [-dz / l, dx / l] : [dz / l, -dx / l];
  };
  for (let i = 0; i < n; i++) {
    const p0 = poly[(i + n - 1) % n], p1 = poly[i], p2 = poly[(i + 1) % n];
    const n1 = edgeN(p0, p1), n2 = edgeN(p1, p2);
    let bx = n1[0] + n2[0], bz = n1[1] + n2[1];
    const l = Math.hypot(bx, bz) || 1; bx /= l; bz /= l;
    const cosH = Math.max(0.4, bx * n1[0] + bz * n1[1]);
    // En un vertice entrante el inglete estira hacia fuera y el canto se dobla sobre si
    // mismo: ahi se encoge recto, sin amplificar.
    const turn = ((p1[0] - p0[0]) * (p2[1] - p1[1]) - (p1[1] - p0[1]) * (p2[0] - p1[0])) * (ccw ? 1 : -1);
    dir.push([bx, bz]); mit.push(turn < 0 ? 1 : Math.min(2.0, 1 / cosH));
  }
  return { dir, mit };
}

// Extrusion con los cantos de arriba y abajo redondeados: la silueta manda y el radio solo
// mata la arista. Es lo que da forma a la carcasa del mando.
function roundPrism(poly, h, r, seg) {
  seg = seg || 3;
  // El sentido del contorno decide hacia donde miran las paredes: se normaliza aqui para
  // que dibujar la silueta en un sentido u otro de igual.
  if (chartArea(poly) > 0) poly = poly.slice().reverse();
  const ins = polyInset(poly), dir = ins.dir, mit = ins.mit, n = poly.length;
  const verts = [], norms = [], uvs = [], tris = [], rings = [];
  for (let s = -1; s <= 1; s += 2)
    for (let k = 0; k <= seg; k++) {
      const a = (Math.PI / 2) * (s < 0 ? k / seg - 1 : k / seg);
      rings.push({ a: a, y: s * (h / 2 - r) + r * Math.sin(a), in: r * (1 - Math.cos(a)) });
    }
  for (let j = 0; j < rings.length; j++) {
    const R = rings[j], ca = Math.cos(R.a), sa = Math.sin(R.a);
    for (let i = 0; i < n; i++) {
      const p = poly[i], d = dir[i], off = R.in * mit[i];
      verts.push(p[0] + d[0] * off, R.y, p[1] + d[1] * off);
      norms.push(-d[0] * ca, sa, -d[1] * ca);
      uvs.push(i / n, j / (rings.length - 1));
    }
  }
  for (let j = 0; j < rings.length - 1; j++) for (let i = 0; i < n; i++) {
    const i2 = (i + 1) % n, q = j * n, w = (j + 1) * n;
    tris.push(q + i, q + i2, w + i, q + i2, w + i2, w + i);
  }
  // Tapas: el contorno ya encogido por el radio, triangulado con recorte de orejas.
  const caps = [[0, -1], [rings.length - 1, 1]];
  for (const cap of caps) {
    const R = rings[cap[0]], up = cap[1];
    const flat = poly.map((p, i) => [p[0] + dir[i][0] * R.in * mit[i],
                                     p[1] + dir[i][1] * R.in * mit[i]]);
    const ear = earClip(flat), base = verts.length / 3;
    for (const f of flat) { verts.push(f[0], R.y, f[1]); norms.push(0, up, 0); uvs.push(0.5, 0.5); }
    // Una tapa plana en XZ mira hacia -Y cuando sus puntos van en sentido antihorario en el
    // plano (x, z): se orienta cada triangulo por su area firmada, no por el giro del contorno.
    for (let i = 0; i < ear.tris.length; i += 3) {
      const t = ear.tris, a = flat[t[i]], b2 = flat[t[i + 1]], c = flat[t[i + 2]];
      const ar = (b2[0] - a[0]) * (c[1] - a[1]) - (b2[1] - a[1]) * (c[0] - a[0]);
      if (ar * up < 0) tris.push(base + t[i], base + t[i + 1], base + t[i + 2]);
      else             tris.push(base + t[i], base + t[i + 2], base + t[i + 1]);
    }
  }
  return { verts, norms, uvs, tris };
}

// Cose una pila de anillos (todos con la misma cantidad de puntos [x,y,z]) en un tubo con
// normales suaves. Los mangos del mando son esto: secciones ovaladas que bajan y se afilan.
function loft(rings, opt) {
  opt = opt || {};
  if (chartArea(rings[0].map(p => [p[0], p[2]])) > 0)
    rings = rings.map(r => r.slice().reverse());     // mismo criterio que roundPrism
  const m = rings.length, n = rings[0].length;
  const verts = [], norms = [], uvs = [], tris = [];
  for (let j = 0; j < m; j++) for (let i = 0; i < n; i++) {
    const p = rings[j][i];
    verts.push(p[0], p[1], p[2]); norms.push(0, 0, 0); uvs.push(i / n, j / (m - 1));
  }
  const acc = (ia, ib, ic) => {
    const a = ia * 3, b = ib * 3, c = ic * 3;
    const ux = verts[b] - verts[a], uy = verts[b + 1] - verts[a + 1], uz = verts[b + 2] - verts[a + 2];
    const vx = verts[c] - verts[a], vy = verts[c + 1] - verts[a + 1], vz = verts[c + 2] - verts[a + 2];
    const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    for (const k of [a, b, c]) { norms[k] += nx; norms[k + 1] += ny; norms[k + 2] += nz; }
  };
  for (let j = 0; j < m - 1; j++) for (let i = 0; i < n; i++) {
    const i2 = (i + 1) % n, q = j * n, w = (j + 1) * n;
    tris.push(q + i, q + i2, w + i, q + i2, w + i2, w + i);
    acc(q + i, q + i2, w + i); acc(q + i2, w + i2, w + i);
  }
  // Una punta colapsada deja la normal a cero: ahi se apunta a lo largo del eje del tubo.
  const cy = rings[0][0][1] > rings[m - 1][0][1] ? -1 : 1;
  for (let i = 0; i < verts.length; i += 3) {
    const l = Math.hypot(norms[i], norms[i + 1], norms[i + 2]);
    if (l > 1e-6) { norms[i] /= l; norms[i + 1] /= l; norms[i + 2] /= l; }
    else { norms[i] = 0; norms[i + 1] = cy; norms[i + 2] = 0; }
  }
  const ends = [opt.capStart ? 0 : -1, opt.capEnd ? m - 1 : -1];
  for (const end of ends) {
    if (end < 0) continue;
    const ring = rings[end], up = end === 0 ? -1 : 1, base = verts.length / 3;
    const c = [0, 0, 0];
    for (const p of ring) { c[0] += p[0] / n; c[1] += p[1] / n; c[2] += p[2] / n; }
    verts.push(c[0], c[1], c[2]); norms.push(0, up, 0); uvs.push(0.5, 0.5);
    for (const p of ring) { verts.push(p[0], p[1], p[2]); norms.push(0, up, 0); uvs.push(0.5, 0.5); }
    for (let i = 0; i < n; i++) {
      const p0 = ring[i], p1 = ring[(i + 1) % n];
      const ar = (p0[0] - c[0]) * (p1[2] - c[2]) - (p0[2] - c[2]) * (p1[0] - c[0]);
      const a = base + 1 + i, b = base + 1 + (i + 1) % n;
      if (ar * up < 0) tris.push(base, a, b); else tris.push(base, b, a);
    }
  }
  return { verts, norms, uvs, tris };
}

// Curva suave que pasa por los puntos de control (Catmull-Rom), para siluetas sin esquinas:
// el contorno del mando se define con 30 puntos y sale con 120.
function smoothPoly(pts, per) {
  per = per || 4;
  const n = pts.length, out = [];
  for (let i = 0; i < n; i++) {
    const p0 = pts[(i + n - 1) % n], p1 = pts[i], p2 = pts[(i + 1) % n], p3 = pts[(i + 2) % n];
    for (let k = 0; k < per; k++) {
      const t = k / per, t2 = t * t, t3 = t2 * t;
      out.push([0, 1].map(c => 0.5 * ((2 * p1[c]) + (-p0[c] + p2[c]) * t +
        (2 * p0[c] - 5 * p1[c] + 4 * p2[c] - p3[c]) * t2 +
        (-p0[c] + 3 * p1[c] - 3 * p2[c] + p3[c]) * t3)));
    }
  }
  return out;
}

/* -------------------------------------------------------------------- escena */
const VS = `
attribute vec3 aPos; attribute vec3 aNrm; attribute vec3 aCol; attribute vec2 aUV;
uniform mat4 uMVP, uModel; uniform mat3 uNrmMat;
varying vec3 vN, vP, vC; varying vec2 vUV;
void main() {
  vN = normalize(uNrmMat * aNrm);
  vP = (uModel * vec4(aPos, 1.0)).xyz;
  vC = aCol; vUV = aUV;
  gl_Position = uMVP * vec4(aPos, 1.0);
}`;

const FS = `
precision mediump float;
varying vec3 vN, vP, vC; varying vec2 vUV;
uniform vec3 uEye, uTint, uPickCol;
uniform float uMix, uPick, uUseTex, uGloss, uDim;
uniform sampler2D uTex;
void main() {
  if (uPick > 0.5) { gl_FragColor = vec4(uPickCol, 1.0); return; }
  vec3 n = normalize(vN);
  vec3 l1 = normalize(vec3(-0.45, 0.85, 0.55));   // luz clave
  vec3 l2 = normalize(vec3(0.7, 0.25, -0.6));     // relleno frio
  vec3 v = normalize(uEye - vP);
  vec3 base = vC;
  if (uUseTex > 0.5) base = texture2D(uTex, vUV).rgb;
  base = mix(base, uTint, uMix);
  float d1 = max(dot(n, l1), 0.0), d2 = max(dot(n, l2), 0.0);
  float spec = pow(max(dot(normalize(l1 + v), n), 0.0), 42.0) * uGloss;
  float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);
  vec3 c = base * (0.24 + 0.76 * d1) + vec3(0.18, 0.22, 0.32) * d2 * 0.5
         + vec3(1.0) * spec * (0.25 + 0.5 * uMix) + vec3(0.45, 0.55, 0.8) * rim * 0.15;
  gl_FragColor = vec4(pow(clamp(c * uDim, 0.0, 1.0), vec3(0.4545)), 1.0);
}`;

function compile(gl, type, src) {
  const s = gl.createShader(type);
  gl.shaderSource(s, src); gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
  return s;
}

class Scene {
  constructor(canvas) {
    const opts = { antialias: true, alpha: true, premultipliedAlpha: false };
    const gl = canvas.getContext("webgl", opts) || canvas.getContext("experimental-webgl", opts);
    if (!gl) throw new Error("sin WebGL");
    this.gl = gl; this.canvas = canvas;
    const p = gl.createProgram();
    gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, VS));
    gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, FS));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    this.prog = p; gl.useProgram(p);
    this.loc = {};
    for (const k of ["uMVP", "uModel", "uNrmMat", "uEye", "uTint", "uMix", "uPick", "uPickCol",
                     "uUseTex", "uTex", "uGloss", "uDim"]) this.loc[k] = gl.getUniformLocation(p, k);
    for (const k of ["aPos", "aNrm", "aCol", "aUV"]) this.loc[k] = gl.getAttribLocation(p, k);
    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.CULL_FACE);
    gl.uniform1i(this.loc.uTex, 0);
    this.mesh = null; this.buf = {}; this.tex = {}; this.order = null;
    this.tint = [1, 0.67, 0.18];      // el acento del lanzador
    this.highlight = {};              // id -> 0..1
    this.gloss = 1;
    this.model = M4.ident();
    this.eye = [0, 6, 13]; this.target = [0, 0, 0];
  }
  setMesh(m) {
    const gl = this.gl;
    for (const k of ["pos", "nrm", "col", "uv", "idx"]) if (this.buf[k]) gl.deleteBuffer(this.buf[k]);
    const mk = (data, target) => {
      const b = gl.createBuffer();
      gl.bindBuffer(target, b); gl.bufferData(target, data, gl.STATIC_DRAW);
      return b;
    };
    this.buf.pos = mk(m.pos, gl.ARRAY_BUFFER);
    this.buf.nrm = mk(m.nrm, gl.ARRAY_BUFFER);
    this.buf.col = mk(m.col, gl.ARRAY_BUFFER);
    this.buf.uv = mk(m.uv, gl.ARRAY_BUFFER);
    this.buf.idx = mk(m.idx, gl.ELEMENT_ARRAY_BUFFER);
    this.mesh = m;
  }
  // La imagen puede no ser potencia de dos (una caratula cualquiera): sin mipmaps y con
  // CLAMP_TO_EDGE, que es lo unico que WebGL1 admite en ese caso.
  setTexture(key, img) {
    const gl = this.gl, t = this.tex[key] || gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    this.tex[key] = t;
  }
  resize() {
    const c = this.canvas, dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.round(c.clientWidth * dpr)), h = Math.max(1, Math.round(c.clientHeight * dpr));
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
    this.gl.viewport(0, 0, c.width, c.height);
    return c.width / Math.max(1, c.height);
  }
  bind() {
    const gl = this.gl, l = this.loc;
    const at = (name, buf, n) => {
      if (l[name] < 0) return;
      gl.bindBuffer(gl.ARRAY_BUFFER, buf);
      gl.enableVertexAttribArray(l[name]);
      gl.vertexAttribPointer(l[name], n, gl.FLOAT, false, 0, 0);
    };
    at("aPos", this.buf.pos, 3); at("aNrm", this.buf.nrm, 3);
    at("aCol", this.buf.col, 3); at("aUV", this.buf.uv, 2);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.buf.idx);
  }
  // pickMap != null: cada parte se dibuja con un color plano = su indice, para leer un pixel.
  draw(pickMap) {
    const gl = this.gl, l = this.loc, m = this.mesh;
    if (!m) return;
    const asp = this.resize();
    gl.useProgram(this.prog);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const proj = M4.persp(0.62, asp, 0.5, 200);
    const view = M4.lookAt(this.eye[0], this.eye[1], this.eye[2], this.target[0], this.target[1], this.target[2]);
    const pv = M4.mul(proj, view);
    // Cada parte puede traer su propia matriz (`p.mat`): asi un carrusel de veinte cajas es
    // UNA malla subida una vez y lo unico que cambia por cuadro son diecinueve uniformes.
    let lastMat = undefined;
    const setModel = mm => {
      if (mm === lastMat) return;
      lastMat = mm;
      gl.uniformMatrix4fv(l.uMVP, false, M4.mul(pv, mm));
      gl.uniformMatrix4fv(l.uModel, false, mm);
      gl.uniformMatrix3fv(l.uNrmMat, false, M4.normal(mm));
    };
    gl.uniform3fv(l.uEye, this.eye);
    gl.uniform3fv(l.uTint, this.tint);
    gl.uniform1f(l.uPick, pickMap ? 1 : 0);
    gl.uniform1f(l.uGloss, this.gloss);
    this.bind();
    const order = this.order && this.order.length === m.parts.length ? this.order : null;
    const seq = order ? order.map(i => m.parts[i]) : m.parts;
    seq.forEach((p, j) => {
      const i = order ? order[j] : j;
      if (p.hidden) return;
      setModel(p.mat ? M4.mul(this.model, p.mat) : this.model);
      if (pickMap) {
        pickMap[i + 1] = p.id;
        gl.uniform3f(l.uPickCol, ((i + 1) & 255) / 255, (((i + 1) >> 8) & 255) / 255, 0);
      } else {
        gl.uniform1f(l.uMix, p.id ? (this.highlight[p.id] || 0) : 0);
        gl.uniform1f(l.uDim, p.dim === undefined ? 1 : p.dim);
        const t = p.tex && this.tex[p.tex];
        gl.uniform1f(l.uUseTex, t ? 1 : 0);
        if (t) { gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, t); }
      }
      gl.drawElements(gl.TRIANGLES, p.count, gl.UNSIGNED_SHORT, p.start * 2);
    });
  }
  // Id de la parte bajo el raton, o null. Vuelve a dibujar normal antes de salir, asi que
  // el usuario nunca llega a ver la pasada de colores planos.
  pick(clientX, clientY) {
    const gl = this.gl, r = this.canvas.getBoundingClientRect();
    if (!r.width || !r.height) return null;
    const dpr = this.canvas.width / r.width;
    const x = Math.round((clientX - r.left) * dpr), y = Math.round((r.bottom - clientY) * dpr);
    const map = {};
    this.draw(map);
    const px = new Uint8Array(4);
    gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    this.draw(null);
    return map[px[0] | (px[1] << 8)] || null;
  }
  dispose() {
    const gl = this.gl;
    for (const k in this.buf) if (this.buf[k]) gl.deleteBuffer(this.buf[k]);
    for (const k in this.tex) if (this.tex[k]) gl.deleteTexture(this.tex[k]);
    gl.deleteProgram(this.prog);
    this.buf = {}; this.tex = {}; this.mesh = null;
  }
}

return { M4, Builder, Scene, roundedBox, cylinder, sphere, extrude,
         earClip, polyInset, roundPrism, loft, smoothPoly, chartArea };
})();

if (typeof module !== "undefined") module.exports = GL;   // para las pruebas con node
