// Posproceso del presentador (ver postfx.hpp). Una cadena de pases compute: cada pase lee
// hasta 8 imagenes por muestreador lineal y escribe una imagen RGBA16F. Todas las imagenes
// viven en layout GENERAL de principio a fin; entre pases solo hace falta una barrera de
// memoria. Las imagenes se crean al vuelo con el tamano que pide cada cuadro y se rehacen
// cuando cambia (ventana redimensionada, VI que cambia de resolucion).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#ifdef KESTREL_PRDP
#define VK_NO_PROTOTYPES
#include <volk.h>
#else
#include <vulkan/vulkan.h>
#endif

#include "postfx.hpp"

namespace kestrel::postfx {

namespace {

struct SpvBlob {
  const char* name;
  const uint32_t* code;
  size_t bytes;
};

struct NetPass {
  const char* pass;     // shader
  const char* out;      // nombre de la imagen que escribe
  const char* ref;      // su tamano es el de esta imagen ...
  int scale;            // ... por este factor
  int nin;
  const char* in[7];    // entradas t0.. (MAIN = cuadro del invitado)
};

#include "postfx_spv.inc"

constexpr u32 kSlots = 9;      // t0..t7 + salida
constexpr u32 kMaxPasses = 24;
constexpr VkFormat kFmt = VK_FORMAT_R16G16B16A16_SFLOAT;

struct Img {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  u32 w = 0, h = 0;
  bool fresh = true;   // recien creada: aun en UNDEFINED
};

struct PC {
  float inSize[4], outSize[4], srcSize[4], param[4];
};

}  // namespace

struct State {
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorPool dpool = VK_NULL_HANDLE;
  std::map<std::string, VkPipeline> pipes;
  std::map<std::string, Img> imgs;
  Img srcOpt;            // copia optima (muestreable) del cuadro, RGBA8
  bool broken = false;   // algo fallo al crear: no volver a intentarlo cada cuadro

  // KESTREL_FXDUMP=<f.ppm>: vuelca UNA vez la salida del filtro, en el cuadro presentado
  // numero KESTREL_FXDUMP_AT (por defecto 300). Desarrollo: la captura de pantalla de una
  // ventana Vulkan no es fiable, y esto es justo lo que se copia a la cadena.
  const char* dumpPath = nullptr;
  u32 dumpAt = 300, frames = 0;
  int dumpState = 0;     // 0 esperando, 1 copia grabada en este cuadro, 2 hecho
  VkBuffer dumpBuf = VK_NULL_HANDLE;
  VkDeviceMemory dumpMem = VK_NULL_HANDLE;
  u32 dumpW = 0, dumpH = 0;
};

namespace {

auto memType(VkPhysicalDevice pd, u32 bits, VkMemoryPropertyFlags want) -> u32 {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for(u32 i = 0; i < mp.memoryTypeCount; i++)
    if((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return ~0u;
}

auto freeImg(State* s, Img& im) -> void {
  if(im.view) vkDestroyImageView(s->dev, im.view, nullptr);
  if(im.image) vkDestroyImage(s->dev, im.image, nullptr);
  if(im.mem) vkFreeMemory(s->dev, im.mem, nullptr);
  im = Img{};
}

auto makeImg(State* s, Img& im, u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage) -> bool {
  if(im.image && im.w == w && im.h == h) return true;
  freeImg(s, im);
  VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = fmt;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if(vkCreateImage(s->dev, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(s->dev, im.image, &mr);
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = memType(s->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if(ai.memoryTypeIndex == ~0u) ai.memoryTypeIndex = memType(s->phys, mr.memoryTypeBits, 0);
  if(vkAllocateMemory(s->dev, &ai, nullptr, &im.mem) != VK_SUCCESS) { freeImg(s, im); return false; }
  vkBindImageMemory(s->dev, im.image, im.mem, 0);
  VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = im.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = fmt;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if(vkCreateImageView(s->dev, &vi, nullptr, &im.view) != VK_SUCCESS) { freeImg(s, im); return false; }
  im.w = w;
  im.h = h;
  im.fresh = true;
  return true;
}

auto pipeline(State* s, const char* name) -> VkPipeline {
  auto it = s->pipes.find(name);
  if(it != s->pipes.end()) return it->second;
  const SpvBlob* b = nullptr;
  for(const auto& x : kSpvBlobs)
    if(std::strcmp(x.name, name) == 0) b = &x;
  VkPipeline p = VK_NULL_HANDLE;
  if(b) {
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = b->bytes;
    mi.pCode = b->code;
    VkShaderModule mod = VK_NULL_HANDLE;
    if(vkCreateShaderModule(s->dev, &mi, nullptr, &mod) == VK_SUCCESS) {
      VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      ci.stage.module = mod;
      ci.stage.pName = "main";
      ci.layout = s->layout;
      if(vkCreateComputePipelines(s->dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS)
        p = VK_NULL_HANDLE;
      vkDestroyShaderModule(s->dev, mod, nullptr);
    }
  }
  if(!p) std::fprintf(stderr, "[postfx] no se pudo crear el pase %s\n", name);
  s->pipes[name] = p;
  return p;
}

auto toGeneral(VkCommandBuffer cb, Img& im) -> void {
  if(!im.fresh) return;
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &b);
  im.fresh = false;
}

auto memBarrier(VkCommandBuffer cb, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss,
                VkPipelineStageFlags ds) -> void {
  VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  m.srcAccessMask = sa;
  m.dstAccessMask = da;
  vkCmdPipelineBarrier(cb, ss, ds, 0, 1, &m, 0, nullptr, 0, nullptr);
}

auto sizeVec(float* v, u32 w, u32 h) -> void {
  v[0] = (float)w;
  v[1] = (float)h;
  v[2] = 1.0f / (float)w;
  v[3] = 1.0f / (float)h;
}

// Un pase: `ins` (hasta 8) -> `out`. Las ranuras sin entrada apuntan al cuadro original.
struct Rec {
  State* s;
  VkCommandBuffer cb;
  u32 sw, sh;
  u32 n = 0;
  bool ok = true;

  auto pass(const char* shader, std::initializer_list<Img*> ins, Img& out, const float* param)
      -> void {
    if(!ok) return;
    VkPipeline p = pipeline(s, shader);
    if(!p || n >= kMaxPasses) { ok = false; return; }
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s->dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s->dsl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if(vkAllocateDescriptorSets(s->dev, &ai, &set) != VK_SUCCESS) { ok = false; return; }
    VkDescriptorImageInfo ii[kSlots] = {};
    VkWriteDescriptorSet w[kSlots] = {};
    u32 k = 0;
    std::vector<Img*> v(ins);
    for(u32 i = 0; i < 8; i++) {
      Img* im = i < v.size() ? v[i] : &s->srcOpt;
      ii[i] = {s->sampler, im->view, VK_IMAGE_LAYOUT_GENERAL};
      w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[i].dstSet = set;
      w[i].dstBinding = i;
      w[i].descriptorCount = 1;
      w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w[i].pImageInfo = &ii[i];
      k++;
    }
    ii[8] = {VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL};
    w[8] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[8].dstSet = set;
    w[8].dstBinding = 8;
    w[8].descriptorCount = 1;
    w[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[8].pImageInfo = &ii[8];
    k++;
    vkUpdateDescriptorSets(s->dev, k, w, 0, nullptr);

    PC pc = {};
    Img* in0 = v.empty() ? &s->srcOpt : v[0];
    sizeVec(pc.inSize, in0->w, in0->h);
    sizeVec(pc.outSize, out.w, out.h);
    sizeVec(pc.srcSize, sw, sh);
    if(param) std::memcpy(pc.param, param, sizeof pc.param);

    if(n) memBarrier(cb, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, s->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (out.w + 7) / 8, (out.h + 7) / 8, 1);
    n++;
  }
};

auto img(State* s, const std::string& name, u32 w, u32 h, VkCommandBuffer cb, bool& ok) -> Img& {
  Img& im = s->imgs[name];
  if(!ok) return im;
  if(!makeImg(s, im, w, h, kFmt,
              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
    ok = false;
    return im;
  }
  toGeneral(cb, im);
  return im;
}

}  // namespace

auto create(VkPhysicalDevice phys, VkDevice dev) -> State* {
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(phys, kFmt, &fp);
  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
  if((fp.optimalTilingFeatures & need) != need) {
    std::fprintf(stderr, "[postfx] la GPU no admite RGBA16F para muestrear/escribir/copiar; filtros apagados\n");
    return nullptr;
  }
  State* s = new State;
  s->phys = phys;
  s->dev = dev;
  s->dumpPath = std::getenv("KESTREL_FXDUMP");
  if(const char* a = std::getenv("KESTREL_FXDUMP_AT")) s->dumpAt = (u32)std::atoi(a);

  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.maxLod = 0.0f;
  bool ok = vkCreateSampler(dev, &sci, nullptr, &s->sampler) == VK_SUCCESS;

  VkDescriptorSetLayoutBinding b[kSlots] = {};
  for(u32 i = 0; i < kSlots; i++) {
    b[i].binding = i;
    b[i].descriptorType = i < 8 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[i].descriptorCount = 1;
    b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dli.bindingCount = kSlots;
  dli.pBindings = b;
  ok = ok && vkCreateDescriptorSetLayout(dev, &dli, nullptr, &s->dsl) == VK_SUCCESS;

  VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PC)};
  VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &s->dsl;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  ok = ok && vkCreatePipelineLayout(dev, &pli, nullptr, &s->layout) == VK_SUCCESS;

  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * kMaxPasses},
                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxPasses}};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = kMaxPasses;
  dpi.poolSizeCount = 2;
  dpi.pPoolSizes = ps;
  ok = ok && vkCreateDescriptorPool(dev, &dpi, nullptr, &s->dpool) == VK_SUCCESS;
  if(!ok) {
    std::fprintf(stderr, "[postfx] fallo al crear el estado; filtros apagados\n");
    destroy(s);
    return nullptr;
  }
  return s;
}

auto destroy(State* s) -> void {
  if(!s) return;
  if(s->dumpBuf) vkDestroyBuffer(s->dev, s->dumpBuf, nullptr);
  if(s->dumpMem) vkFreeMemory(s->dev, s->dumpMem, nullptr);
  for(auto& [n, p] : s->pipes)
    if(p) vkDestroyPipeline(s->dev, p, nullptr);
  for(auto& [n, im] : s->imgs) freeImg(s, im);
  freeImg(s, s->srcOpt);
  if(s->dpool) vkDestroyDescriptorPool(s->dev, s->dpool, nullptr);
  if(s->layout) vkDestroyPipelineLayout(s->dev, s->layout, nullptr);
  if(s->dsl) vkDestroyDescriptorSetLayout(s->dev, s->dsl, nullptr);
  if(s->sampler) vkDestroySampler(s->dev, s->sampler, nullptr);
  delete s;
}

auto record(State* s, VkCommandBuffer cb, VkImage src, u32 sw, u32 sh, u32 dw, u32 dh, int f)
    -> VkImage {
  if(!s || s->broken || f < Sharp || f >= Count || !sw || !sh || !dw || !dh) return VK_NULL_HANDLE;
  // El cuadro anterior ya termino (valla esperada): sus juegos de descriptores se reciclan.
  vkResetDescriptorPool(s->dev, s->dpool, 0);

  // 1) copia del cuadro lineal a una imagen optima muestreable
  if(!makeImg(s, s->srcOpt, sw, sh, VK_FORMAT_R8G8B8A8_UNORM,
              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    s->broken = true;
    return VK_NULL_HANDLE;
  }
  toGeneral(cb, s->srcOpt);
  memBarrier(cb, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageCopy cp = {};
  cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  cp.extent = {sw, sh, 1};
  vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, s->srcOpt.image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
  memBarrier(cb, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  // 2) la cadena del filtro
  bool ok = true;
  Rec r{s, cb, sw, sh};
  Img* src0 = &s->srcOpt;
  Img& out = img(s, "OUT", dw, dh, cb, ok);
  if(!ok) { s->broken = true; return VK_NULL_HANDLE; }
  static const float kRcas[4] = {0.2f, 0, 0, 0};
  static const float kCrt[4] = {0.6f, 0, 0, 0};
  static const float kCel[4] = {5.0f, 0.45f, 1.15f, 0};
  static const float kFlat[4] = {1.0f, 0, 0, 0};
  static const float kOleo[4] = {2.0f, 0, 0, 0};
  switch(f) {
    case Sharp:
      r.pass("sharp", {src0}, out, nullptr);
      break;
    case Fsr: {
      Img& t = img(s, "T0", dw, dh, cb, ok);
      if(!ok) break;
      r.pass("easu", {src0}, t, nullptr);
      r.pass("rcas", {&t}, out, kRcas);
      break;
    }
    case Crt:
      r.pass("crt", {src0}, out, kCrt);
      break;
    case Cel: {
      Img& t = img(s, "T0", dw, dh, cb, ok);
      if(!ok) break;
      Img& t1 = img(s, "T1", dw, dh, cb, ok);
      if(!ok) break;
      r.pass("easu", {src0}, t, nullptr);
      r.pass("kuwahara", {&t}, t1, kFlat);
      r.pass("cel", {&t1, src0}, out, kCel);
      break;
    }
    case Oleo: {
      Img& t = img(s, "T0", dw, dh, cb, ok);
      if(!ok) break;
      r.pass("easu", {src0}, t, nullptr);
      r.pass("kuwahara", {&t}, out, kOleo);
      break;
    }
    case Anime4k: {
      // red x2 a la resolucion del invitado; MAIN de entrada es el cuadro, MAIN de salida
      // es la imagen doble ("A4K"). Luego FSR la lleva al tamano de la ventana.
      std::map<std::string, Img*> named;
      named["MAIN"] = src0;
      for(const NetPass& np : kNet_a4k_m) {
        Img* ref = named.count(np.ref) ? named[np.ref] : nullptr;
        if(!ref) { ok = false; break; }
        std::string oname = std::strcmp(np.out, "MAIN") == 0 ? std::string("A4K") : np.out;
        Img& o = img(s, oname, ref->w * np.scale, ref->h * np.scale, cb, ok);
        if(!ok) break;
        std::vector<Img*> ins;
        for(int i = 0; i < np.nin; i++) {
          if(!named.count(np.in[i])) { ok = false; break; }
          ins.push_back(named[np.in[i]]);
        }
        if(!ok) break;
        // pass() toma una initializer_list; aqui el numero de entradas es variable
        Img* a[8] = {};
        for(size_t i = 0; i < ins.size(); i++) a[i] = ins[i];
        switch(ins.size()) {
          case 1: r.pass(np.pass, {a[0]}, o, nullptr); break;
          case 2: r.pass(np.pass, {a[0], a[1]}, o, nullptr); break;
          case 7: r.pass(np.pass, {a[0], a[1], a[2], a[3], a[4], a[5], a[6]}, o, nullptr); break;
          default: ok = false; break;
        }
        named[np.out] = &o;
      }
      if(!ok) break;
      Img* up = named["MAIN"];
      Img& t = img(s, "T0", dw, dh, cb, ok);
      if(!ok) break;
      r.pass("easu", {up}, t, nullptr);
      r.pass("rcas", {&t}, out, kRcas);
      break;
    }
    default:
      ok = false;
  }
  if(!ok || !r.ok) {
    std::fprintf(stderr, "[postfx] la cadena del filtro %d fallo; vuelta al blit de siempre\n", f);
    s->broken = true;
    return VK_NULL_HANDLE;
  }
  memBarrier(cb, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  if(s->dumpPath && s->dumpState == 0 && ++s->frames >= s->dumpAt) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (VkDeviceSize)dw * dh * 8;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if(vkCreateBuffer(s->dev, &bi, nullptr, &s->dumpBuf) == VK_SUCCESS) {
      VkMemoryRequirements mr;
      vkGetBufferMemoryRequirements(s->dev, s->dumpBuf, &mr);
      VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = mr.size;
      ai.memoryTypeIndex = memType(s->phys, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if(ai.memoryTypeIndex != ~0u && vkAllocateMemory(s->dev, &ai, nullptr, &s->dumpMem) == VK_SUCCESS) {
        vkBindBufferMemory(s->dev, s->dumpBuf, s->dumpMem, 0);
        VkBufferImageCopy c = {};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {dw, dh, 1};
        vkCmdCopyImageToBuffer(cb, out.image, VK_IMAGE_LAYOUT_GENERAL, s->dumpBuf, 1, &c);
        memBarrier(cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        s->dumpW = dw;
        s->dumpH = dh;
        s->dumpState = 1;
      }
    }
    if(s->dumpState != 1) s->dumpState = 2;
  }
  return out.image;
}

namespace {
auto halfToFloat(uint16_t h) -> float {
  u32 e = (h >> 10) & 31, m = h & 1023;
  float v = e == 0 ? (float)m / 16777216.0f
                   : e == 31 ? 65504.0f : std::ldexp(1.0f + (float)m / 1024.0f, (int)e - 15);
  return (h & 0x8000) ? -v : v;
}
}  // namespace

auto frameDone(State* s) -> void {
  if(!s || s->dumpState != 1) return;
  s->dumpState = 2;
  void* p = nullptr;
  if(vkMapMemory(s->dev, s->dumpMem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return;
  const uint16_t* h = (const uint16_t*)p;
  if(FILE* f = std::fopen(s->dumpPath, "wb")) {
    std::fprintf(f, "P6\n%u %u\n255\n", s->dumpW, s->dumpH);
    std::vector<uint8_t> row(s->dumpW * 3);
    for(u32 y = 0; y < s->dumpH; y++) {
      for(u32 x = 0; x < s->dumpW; x++)
        for(int c = 0; c < 3; c++) {
          float v = halfToFloat(h[((size_t)y * s->dumpW + x) * 4 + c]);
          row[x * 3 + c] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f + 0.5f));
        }
      std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[postfx] volcado %ux%u -> %s\n", s->dumpW, s->dumpH, s->dumpPath);
  }
  vkUnmapMemory(s->dev, s->dumpMem);
}

}  // namespace kestrel::postfx
