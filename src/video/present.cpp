#include "present.hpp"
#include "../core/memory.hpp"
#include "../vrdp/vrdp.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

#ifdef KESTREL_PRDP
// paraLLEl-RDP's volk *defines* the vk* symbols as function POINTER VARIABLES. present.cpp
// must see the matching declarations (VK_NO_PROTOTYPES → pointers, from volk.h), or a call
// like vkCreateInstance(...) would jump to the pointer's storage address and execute its
// bytes (DEP fault). volk.h pulls in the Vulkan headers, so GLFW picks up the VK types too.
#define VK_NO_PROTOTYPES
#include <volk.h>
#include <GLFW/glfw3.h>
#else
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#endif

namespace kestrel {

// N64 VI output is nominally 320x240; VI_WIDTH gives the real line stride.
static constexpr u32 kSrcW = 320;
static constexpr u32 kSrcH = 240;
static constexpr int kScale = 2;   // window = 640x480

// All Vulkan state for the presenter lives here; torn down in reverse order.
// Named (not anonymous) so present.hpp can hold an opaque Vk* pimpl.
struct Vk {
  GLFWwindow* win = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  u32 qfamily = 0;
  VkQueue queue = VK_NULL_HANDLE;

  VkSwapchainKHR swap = VK_NULL_HANDLE;
  VkFormat swapFormat = VK_FORMAT_UNDEFINED;
  VkExtent2D extent = {};
  bool needRecreate = false;   // el present pidio rehacer la cadena
  std::vector<VkImage> swapImages;

  // Host-visible source image holding the current N64 frame (R8G8B8A8).
  VkImage srcImage = VK_NULL_HANDLE;
  VkDeviceMemory srcMem = VK_NULL_HANDLE;
  void* srcMapped = nullptr;
  VkDeviceSize srcRowPitch = 0;
  u32 srcW = kSrcW, srcH = kSrcH;

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkSemaphore semAcquire = VK_NULL_HANDLE, semRender = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  bool shared = false;        // instancia/dispositivo prestados por parallel-rdp: no destruir
  bool presentable = false;   // false → surface unusable (e.g. no desktop session); compose only
};

namespace {

auto findMemType(VkPhysicalDevice pd, u32 typeBits, VkMemoryPropertyFlags want) -> u32 {
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for(u32 i = 0; i < mp.memoryTypeCount; i++)
    if((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return ~0u;
}

auto barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
             VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) -> void {
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  b.srcAccessMask = srcA; b.dstAccessMask = dstA;
  vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

auto createSwapchain(Vk& v, bool quiet = false) -> bool {
  VkSurfaceCapabilitiesKHR caps;
  VkResult rc = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v.phys, v.surface, &caps);
  if(!quiet)
    std::fprintf(stderr, "[video] surfaceCaps rc=%d minImg=%u maxImg=%u curExt=%ux%u\n",
               rc, caps.minImageCount, caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height);
  // A broken surface (VK_ERROR_UNKNOWN etc.) yields garbage caps; a swapchain
  // built on it crashes the driver in vkAcquireNextImageKHR. Bail to compose-only.
  if(rc != VK_SUCCESS) { std::fprintf(stderr, "[video] surface unusable; compose-only (no window present)\n"); return false; }

  u32 nfmt = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(v.phys, v.surface, &nfmt, nullptr);
  std::vector<VkSurfaceFormatKHR> fmts(nfmt);
  vkGetPhysicalDeviceSurfaceFormatsKHR(v.phys, v.surface, &nfmt, fmts.data());
  if(!quiet) std::fprintf(stderr, "[video] surfaceFormats=%u\n", nfmt);
  if(nfmt == 0) return false;
  VkSurfaceFormatKHR pick = fmts[0];
  for(auto& f : fmts) if(f.format == VK_FORMAT_B8G8R8A8_UNORM) { pick = f; break; }
  v.swapFormat = pick.format;

  int fbw, fbh; glfwGetFramebufferSize(v.win, &fbw, &fbh);
  v.extent = caps.currentExtent.width != 0xffffffffu ? caps.currentExtent
             : VkExtent2D{ (u32)fbw, (u32)fbh };
  if(v.extent.width == 0 || v.extent.height == 0) v.extent = { kSrcW * kScale, kSrcH * kScale };

  u32 imgCount = caps.minImageCount + 1;
  if(caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

  VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
  ci.surface = v.surface;
  ci.minImageCount = imgCount;
  ci.imageFormat = pick.format;
  ci.imageColorSpace = pick.colorSpace;
  ci.imageExtent = v.extent;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync, always supported
  ci.clipped = VK_TRUE;
  VkResult sc = vkCreateSwapchainKHR(v.dev, &ci, nullptr, &v.swap);
  if(!quiet)
    std::fprintf(stderr, "[video] createSwapchain rc=%d ext=%ux%u fmt=%d imgCount=%u\n",
               sc, v.extent.width, v.extent.height, pick.format, imgCount);
  if(sc != VK_SUCCESS) return false;

  u32 n = 0; vkGetSwapchainImagesKHR(v.dev, v.swap, &n, nullptr);
  v.swapImages.resize(n);
  vkGetSwapchainImagesKHR(v.dev, v.swap, &n, v.swapImages.data());
  return true;
}

// Rehacer la cadena de intercambio con el tamano actual de la ventana.
//
// El blit de presentacion escala del framebuffer del guest al `extent` DE LA CADENA, y ese
// extent se fija cuando la cadena se crea. Una ventana redimensionada (o puesta a pantalla
// completa) seguia presentando contra el tamano viejo: lo que se veia era el compositor
// estirando o recortando una imagen del tamano equivocado. Hay que rehacerla cuando el
// tamano cambia y cuando el driver dice OUT_OF_DATE.
//
// vkDeviceWaitIdle primero: las imagenes viejas pueden seguir en vuelo en la cola.
auto recreateSwapchain(Vk& v) -> bool {
  vkDeviceWaitIdle(v.dev);
  VkSwapchainKHR old = v.swap;
  v.swap = VK_NULL_HANDLE;
  v.swapImages.clear();
  bool ok = createSwapchain(v, /*quiet=*/true);
  if(old != VK_NULL_HANDLE) vkDestroySwapchainKHR(v.dev, old, nullptr);
  // Una linea por recreacion, no por frame: solo ocurre al redimensionar o cuando el driver
  // marca la cadena obsoleta, y es justo lo que hace falta ver si la ventana sale mal.
  std::fprintf(stderr, "[video] swapchain recreado %ux%u ok=%d\n", v.extent.width, v.extent.height, (int)ok);
  return ok;
}

// La cola grafica puede ser la de parallel-rdp (ver initVulkan). vkQueueSubmit y
// vkQueuePresentKHR sobre una misma cola no son seguros entre hilos, y Granite submite desde
// el hilo del RDP mientras esto corre en el de la ventana: sin el candado el driver pierde el
// dispositivo. Con contexto propio no hay contienda y el guarda no hace nada.
struct QueueGuard {
  bool on;
  explicit QueueGuard(bool shared) : on(shared) { if(on) vrdp::queueLock(); }
  ~QueueGuard() { if(on) vrdp::queueUnlock(); }
};

auto initVulkan(Vk& v) -> bool {
  // Con parallel-rdp activo hay que COMPARTIR su contexto Vulkan en vez de crear uno propio:
  // volk resuelve todos los vk* en UNA tabla global de punteros, asi que el segundo contexto
  // que se carga pisa las entradas del primero y la siguiente llamada del otro salta a un
  // puntero nulo. La caida al abrir la ventana con PRDP era exactamente eso (host RIP 0).
  // De paso ahorra un dispositivo logico entero en la GPU.
  const vrdp::SharedVk* sh = vrdp::sharedVk();
  if(sh) {
    v.shared   = true;
    v.instance = (VkInstance)sh->instance;
    v.phys     = (VkPhysicalDevice)sh->gpu;
    v.dev      = (VkDevice)sh->device;
    v.qfamily  = sh->queueFamily;
    v.queue    = (VkQueue)sh->queue;
  }
#ifdef KESTREL_PRDP
  if(!v.shared && volkInitialize() != VK_SUCCESS) {
    std::fprintf(stderr, "[video] volkInitialize failed\n"); return false;
  }
#endif
  // Create the instance BEFORE glfwInit: on this box glfwInit() poisons the AMD
  // driver so vkCreateInstance hangs. Hardcode the surface extensions GLFW needs.
  if(!v.shared) {
    const char* ext[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "kestrel64"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = ext;
    if(vkCreateInstance(&ici, nullptr, &v.instance) != VK_SUCCESS) {
      std::fprintf(stderr, "[video] vkCreateInstance failed\n"); return false;
    }
#ifdef KESTREL_PRDP
    volkLoadInstance(v.instance);   // populate instance-level entry points
#endif
  }

  if(!glfwInit()) { std::fprintf(stderr, "[video] glfwInit failed\n"); return false; }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  // Tamano de ventana y pantalla completa. El framebuffer del guest sigue siendo
  // kSrcW x kSrcH: esto solo decide a que resolucion se PRESENTA (el blit de la cadena de
  // intercambio escala). KESTREL_WINSIZE=WxH manda; si no, KESTREL_WINSCALE=N da Nx sobre
  // 320x240. KESTREL_FULLSCREEN=1 usa el modo actual del monitor primario.
  u32 winW = kSrcW * kScale, winH = kSrcH * kScale;
  if(const char* sc = std::getenv("KESTREL_WINSCALE")) {
    u32 n = (u32)std::strtoul(sc, nullptr, 10);
    if(n >= 1 && n <= 16) { winW = kSrcW * n; winH = kSrcH * n; }
  }
  if(const char* ws = std::getenv("KESTREL_WINSIZE")) {
    unsigned w = 0, h = 0; char x = 0;
    if(std::sscanf(ws, "%u%c%u", &w, &x, &h) == 3 && (x == 'x' || x == 'X') && w >= 64 && h >= 64) {
      winW = (u32)w; winH = (u32)h;
    }
  }
  GLFWmonitor* mon = nullptr;
  if(const char* fs = std::getenv("KESTREL_FULLSCREEN")) if(fs[0] != '0') {
    mon = glfwGetPrimaryMonitor();
    if(mon) {
      // Sin cambiar el modo del monitor: se toma el que ya hay. Cambiarlo de verdad deja la
      // sesion del escritorio rota si el emulador se cae, y no compra nada porque la imagen
      // se escala igual en el blit de presentacion.
      if(const GLFWvidmode* vm = glfwGetVideoMode(mon)) {
        winW = (u32)vm->width; winH = (u32)vm->height;
        glfwWindowHint(GLFW_RED_BITS, vm->redBits);
        glfwWindowHint(GLFW_GREEN_BITS, vm->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS, vm->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, vm->refreshRate);
      }
    }
  }
  glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
  std::fprintf(stderr, "[video] ventana %ux%u%s\n", winW, winH, mon ? " (pantalla completa)" : "");
  v.win = glfwCreateWindow((int)winW, (int)winH, "kestrel64", mon, nullptr);
  if(!v.win) { std::fprintf(stderr, "[video] glfwCreateWindow failed\n"); return false; }

  if(glfwCreateWindowSurface(v.instance, v.win, nullptr, &v.surface) != VK_SUCCESS) {
    std::fprintf(stderr, "[video] surface creation failed\n"); return false;
  }
  // Give the window manager a beat to map/composite the window before we query
  // surface caps — some drivers return VK_ERROR_UNKNOWN on an un-composited surface.
  glfwShowWindow(v.win);
  for(int i = 0; i < 20; i++) { glfwPollEvents(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

  if(v.shared) {
    // El dispositivo ya lo eligio parallel-rdp; aqui solo hay que verificar que su cola
    // grafica sepa presentar en esta superficie. Si no puede, se queda en compose-only.
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(v.phys, v.qfamily, v.surface, &present);
    if(!present) {
      std::fprintf(stderr, "[video] shared queue cannot present here; compose-only\n");
      return true;
    }
  } else {
    u32 nphys = 0; vkEnumeratePhysicalDevices(v.instance, &nphys, nullptr);
    if(!nphys) { std::fprintf(stderr, "[video] no Vulkan devices\n"); return false; }
    std::vector<VkPhysicalDevice> devs(nphys);
    vkEnumeratePhysicalDevices(v.instance, &nphys, devs.data());
    // Pick a device with a graphics+present queue family, preferring discrete GPUs.
    int bestScore = -1;
    for(auto pd : devs) {
      u32 nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
      std::vector<VkQueueFamilyProperties> qs(nq);
      vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs.data());
      for(u32 i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, v.surface, &present);
        if((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
          VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
          int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2 : 1;
          if(score > bestScore) { bestScore = score; v.phys = pd; v.qfamily = i; }
          break;
        }
      }
    }
    if(v.phys == VK_NULL_HANDLE) { std::fprintf(stderr, "[video] no graphics+present queue\n"); return false; }
  }
  { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(v.phys, &p);
    std::fprintf(stderr, "[video] Vulkan device: %s\n", p.deviceName); }

  if(!v.shared) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = v.qfamily; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* devExt[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = devExt;
    if(vkCreateDevice(v.phys, &dci, nullptr, &v.dev) != VK_SUCCESS) {
      std::fprintf(stderr, "[video] vkCreateDevice failed\n"); return false;
    }
#ifdef KESTREL_PRDP
    volkLoadDevice(v.dev);   // populate device-level entry points (swapchain etc.)
#endif
    vkGetDeviceQueue(v.dev, v.qfamily, 0, &v.queue);
  }

  // Swapchain may fail if there's no usable desktop surface (headless launch).
  // Keep the window+compose path alive (presentable stays false) instead of
  // aborting — dumps still work and a real session gets full presentation.
  v.presentable = createSwapchain(v);
  if(!v.presentable) return true;   // compose-only; no per-frame present

  // Host-visible linear source image we write the N64 frame into and blit from.
  VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8G8B8A8_UNORM;
  ii.extent = { v.srcW, v.srcH, 1 };
  ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_LINEAR;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if(vkCreateImage(v.dev, &ii, nullptr, &v.srcImage) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(v.dev, v.srcImage, &mr);
  u32 mt = findMemType(v.phys, mr.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if(mt == ~0u) { std::fprintf(stderr, "[video] no host-visible memory\n"); return false; }
  VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
  if(vkAllocateMemory(v.dev, &ai, nullptr, &v.srcMem) != VK_SUCCESS) return false;
  vkBindImageMemory(v.dev, v.srcImage, v.srcMem, 0);
  vkMapMemory(v.dev, v.srcMem, 0, mr.size, 0, &v.srcMapped);
  VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
  VkSubresourceLayout sl; vkGetImageSubresourceLayout(v.dev, v.srcImage, &sub, &sl);
  v.srcRowPitch = sl.rowPitch;

  VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = v.qfamily;
  vkCreateCommandPool(v.dev, &pci, nullptr, &v.pool);
  VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
  cbi.commandPool = v.pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
  vkAllocateCommandBuffers(v.dev, &cbi, &v.cmd);
  VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
  vkCreateSemaphore(v.dev, &sci, nullptr, &v.semAcquire);
  vkCreateSemaphore(v.dev, &sci, nullptr, &v.semRender);
  VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
  vkCreateFence(v.dev, &fci, nullptr, &v.fence);

  // Move the source image into GENERAL once; it stays there (host writes + blit src).
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(v.cmd, &bi);
  barrier(v.cmd, v.srcImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          0, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_HOST_BIT);
  vkEndCommandBuffer(v.cmd);
  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  si.commandBufferCount = 1; si.pCommandBuffers = &v.cmd;
  {
    QueueGuard qg(v.shared);
    vkQueueSubmit(v.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(v.queue);
  }
  return true;
}


auto presentFrame(Vk& v, const u32* px, u32 w, u32 h) -> void {
  // Upload the frame into the mapped source image, honoring its row pitch.
  for(u32 y = 0; y < h && y < v.srcH; y++) {
    u8* dst = (u8*)v.srcMapped + y * v.srcRowPitch;
    std::memcpy(dst, px + y * w, std::min(w, v.srcW) * 4);
  }

  // La ventana pudo cambiar de tamano desde el frame anterior. El extent de la cadena es
  // el destino del blit, asi que si no se rehace la imagen sale del tamano equivocado.
  {
    int fbw = 0, fbh = 0;
    if(v.win) glfwGetFramebufferSize(v.win, &fbw, &fbh);
    if(fbw == 0 || fbh == 0) return;                       // minimizada: nada que presentar
    if((u32)fbw != v.extent.width || (u32)fbh != v.extent.height)
      if(!recreateSwapchain(v)) return;
  }

  u32 idx = 0;
  VkResult acq = vkAcquireNextImageKHR(v.dev, v.swap, UINT64_MAX, v.semAcquire, VK_NULL_HANDLE, &idx);
  if(acq == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(v); return; }   // rehacer y saltar el frame
  // Skip the frame on ANY non-success (resize, surface lost, headless caps). We
  // must not submit waiting on an unsignaled acquire semaphore, nor index a
  // stale swapchain — either would crash instead of gracefully dropping a frame.
  if(acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) return;
  if(idx >= v.swapImages.size()) return;

  vkResetCommandBuffer(v.cmd, 0);
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(v.cmd, &bi);

  VkImage swap = v.swapImages[idx];
  barrier(v.cmd, swap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

  // Destino centrado que conserva la relacion de aspecto del guest. Estirar a la ventana
  // entera deforma la imagen en cuanto la ventana deja de ser 4:3 (maximizar, pantalla
  // completa en un monitor ancho), que es lo que hacia antes.
  const u32 ew = v.extent.width, eh = v.extent.height;
  u32 dw = ew, dh = (u32)((u64)ew * v.srcH / v.srcW);
  if(dh > eh) { dh = eh; dw = (u32)((u64)eh * v.srcW / v.srcH); }
  const s32 dx = (s32)(ew - dw) / 2, dy = (s32)(eh - dh) / 2;

  // Las bandas laterales quedarian con basura del frame anterior: el blit solo cubre el
  // rectangulo util y la imagen del swapchain entra en UNDEFINED.
  if(dw != ew || dh != eh) {
    VkClearColorValue black = {};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(v.cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
  }

  VkImageBlit blit = {};
  blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  blit.srcOffsets[1] = { (s32)v.srcW, (s32)v.srcH, 1 };
  blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  blit.dstOffsets[0] = { dx, dy, 0 };
  blit.dstOffsets[1] = { dx + (s32)dw, dy + (s32)dh, 1 };
  vkCmdBlitImage(v.cmd, v.srcImage, VK_IMAGE_LAYOUT_GENERAL, swap,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

  barrier(v.cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  vkEndCommandBuffer(v.cmd);

  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  si.waitSemaphoreCount = 1; si.pWaitSemaphores = &v.semAcquire; si.pWaitDstStageMask = &waitStage;
  si.commandBufferCount = 1; si.pCommandBuffers = &v.cmd;
  si.signalSemaphoreCount = 1; si.pSignalSemaphores = &v.semRender;
  vkResetFences(v.dev, 1, &v.fence);
  {
    QueueGuard qg(v.shared);
    vkQueueSubmit(v.queue, 1, &si, v.fence);

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &v.semRender;
    pi.swapchainCount = 1; pi.pSwapchains = &v.swap; pi.pImageIndices = &idx;
    VkResult pr = vkQueuePresentKHR(v.queue, &pi);
    if(pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) v.needRecreate = true;
  }

  vkWaitForFences(v.dev, 1, &v.fence, VK_TRUE, UINT64_MAX);   // simple: one frame in flight

  // El present dijo que la cadena ya no vale (redimension a mitad de frame): se rehace aqui,
  // fuera del candado de cola y con el frame ya terminado.
  if(v.needRecreate) { v.needRecreate = false; recreateSwapchain(v); }
}

auto destroyVulkan(Vk& v) -> void {
  if(v.dev) vkDeviceWaitIdle(v.dev);
  if(v.fence) vkDestroyFence(v.dev, v.fence, nullptr);
  if(v.semRender) vkDestroySemaphore(v.dev, v.semRender, nullptr);
  if(v.semAcquire) vkDestroySemaphore(v.dev, v.semAcquire, nullptr);
  if(v.pool) vkDestroyCommandPool(v.dev, v.pool, nullptr);
  if(v.srcMapped) vkUnmapMemory(v.dev, v.srcMem);
  if(v.srcImage) vkDestroyImage(v.dev, v.srcImage, nullptr);
  if(v.srcMem) vkFreeMemory(v.dev, v.srcMem, nullptr);
  if(v.swap) vkDestroySwapchainKHR(v.dev, v.swap, nullptr);
  // El dispositivo y la instancia prestados son de parallel-rdp: los destruye su Context.
  // La superficie si es nuestra aunque la instancia no lo sea.
  if(v.dev && !v.shared) vkDestroyDevice(v.dev, nullptr);
  if(v.surface) vkDestroySurfaceKHR(v.instance, v.surface, nullptr);
  if(v.instance && !v.shared) vkDestroyInstance(v.instance, nullptr);
  if(v.win) glfwDestroyWindow(v.win);
  glfwTerminate();
}

// Write a 24-bit BMP from an R8G8B8A8 frame — headless verification of the pixels.
auto dumpBmp(const char* path, u32 w, u32 h, const u32* px) -> void {
  u32 rowBytes = (w * 3 + 3) & ~3u, imgSize = rowBytes * h, fileSize = 54 + imgSize;
  std::vector<u8> f(fileSize, 0);
  auto put16 = [&](u32 o, u16 val){ f[o]=val; f[o+1]=val>>8; };
  auto put32 = [&](u32 o, u32 val){ f[o]=val; f[o+1]=val>>8; f[o+2]=val>>16; f[o+3]=val>>24; };
  f[0]='B'; f[1]='M'; put32(2,fileSize); put32(10,54);
  put32(14,40); put32(18,w); put32(22,(u32)-(s32)h); put16(26,1); put16(28,24); put32(34,imgSize);
  for(u32 y = 0; y < h; y++) for(u32 x = 0; x < w; x++) {
    u32 c = px[y * w + x];                 // bytes R,G,B,A
    u32 o = 54 + y * rowBytes + x * 3;
    f[o] = (c >> 16) & 0xff; f[o+1] = (c >> 8) & 0xff; f[o+2] = c & 0xff;   // BMP wants B,G,R
  }
  if(FILE* fp = std::fopen(path, "wb")) { std::fwrite(f.data(), 1, f.size(), fp); std::fclose(fp);
    std::fprintf(stderr, "[video] dumped frame -> %s\n", path); }
}

// --- HUD: tiny 5x7 bitmap font for the speed-gauge footer --------------------
// Each glyph is 7 rows; the low 5 bits of each byte are the pixels (bit4=left).
// Only the characters the footer needs are defined; everything else prints blank.
struct Glyph { char c; u8 rows[7]; };
constexpr Glyph kFont[] = {
  {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
  {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
  {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
  {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
  {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
  {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
  {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
  {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
  {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
  {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
  {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}}, {'%',{0x18,0x19,0x02,0x04,0x08,0x13,0x03}},
  {':',{0x00,0x04,0x04,0x00,0x04,0x04,0x00}}, {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
};
auto glyphFor(char c) -> const u8* {
  for(auto& g : kFont) if(g.c == c) return g.rows;
  return nullptr;   // undefined char (incl. space) → blank advance
}

// Blit `s` into `frame` at (x0,y0), color `rgba`, 5x7 glyphs, 1px gap. Bounds-safe.
auto drawText(u32* frame, u32 w, u32 h, int x0, int y0, const char* s, u32 rgba) -> void {
  int x = x0;
  for(; *s; s++, x += 6) {
    const u8* g = glyphFor(*s);
    if(!g) continue;
    for(int ry = 0; ry < 7; ry++) {
      int py = y0 + ry; if(py < 0 || (u32)py >= h) continue;
      for(int rx = 0; rx < 5; rx++) {
        if(!(g[ry] & (0x10 >> rx))) continue;
        int px = x + rx; if(px < 0 || (u32)px >= w) continue;
        frame[(u32)py * w + (u32)px] = rgba;
      }
    }
  }
}

// Paint the speed-gauge footer over the bottom rows of the composed frame.
auto drawHud(u32* frame, u32 w, u32 h, double cpu, double rsp, double ram) -> void {
  if(h < 12 || w < 40) return;
  const u32 kBand = 10;                      // footer height in source pixels
  u32 y0 = h - kBand;
  for(u32 y = y0; y < h; y++)                // dark translucent-looking bar (solid)
    for(u32 x = 0; x < w; x++) frame[y * w + x] = 0xff141414u;
  auto clampi = [](double v) -> int { if(v < 0) return 0; if(v > 999) return 999; return (int)(v + 0.5); };
  char buf[64];
  std::snprintf(buf, sizeof buf, "CPU%d%% RSP%d%% RAM%d%%",
                clampi(cpu), clampi(rsp), clampi(ram));
  drawText(frame, w, h, 3, (int)y0 + 1, buf, 0xffffffffu);   // white on the bar
}


// ---------------------------------------------------------------- mapeo del mando
// El lanzador escribe un fichero de asignaciones y lo pasa en KESTREL_PAD1. Cada linea es
//   <CONTROL> <TECLA|-> <BOTON_GAMEPAD|->
// con los nombres de GLFW sin el prefijo (X, SPACE, LEFT, KP_0, A, DPAD_UP, ...). Sin
// fichero no se carga nada y el camino de teclado/gamepad es exactamente el de siempre:
// esto no puede cambiar el comportamiento de una ejecucion que no lo use.
struct PadMap {
  // Orden fijo: los 14 botones del joybus y las 4 direcciones del stick.
  static constexpr int kN = 18;
  int  key[kN];
  int  gpb[kN];
  bool loaded = false;
  PadMap() { for(int i = 0; i < kN; i++) { key[i] = -1; gpb[i] = -1; } }
};

struct NamedKey { const char* n; int v; };

static auto lookupKey(const char* n) -> int {
  static const NamedKey tbl[] = {
    {"SPACE", GLFW_KEY_SPACE}, {"APOSTROPHE", GLFW_KEY_APOSTROPHE}, {"COMMA", GLFW_KEY_COMMA},
    {"MINUS", GLFW_KEY_MINUS}, {"PERIOD", GLFW_KEY_PERIOD}, {"SLASH", GLFW_KEY_SLASH},
    {"SEMICOLON", GLFW_KEY_SEMICOLON}, {"EQUAL", GLFW_KEY_EQUAL},
    {"LEFT_BRACKET", GLFW_KEY_LEFT_BRACKET}, {"BACKSLASH", GLFW_KEY_BACKSLASH},
    {"RIGHT_BRACKET", GLFW_KEY_RIGHT_BRACKET}, {"GRAVE_ACCENT", GLFW_KEY_GRAVE_ACCENT},
    {"ESCAPE", GLFW_KEY_ESCAPE}, {"ENTER", GLFW_KEY_ENTER}, {"TAB", GLFW_KEY_TAB},
    {"BACKSPACE", GLFW_KEY_BACKSPACE}, {"INSERT", GLFW_KEY_INSERT}, {"DELETE", GLFW_KEY_DELETE},
    {"RIGHT", GLFW_KEY_RIGHT}, {"LEFT", GLFW_KEY_LEFT}, {"DOWN", GLFW_KEY_DOWN},
    {"UP", GLFW_KEY_UP}, {"PAGE_UP", GLFW_KEY_PAGE_UP}, {"PAGE_DOWN", GLFW_KEY_PAGE_DOWN},
    {"HOME", GLFW_KEY_HOME}, {"END", GLFW_KEY_END},
    {"LEFT_SHIFT", GLFW_KEY_LEFT_SHIFT}, {"LEFT_CONTROL", GLFW_KEY_LEFT_CONTROL},
    {"LEFT_ALT", GLFW_KEY_LEFT_ALT}, {"RIGHT_SHIFT", GLFW_KEY_RIGHT_SHIFT},
    {"RIGHT_CONTROL", GLFW_KEY_RIGHT_CONTROL}, {"RIGHT_ALT", GLFW_KEY_RIGHT_ALT},
    {"KP_ENTER", GLFW_KEY_KP_ENTER},
  };
  if(!n || !*n) return -1;
  usize len = std::strlen(n);
  if(len == 1) {
    char c = n[0];
    if(c >= 'A' && c <= 'Z') return GLFW_KEY_A + (c - 'A');
    if(c >= 'a' && c <= 'z') return GLFW_KEY_A + (c - 'a');
    if(c >= '0' && c <= '9') return GLFW_KEY_0 + (c - '0');
  }
  if(len == 4 && !std::strncmp(n, "KP_", 3) && n[3] >= '0' && n[3] <= '9')
    return GLFW_KEY_KP_0 + (n[3] - '0');
  if(n[0] == 'F' && len >= 2 && n[1] >= '1' && n[1] <= '9') {
    int f = std::atoi(n + 1);
    if(f >= 1 && f <= 25) return GLFW_KEY_F1 + (f - 1);
  }
  for(const auto& e : tbl) if(!std::strcmp(e.n, n)) return e.v;
  return -1;
}

static auto lookupGamepad(const char* n) -> int {
  static const NamedKey tbl[] = {
    {"A", GLFW_GAMEPAD_BUTTON_A}, {"B", GLFW_GAMEPAD_BUTTON_B},
    {"X", GLFW_GAMEPAD_BUTTON_X}, {"Y", GLFW_GAMEPAD_BUTTON_Y},
    {"LEFT_BUMPER", GLFW_GAMEPAD_BUTTON_LEFT_BUMPER},
    {"RIGHT_BUMPER", GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER},
    {"BACK", GLFW_GAMEPAD_BUTTON_BACK}, {"START", GLFW_GAMEPAD_BUTTON_START},
    {"GUIDE", GLFW_GAMEPAD_BUTTON_GUIDE},
    {"LEFT_THUMB", GLFW_GAMEPAD_BUTTON_LEFT_THUMB},
    {"RIGHT_THUMB", GLFW_GAMEPAD_BUTTON_RIGHT_THUMB},
    {"DPAD_UP", GLFW_GAMEPAD_BUTTON_DPAD_UP}, {"DPAD_RIGHT", GLFW_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"DPAD_DOWN", GLFW_GAMEPAD_BUTTON_DPAD_DOWN}, {"DPAD_LEFT", GLFW_GAMEPAD_BUTTON_DPAD_LEFT},
    // Los gatillos son ejes, no botones: se marcan con codigos negativos propios y el
    // lector los resuelve contra gp.axes.
    {"LEFT_TRIGGER", -2}, {"RIGHT_TRIGGER", -3},
  };
  if(!n || !*n) return -1;
  for(const auto& e : tbl) if(!std::strcmp(e.n, n)) return e.v;
  return -1;
}

// Indices del mapa. Los 14 primeros llevan su bit del joybus; los 4 ultimos son el stick.
static const char* kPadIds[PadMap::kN] = {
  "A", "B", "Z", "START", "DU", "DD", "DL", "DR", "L", "R", "CU", "CD", "CL", "CR",
  "SX+", "SX-", "SY+", "SY-",
};
static const u32 kPadBits[14] = {
  0x8000, 0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100,
  0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x0001,
};

static auto loadPadMap() -> const PadMap& {
  static PadMap m;
  static bool once = false;
  if(once) return m;
  once = true;
  const char* path = std::getenv("KESTREL_PAD1");
  if(!path || !*path) return m;
  std::FILE* f = std::fopen(path, "r");
  if(!f) { std::fprintf(stderr, "[input] no se pudo abrir %s\n", path); return m; }
  char line[256];
  int n = 0;
  while(std::fgets(line, sizeof line, f)) {
    char id[64] = {0}, k[64] = {0}, g[64] = {0};
    if(std::sscanf(line, "%63s %63s %63s", id, k, g) < 2) continue;
    for(int i = 0; i < PadMap::kN; i++) {
      if(std::strcmp(kPadIds[i], id)) continue;
      m.key[i] = lookupKey(std::strcmp(k, "-") ? k : nullptr);
      m.gpb[i] = lookupGamepad(std::strcmp(g, "-") ? g : nullptr);
      n++;
      break;
    }
  }
  std::fclose(f);
  m.loaded = n > 0;
  std::fprintf(stderr, "[input] mapa de mando: %d controles desde %s\n", n, path);
  return m;
}

}  // namespace

// Bring up Vulkan/GLFW on the calling (main) thread. MUST run before the CPU
// worker starts: on this box a CPU-bound sibling thread stalls the driver's
// instance/device bring-up indefinitely, so we serialize — init first, worker
// after. Steady-state presentation coexists with the busy worker fine.
auto Presenter::open() -> bool {
  std::setvbuf(stderr, nullptr, _IONBF, 0);   // unbuffered: never lose a diag on kill
  vk = new Vk();
  if(!initVulkan(*vk)) { destroyVulkan(*vk); delete vk; vk = nullptr; return false; }
  if(vk->win) glfwSetWindowTitle(vk->win, windowTitle.c_str());   // ROM name in the titlebar
  std::fprintf(stderr, "[video] Vulkan presenter up (%ux%u -> %ux%u)\n",
               vk->srcW, vk->srcH, vk->extent.width, vk->extent.height);
  return true;
}

// Compose one N64 frame from RDRAM and present it. Returns false when the window
// has been closed (caller should shut down). Rate-limited to ~60 Hz internally.
auto Presenter::pumpFrame() -> bool {
  Vk& v = *vk;
  if(glfwWindowShouldClose(v.win)) return false;
  glfwPollEvents();

  // --- player-1 keyboard → N64 pad -------------------------------------------
  // Buttons: X=A  C=B  Space=Z  Enter=Start  Q=L  E=R ; D-pad = arrows ;
  // C-buttons = I/J/K/L ; analog stick = W/A/S/D (full ±80 deflection).
  {
    auto down = [&](int key) { return key >= 0 && glfwGetKey(v.win, key) == GLFW_PRESS; };
    const PadMap& pm = loadPadMap();
    u32 b = 0;
    int sxm = 0, sym = 0;
    if(pm.loaded) {
      // Mapa del lanzador: sustituye por completo al teclado de fabrica. Debajo queda el
      // camino de siempre para el caso sin fichero, identico a como estaba.
      for(int i = 0; i < 14; i++) if(down(pm.key[i])) b |= kPadBits[i];
      sxm = (down(pm.key[14]) ? 80 : 0) - (down(pm.key[15]) ? 80 : 0);
      sym = (down(pm.key[16]) ? 80 : 0) - (down(pm.key[17]) ? 80 : 0);
    }
    if(!pm.loaded) {
    if(down(GLFW_KEY_X))     b |= 0x8000;   // A
    if(down(GLFW_KEY_C))     b |= 0x4000;   // B
    if(down(GLFW_KEY_SPACE)) b |= 0x2000;   // Z
    if(down(GLFW_KEY_ENTER)) b |= 0x1000;   // START
    if(down(GLFW_KEY_UP))    b |= 0x0800;   // D-Up
    if(down(GLFW_KEY_DOWN))  b |= 0x0400;   // D-Down
    if(down(GLFW_KEY_LEFT))  b |= 0x0200;   // D-Left
    if(down(GLFW_KEY_RIGHT)) b |= 0x0100;   // D-Right
    if(down(GLFW_KEY_Q))     b |= 0x0020;   // L
    if(down(GLFW_KEY_E))     b |= 0x0010;   // R
    if(down(GLFW_KEY_I))     b |= 0x0008;   // C-Up
    if(down(GLFW_KEY_K))     b |= 0x0004;   // C-Down
    if(down(GLFW_KEY_J))     b |= 0x0002;   // C-Left
    if(down(GLFW_KEY_L))     b |= 0x0001;   // C-Right
    }
    int sx = pm.loaded ? sxm : ((down(GLFW_KEY_D) ? 80 : 0) - (down(GLFW_KEY_A) ? 80 : 0));
    int sy = pm.loaded ? sym : ((down(GLFW_KEY_W) ? 80 : 0) - (down(GLFW_KEY_S) ? 80 : 0));

    // --- optional physical gamepad (player 1), OR'd on top of the keyboard ------
    // GLFW's gamepad mapping DB gives every pad the same button/axis layout, so
    // this is controller-agnostic. Xbox-style default: A=A, B=B, X=Z, Y=R, LB=L,
    // Back=Start(also Start=Start), left stick=analog, right stick=C-buttons,
    // triggers=Z/R. Keyboard stays live so either input source works.
    GLFWgamepadstate gp;
    if(glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &gp)) {
      auto bt = [&](int i) { return i >= 0 && gp.buttons[i] == GLFW_PRESS; };
      // Un gatillo no es un boton: los codigos -2/-3 del mapa se resuelven contra los ejes.
      auto btm = [&](int i) {
        if(i == -2) return gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]  > 0.0f;
        if(i == -3) return gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.0f;
        return bt(i);
      };
      if(pm.loaded) for(int i = 0; i < 14; i++) if(btm(pm.gpb[i])) b |= kPadBits[i];
      if(!pm.loaded) {
      if(bt(GLFW_GAMEPAD_BUTTON_A))            b |= 0x8000;  // A
      if(bt(GLFW_GAMEPAD_BUTTON_B))            b |= 0x4000;  // B
      if(bt(GLFW_GAMEPAD_BUTTON_X))            b |= 0x2000;  // Z
      if(bt(GLFW_GAMEPAD_BUTTON_START))        b |= 0x1000;  // START
      if(bt(GLFW_GAMEPAD_BUTTON_BACK))         b |= 0x1000;  // START (alt)
      if(bt(GLFW_GAMEPAD_BUTTON_DPAD_UP))      b |= 0x0800;
      if(bt(GLFW_GAMEPAD_BUTTON_DPAD_DOWN))    b |= 0x0400;
      if(bt(GLFW_GAMEPAD_BUTTON_DPAD_LEFT))    b |= 0x0200;
      if(bt(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT))   b |= 0x0100;
      if(bt(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER))  b |= 0x0020;  // L
      if(bt(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER)) b |= 0x0010;  // R
      if(bt(GLFW_GAMEPAD_BUTTON_Y))            b |= 0x0010;  // R (alt)
      // Triggers (analog) → Z / R when pressed past half.
      if(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]  > 0.0f) b |= 0x2000;  // Z
      if(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.0f) b |= 0x0010;  // R
      // Right stick → C-buttons (digital past a deadzone).
      float rx = gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_X], ry = gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
      if(ry < -0.5f) b |= 0x0008;  // C-Up
      if(ry >  0.5f) b |= 0x0004;  // C-Down
      if(rx < -0.5f) b |= 0x0002;  // C-Left
      if(rx >  0.5f) b |= 0x0001;  // C-Right
      }
      // Left stick → analog. Deadzone, then scale to the N64's ±80 range.
      float lx = gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X], ly = gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
      if(lx < -0.2f || lx > 0.2f) sx = (int)(lx * 80.0f);
      if(ly < -0.2f || ly > 0.2f) sy = (int)(-ly * 80.0f);  // GLFW +y is down
    }

    mem->padButtons = b;
    if(sx > 80) sx = 80; else if(sx < -80) sx = -80;
    if(sy > 80) sy = 80; else if(sy < -80) sy = -80;
    mem->padStickX = (s8)sx;
    mem->padStickY = (s8)sy;
  }

  static auto lastTick = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if(now - lastTick < std::chrono::milliseconds(16)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return true;
  }
  lastTick = now;

  auto expand5 = [](u32 val) -> u32 { return (val << 3) | (val >> 2); };

  // Snapshot VI state. Reads race the CPU writing RDRAM; that only tears the
  // picture, never crashes (bounds-checked), so no lock.
  u32 origin = mem->rcp.vi_origin & 0x1fff'ffff;
  u32 width  = mem->rcp.vi_width ? mem->rcp.vi_width : kSrcW;
  u32 type   = mem->rcp.vi_ctrl & 3;   // 0 blank, 2 = 16bpp, 3 = 32bpp
  if(width > kSrcW) width = kSrcW;      // srcImage is kSrcW wide
  u32 height = kSrcH;

  frame.assign((usize)width * height, 0xff000000u);   // bytes 0,0,0,255 → black
  const auto& ram = mem->rdram;

  // GPU RDP path: pull the scanned-out image straight from paraLLEl-RDP (VRAM),
  // bypassing the RDRAM framebuffer read. Its RGBA8888 [R,G,B,A] byte order matches
  // `frame`'s little-endian ABGR word, so it copies row-for-row. No-op when PRDP off.
  bool vrdpFrame = false;
  if(vrdp::active()) {
    u32 sw = 0, sh = 0;
    const u8* rgba = vrdp::scanout(sw, sh);
    if(rgba && sw && sh) {
      width = sw > kSrcW ? kSrcW : sw;
      height = sh;
      frame.assign((usize)width * height, 0xff000000u);
      for(u32 y = 0; y < height; y++)
        std::memcpy(&frame[y * width], rgba + (usize)y * sw * 4, (usize)width * 4);
      vrdpFrame = true;
    }
    vrdp::scanoutDone();
  }

  if(!vrdpFrame && std::getenv("KESTREL_VIDEO_TEST")) {
    for(u32 y = 0; y < height; y++) for(u32 x = 0; x < width; x++) {
      u32 R = x * 255 / width, G = y * 255 / height, B = 128;
      if((x % 32) == 0 || (y % 32) == 0) { R = G = B = 255; }
      frame[y * width + x] = 0xff000000u | (B << 16) | (G << 8) | R;   // R8G8B8A8
    }
    type = 99;
  }
  if(vrdpFrame) {
    // already have pixels from the GPU scanout
  } else if(type == 2) {  // RGBA5551, 2 bytes/pixel, big-endian
    for(u32 y = 0; y < height; y++) for(u32 x = 0; x < width; x++) {
      u32 p = origin + (y * width + x) * 2;
      if(p + 1 >= ram.size()) continue;
      u32 px = ((u32)ram[p] << 8) | ram[p + 1];
      u32 R = expand5((px >> 11) & 0x1f), G = expand5((px >> 6) & 0x1f), B = expand5((px >> 1) & 0x1f);
      frame[y * width + x] = 0xff000000u | (B << 16) | (G << 8) | R;
    }
  } else if(type == 3) {  // RGBA8888, big-endian bytes R,G,B,A
    for(u32 y = 0; y < height; y++) for(u32 x = 0; x < width; x++) {
      u32 p = origin + (y * width + x) * 4;
      if(p + 3 >= ram.size()) continue;
      frame[y * width + x] = 0xff000000u | ((u32)ram[p + 2] << 16) | ((u32)ram[p + 1] << 8) | ram[p];
    }
  }

  // Speed-gauge footer (CPU / RSP / RDRAM % of realtime). On unless disabled.
  if(!std::getenv("KESTREL_HUD_OFF")) {
    double c = cpuPct   ? cpuPct->load(std::memory_order_relaxed)   : 0.0;
    double r = rspPct   ? rspPct->load(std::memory_order_relaxed)   : 0.0;
    double m = rdramPct ? rdramPct->load(std::memory_order_relaxed) : 0.0;
    drawHud(frame.data(), width, height, c, r, m);
  }

  if(const char* path = std::getenv("KESTREL_VIDEO_DUMP")) {
    dumpBmp(path, width, height, frame.data());   // every frame; last one = latest state
  }
  if(v.presentable) presentFrame(v, frame.data(), width, height);
  return true;
}

auto Presenter::close() -> void {
  if(vk) { destroyVulkan(*vk); delete vk; vk = nullptr; }
}

auto Presenter::start(Memory* m, std::atomic<bool>* sd,
                      const std::atomic<double>* cpu, const std::atomic<double>* rsp,
                      const std::atomic<double>* ram, const char* title) -> bool {
  mem = m; shutdown = sd; cpuPct = cpu; rspPct = rsp; rdramPct = ram;
  if(title && *title) windowTitle = std::string("kestrel64 - ") + title;
  return true;   // open()/pumpFrame() run later on the main thread
}

}  // namespace kestrel
