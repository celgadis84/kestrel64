// kestrel64 -- GPU-RDP propio, fase 0 (ver gpurdp.hpp y docs/GPU-RDP.md).
//
// Vulkan a pelo sobre volk (el mismo cargador que usa el presentador): una instancia, un
// dispositivo con una cola grafica+compute que el presentador comparte, dos buffers
// visibles desde el anfitrion (espejo de la RDRAM y de sus bits ocultos) y un pipeline de
// compute por primitiva. Nada de Granite: todo lo que hay aqui es codigo propio.
#include "gpurdp.hpp"

// Las estructuras de Vulkan se inicializan como {sType} y el resto a cero: es lo idiomatico,
// no un olvido.
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#define VK_NO_PROTOTYPES
#include <volk.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace kestrel::gpurdp {

#include "gpurdp_spv.inc"

namespace {

struct Push {
  u32 addr, width, bpp, x0, y0, x1, y1, color, base, count, pass;
};

struct Buf {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  u8* map = nullptr;
  VkDeviceSize size = 0;
  bool coherent = true;
};

struct Ctx {
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  u32 qfamily = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  Buf ram, hid;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout pl = VK_NULL_HANDLE;
  VkPipeline fill = VK_NULL_HANDLE;
  VkDescriptorPool dpool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  std::vector<FillRect> q;
  u64 nFill = 0, nFlush = 0;
  vrdp::SharedVk shared{};
};

Ctx* g = nullptr;
std::atomic<bool> g_live{false};

auto pickMemory(VkPhysicalDevice pd, u32 typeBits, bool& coherent) -> int {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  // Se lee de vuelta cada flush: mejor memoria cacheada en el anfitrion. Coherente si se puede,
  // si no se invalida a mano antes de leer.
  const VkMemoryPropertyFlags want[] = {
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
  };
  for(auto f : want)
    for(u32 i = 0; i < mp.memoryTypeCount; i++)
      if((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) {
        coherent = (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        return (int)i;
      }
  return -1;
}

auto makeBuf(Ctx& c, Buf& b, VkDeviceSize size) -> bool {
  b.size = (size + 3) & ~VkDeviceSize(3);
  VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = b.size;
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if(vkCreateBuffer(c.dev, &bi, nullptr, &b.buf) != VK_SUCCESS) return false;
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(c.dev, b.buf, &mr);
  int t = pickMemory(c.phys, mr.memoryTypeBits, b.coherent);
  if(t < 0) return false;
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = (u32)t;
  if(vkAllocateMemory(c.dev, &ai, nullptr, &b.mem) != VK_SUCCESS) return false;
  if(vkBindBufferMemory(c.dev, b.buf, b.mem, 0) != VK_SUCCESS) return false;
  void* p = nullptr;
  if(vkMapMemory(c.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
  b.map = (u8*)p;
  std::memset(b.map, 0, (size_t)b.size);
  return true;
}

auto freeBuf(Ctx& c, Buf& b) -> void {
  if(b.map) vkUnmapMemory(c.dev, b.mem);
  if(b.buf) vkDestroyBuffer(c.dev, b.buf, nullptr);
  if(b.mem) vkFreeMemory(c.dev, b.mem, nullptr);
  b = Buf{};
}

auto hasExt(const std::vector<VkExtensionProperties>& v, const char* n) -> bool {
  for(auto& e : v) if(!std::strcmp(e.extensionName, n)) return true;
  return false;
}

auto initVk(Ctx& c, u32 rdramSize) -> bool {
  if(volkInitialize() != VK_SUCCESS) { std::fprintf(stderr, "[gpurdp] sin cargador Vulkan\n"); return false; }
  // Extensiones de superficie si las hay: el presentador comparte esta instancia y crea su
  // superficie sobre ella (igual que con parallel-RDP, ver vrdp.cpp).
  u32 n = 0; vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> ie(n);
  vkEnumerateInstanceExtensionProperties(nullptr, &n, ie.data());
  std::vector<const char*> iext;
  for(const char* e : {"VK_KHR_surface",
#ifdef _WIN32
                       "VK_KHR_win32_surface"
#else
                       "VK_KHR_xlib_surface"
#endif
                      })
    if(hasExt(ie, e)) iext.push_back(e);
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "kestrel64-gpurdp"; app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = (u32)iext.size(); ici.ppEnabledExtensionNames = iext.data();
  if(vkCreateInstance(&ici, nullptr, &c.inst) != VK_SUCCESS) {
    std::fprintf(stderr, "[gpurdp] vkCreateInstance fallo\n"); c.inst = VK_NULL_HANDLE; return false;
  }
  volkLoadInstance(c.inst);

  u32 np = 0; vkEnumeratePhysicalDevices(c.inst, &np, nullptr);
  if(!np) { std::fprintf(stderr, "[gpurdp] sin dispositivos Vulkan\n"); return false; }
  std::vector<VkPhysicalDevice> pds(np);
  vkEnumeratePhysicalDevices(c.inst, &np, pds.data());
  // Una cola grafica (el presentador la usa para presentar) que tambien haga compute. Se
  // prefiere GPU discreta, igual que el presentador cuando elige solo.
  int best = -1;
  for(auto pd : pds) {
    u32 nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs.data());
    for(u32 i = 0; i < nq; i++) {
      const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
      if((qs[i].queueFlags & need) != need) continue;
      VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
      int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
      if(score > best) { best = score; c.phys = pd; c.qfamily = i; }
      break;
    }
  }
  if(c.phys == VK_NULL_HANDLE) { std::fprintf(stderr, "[gpurdp] sin cola grafica+compute\n"); return false; }

  u32 nd = 0; vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &nd, nullptr);
  std::vector<VkExtensionProperties> de(nd);
  vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &nd, de.data());
  std::vector<const char*> dext;
  if(hasExt(de, "VK_KHR_swapchain")) dext.push_back("VK_KHR_swapchain");
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = c.qfamily; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = (u32)dext.size(); dci.ppEnabledExtensionNames = dext.data();
  if(vkCreateDevice(c.phys, &dci, nullptr, &c.dev) != VK_SUCCESS) {
    std::fprintf(stderr, "[gpurdp] vkCreateDevice fallo\n"); c.dev = VK_NULL_HANDLE; return false;
  }
  volkLoadDevice(c.dev);
  vkGetDeviceQueue(c.dev, c.qfamily, 0, &c.queue);

  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = c.qfamily;
  if(vkCreateCommandPool(c.dev, &pci, nullptr, &c.pool) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = c.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
  if(vkAllocateCommandBuffers(c.dev, &cai, &c.cmd) != VK_SUCCESS) return false;
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if(vkCreateFence(c.dev, &fci, nullptr, &c.fence) != VK_SUCCESS) return false;

  if(!makeBuf(c, c.ram, rdramSize) || !makeBuf(c, c.hid, rdramSize / 2)) {
    std::fprintf(stderr, "[gpurdp] no hay memoria visible para el espejo de RDRAM\n"); return false;
  }

  VkDescriptorSetLayoutBinding b[2] = {};
  for(u32 i = 0; i < 2; i++) {
    b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dli.bindingCount = 2; dli.pBindings = b;
  if(vkCreateDescriptorSetLayout(c.dev, &dli, nullptr, &c.dsl) != VK_SUCCESS) return false;
  VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
  VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1; pli.pSetLayouts = &c.dsl;
  pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
  if(vkCreatePipelineLayout(c.dev, &pli, nullptr, &c.pl) != VK_SUCCESS) return false;

  VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  mi.codeSize = sizeof kSpv_fill; mi.pCode = kSpv_fill;
  VkShaderModule mod = VK_NULL_HANDLE;
  if(vkCreateShaderModule(c.dev, &mi, nullptr, &mod) != VK_SUCCESS) return false;
  VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = mod; cpi.stage.pName = "main";
  cpi.layout = c.pl;
  VkResult r = vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &c.fill);
  vkDestroyShaderModule(c.dev, mod, nullptr);
  if(r != VK_SUCCESS) return false;

  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
  if(vkCreateDescriptorPool(c.dev, &dpi, nullptr, &c.dpool) != VK_SUCCESS) return false;
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = c.dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &c.dsl;
  if(vkAllocateDescriptorSets(c.dev, &dai, &c.set) != VK_SUCCESS) return false;
  VkDescriptorBufferInfo bufs[2] = {{c.ram.buf, 0, VK_WHOLE_SIZE}, {c.hid.buf, 0, VK_WHOLE_SIZE}};
  VkWriteDescriptorSet wr[2] = {};
  for(u32 i = 0; i < 2; i++) {
    wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = c.set; wr[i].dstBinding = i;
    wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr[i].pBufferInfo = &bufs[i];
  }
  vkUpdateDescriptorSets(c.dev, 2, wr, 0, nullptr);

  VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(c.phys, &p);
  std::fprintf(stderr, "[gpurdp] gpu=\"%s\" cola=%u espejo=%u KB coherente=%d\n", p.deviceName,
               c.qfamily, (unsigned)(c.ram.size >> 10), (int)c.ram.coherent);
  return true;
}

auto destroy(Ctx& c) -> void {
  if(c.dev) {
    vkDeviceWaitIdle(c.dev);
    if(c.fill) vkDestroyPipeline(c.dev, c.fill, nullptr);
    if(c.pl) vkDestroyPipelineLayout(c.dev, c.pl, nullptr);
    if(c.dpool) vkDestroyDescriptorPool(c.dev, c.dpool, nullptr);
    if(c.dsl) vkDestroyDescriptorSetLayout(c.dev, c.dsl, nullptr);
    freeBuf(c, c.ram); freeBuf(c, c.hid);
    if(c.fence) vkDestroyFence(c.dev, c.fence, nullptr);
    if(c.pool) vkDestroyCommandPool(c.dev, c.pool, nullptr);
    vkDestroyDevice(c.dev, nullptr);
  }
  if(c.inst) vkDestroyInstance(c.inst, nullptr);
}

auto barrier(VkCommandBuffer cmd, VkAccessFlags dstAcc, VkPipelineStageFlags dstStage) -> void {
  VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = dstAcc;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dstStage, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

}  // namespace

auto wanted() -> bool {
  const char* e = std::getenv("KESTREL_GPURDP");
  return e && e[0] && e[0] != '0';
}

auto init(u32 rdramSize) -> bool {
  if(g || !wanted()) return g != nullptr;
  g = new Ctx();
  if(!initVk(*g, rdramSize)) {
    std::fprintf(stderr, "[gpurdp] no arranca; se queda SoftRDP\n");
    destroy(*g); delete g; g = nullptr;
    return false;
  }
  g->shared = {(void*)g->inst, (void*)g->phys, (void*)g->dev, g->qfamily, (void*)g->queue};
  g_live.store(true, std::memory_order_release);
  // Resumen al salir: prueba de que los rellenos pasaron de verdad por la GPU. Solo cuenta;
  // destruir Vulkan aqui seria pisar a hilos que aun pueden estar vivos.
  std::atexit([] {
    if(g) std::fprintf(stderr, "[gpurdp] rellenos=%llu vaciados=%llu\n",
                       (unsigned long long)g->nFill, (unsigned long long)g->nFlush);
  });
  return true;
}

auto shutdown() -> void {
  if(!g) return;
  g_live.store(false, std::memory_order_release);
  std::fprintf(stderr, "[gpurdp] rellenos=%llu vaciados=%llu\n",
               (unsigned long long)g->nFill, (unsigned long long)g->nFlush);
  destroy(*g); delete g; g = nullptr;
}

auto active() -> bool { return g_live.load(std::memory_order_acquire); }

auto sharedVk() -> const vrdp::SharedVk* { return active() ? &g->shared : nullptr; }

auto queueFill(const FillRect& r) -> void { g->q.push_back(r); g->nFill++; }

auto pending() -> bool { return g && !g->q.empty(); }

auto flush(u8* rdram, u32 size, u8* hidden) -> void {
  if(!g || g->q.empty()) return;
  Ctx& c = *g;
  vkResetCommandBuffer(c.cmd, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(c.cmd, &bi);
  vkCmdBindPipeline(c.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.fill);
  vkCmdBindDescriptorSets(c.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.set, 0, nullptr);
  bool first = true;
  for(const FillRect& r : c.q) {
    // Rango de bytes del rectangulo (filas consecutivas): de su primer byte al ultimo.
    const u32 lo = r.addr + (r.y0 * r.width + r.x0) * r.bpp;
    const u32 hi = r.addr + ((r.y1 - 1) * r.width + r.x1) * r.bpp - 1;
    // Rectangulos en orden, como los pinta SoftRDP: si dos se solapan gana el ultimo.
    if(!first) barrier(c.cmd, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    first = false;
    Push p = {r.addr, r.width, r.bpp, r.x0, r.y0, r.x1, r.y1, r.color, lo >> 2, (hi >> 2) - (lo >> 2) + 1, 0};
    vkCmdPushConstants(c.cmd, c.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(c.cmd, (p.count + 63) / 64, 1, 1);
    if(r.bpp == 2) {
      // Bits ocultos: entrada e = media palabra 2e; cuatro entradas por palabra del buffer.
      p.base = (lo >> 1) >> 2; p.count = ((hi >> 1) >> 2) - p.base + 1; p.pass = 1;
      vkCmdPushConstants(c.cmd, c.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
      vkCmdDispatch(c.cmd, (p.count + 63) / 64, 1, 1);
    }
  }
  barrier(c.cmd, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
  vkEndCommandBuffer(c.cmd);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1; si.pCommandBuffers = &c.cmd;
  vrdp::queueLock();
  VkResult sr = vkQueueSubmit(c.queue, 1, &si, c.fence);
  vrdp::queueUnlock();
  if(sr != VK_SUCCESS) {
    std::fprintf(stderr, "[gpurdp] vkQueueSubmit fallo (%d)\n", (int)sr);
    c.q.clear(); return;
  }
  vkWaitForFences(c.dev, 1, &c.fence, VK_TRUE, ~0ull);
  vkResetFences(c.dev, 1, &c.fence);
  if(!c.ram.coherent || !c.hid.coherent) {
    VkMappedMemoryRange mr[2] = {{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}, {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}};
    mr[0].memory = c.ram.mem; mr[0].size = VK_WHOLE_SIZE;
    mr[1].memory = c.hid.mem; mr[1].size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(c.dev, 2, mr);
  }
  // De vuelta solo los bytes de los rectangulos: el resto del espejo puede estar viejo.
  for(const FillRect& r : c.q) {
    const u32 n = (r.x1 - r.x0) * r.bpp;
    for(u32 y = r.y0; y < r.y1; y++) {
      const u32 a = r.addr + (y * r.width + r.x0) * r.bpp;
      if(a + n > size) break;
      std::memcpy(rdram + a, c.ram.map + a, n);
      if(r.bpp == 2 && hidden) std::memcpy(hidden + (a >> 1), c.hid.map + (a >> 1), n >> 1);
    }
  }
  c.q.clear();
  c.nFlush++;
}

}  // namespace kestrel::gpurdp
