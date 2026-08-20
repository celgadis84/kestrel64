# parallel_rdp.cmake — build Themaister's paraLLEl-RDP as a static lib for kestrel.
#
# Mirrors ares' config.mk source/include list. The tree is VENDORED under
# third_party/parallel-rdp (upstream commit in COMMIT) because kestrel patches the
# shaders: parallel-rdp assumes ares' word-swapped RDRAM layout and kestrel keeps
# guest big-endian byte order (see docs/parallel-rdp-integration.md). Shaders are
# baked into shaders/slangmosh.hpp (SPIR-V embedded) by tools/slangmosh_lite.py, so
# NO shader compiler is needed at build time — we only compile C++ + volk.c. volk loads vulkan-1.dll at runtime,
# so we do not link the Vulkan loader here.
#
# Off by default: the deterministic core (systemtest / lockstep md5) must never depend
# on this. Enable with -DKESTREL_PRDP=ON.

if(NOT DEFINED PRDP_DIR)
  set(PRDP_DIR "${CMAKE_SOURCE_DIR}/third_party/parallel-rdp"
      CACHE PATH "Path to the paraLLEl-RDP source tree (vendored under third_party/)")
endif()

# PRDP_DIR is the outer tree: it holds parallel-rdp/ (core), util/, volk/, vulkan/,
# vulkan-headers/ side by side. (Do NOT descend into the inner parallel-rdp/ here.)
set(PR "${PRDP_DIR}")

if(NOT EXISTS "${PR}/parallel-rdp/rdp_device.hpp")
  message(FATAL_ERROR "parallel-rdp not found at ${PR} — set -DPRDP_DIR=...")
endif()

# Core RDP + video interface (all .cpp in parallel-rdp/).
file(GLOB PRDP_CORE_CXX CONFIGURE_DEPENDS "${PR}/parallel-rdp/*.cpp")

# Granite Vulkan backend — explicit subset used by the RDP command processor.
set(PRDP_VK_CXX
  "${PR}/vulkan/buffer.cpp"
  "${PR}/vulkan/buffer_pool.cpp"
  "${PR}/vulkan/command_buffer.cpp"
  "${PR}/vulkan/command_pool.cpp"
  "${PR}/vulkan/context.cpp"
  "${PR}/vulkan/cookie.cpp"
  "${PR}/vulkan/descriptor_set.cpp"
  "${PR}/vulkan/device.cpp"
  "${PR}/vulkan/event_manager.cpp"
  "${PR}/vulkan/fence.cpp"
  "${PR}/vulkan/fence_manager.cpp"
  "${PR}/vulkan/image.cpp"
  "${PR}/vulkan/indirect_layout.cpp"
  "${PR}/vulkan/memory_allocator.cpp"
  "${PR}/vulkan/pipeline_event.cpp"
  "${PR}/vulkan/query_pool.cpp"
  "${PR}/vulkan/render_pass.cpp"
  "${PR}/vulkan/sampler.cpp"
  "${PR}/vulkan/semaphore.cpp"
  "${PR}/vulkan/semaphore_manager.cpp"
  "${PR}/vulkan/shader.cpp"
  "${PR}/vulkan/texture/texture_format.cpp"
)

set(PRDP_UTIL_CXX
  "${PR}/util/arena_allocator.cpp"
  "${PR}/util/logging.cpp"
  "${PR}/util/thread_id.cpp"
  "${PR}/util/aligned_alloc.cpp"
  "${PR}/util/timer.cpp"
  "${PR}/util/timeline_trace_file.cpp"
  "${PR}/util/environment.cpp"
  "${PR}/util/thread_name.cpp"
)

add_library(parallel_rdp STATIC
  ${PRDP_CORE_CXX}
  ${PRDP_VK_CXX}
  ${PRDP_UTIL_CXX}
  "${PR}/volk/volk.c"
)

target_include_directories(parallel_rdp PUBLIC
  "${PR}/parallel-rdp"
  "${PR}/volk"
  "${PR}/vulkan"
  "${PR}/vulkan-headers/include"
  "${PR}/util"
)

# NOTE: do NOT define GRANITE_VULKAN_MT. It arms Granite's task/thread manager, which then
# asserts "Thread does not exist in thread manager" unless every caller thread is registered
# with Granite. ares builds parallel-rdp WITHOUT it and drives the CommandProcessor from a
# single thread — we do the same (our RDP worker owns it), so single-threaded Granite is
# correct and avoids the thread-registration crash. The GPU compute still runs async.
if(WIN32)
  target_compile_definitions(parallel_rdp PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()

# Granite is not warning-clean; silence to keep kestrel's -Wall output readable.
if(NOT MSVC)
  target_compile_options(parallel_rdp PRIVATE -w)
endif()

set_target_properties(parallel_rdp PROPERTIES
  CXX_STANDARD 17            # Granite targets C++17; kestrel core is C++20
  CXX_STANDARD_REQUIRED ON
)

if(WIN32)
  target_link_libraries(parallel_rdp PUBLIC winmm)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(parallel_rdp PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
