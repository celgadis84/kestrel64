// Cabecera comun de los pases de posproceso del presentador (src/video/postfx.cpp).
//
// Cada pase es un compute shader que lee hasta 8 texturas (t0..t7, muestreador lineal con
// bordes fijados) y escribe UNA imagen RGBA16F. El pase no sabe de donde vienen sus
// entradas: la cadena la monta postfx.cpp. Grupo 8x8.
//
//   pc.inSize  = (ancho, alto, 1/ancho, 1/alto) de t0
//   pc.outSize = lo mismo de la imagen de salida
//   pc.srcSize = lo mismo del cuadro ORIGINAL del invitado (antes de cualquier pase); los
//                filtros esteticos miden en texeles del invitado aunque trabajen a la
//                resolucion de la ventana
//   pc.param   = parametros propios de cada pase
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D t0;
layout(set = 0, binding = 1) uniform sampler2D t1;
layout(set = 0, binding = 2) uniform sampler2D t2;
layout(set = 0, binding = 3) uniform sampler2D t3;
layout(set = 0, binding = 4) uniform sampler2D t4;
layout(set = 0, binding = 5) uniform sampler2D t5;
layout(set = 0, binding = 6) uniform sampler2D t6;
layout(set = 0, binding = 7) uniform sampler2D t7;
layout(set = 0, binding = 8, rgba16f) uniform writeonly image2D outImg;

layout(push_constant) uniform PC {
  vec4 inSize;
  vec4 outSize;
  vec4 srcSize;
  vec4 param;
} pc;

// Pixel de salida de esta invocacion; false = fuera de la imagen (el ultimo grupo sobra).
bool outPixel(out ivec2 p) {
  p = ivec2(gl_GlobalInvocationID.xy);
  return p.x < int(pc.outSize.x) && p.y < int(pc.outSize.y);
}

// Centro del pixel de salida en coordenadas normalizadas (0..1), igual para todas las
// entradas: todas cubren la misma imagen, solo cambia su resolucion.
vec2 outUv(ivec2 p) { return (vec2(p) + 0.5) * pc.outSize.zw; }

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
