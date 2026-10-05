# NVRHI (vendored)

NVIDIA Rendering Hardware Interface. This copy contains the **common sources, the validation layer and the Vulkan backend only**. It is built as one static library, premake project `NVRHI` (see `premake5.lua`).

| | |
|---|---|
| Upstream | https://github.com/NVIDIA-RTX/NVRHI |
| Version | No release tags upstream. Pinned to the `main` branch head, NVRHI header version `c_HeaderVersion = 27`, CMake project version 1.0.0 |
| Commit | `6b96fb03e07539f08327aea76c56d55f1de9d906` (committed 2026-10-05 01:49:30 -0700) |
| Date vendored | 2026-10-05 |
| License | MIT (`LICENSE.txt`). `ThirdPartyLicenses.txt` is upstream's Apache-2.0 notice for Vulkan-Headers |
| Language | C++17 (upstream `CMAKE_CXX_STANDARD 17`) |

## Bundled Vulkan-Headers

Upstream NVRHI no longer has a `thirdparty/Vulkan-Headers` git submodule (its `.gitmodules` is empty). Instead its CMake fetches Vulkan-Headers with `FetchContent` at `NVRHI_VULKAN_HEADERS_GIT_TAG = v1.4.352`. That same tag is vendored here.

| | |
|---|---|
| Upstream | https://github.com/KhronosGroup/Vulkan-Headers |
| Tag | `v1.4.352` (annotated tag object `eae540635060031f68af0fe1fecacc1264484a96`) |
| Commit | `015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9` (committed 2026-05-15) |
| `VK_HEADER_VERSION` | 352. Exposes `VK_API_VERSION_1_4`. NVRHI needs at least 318 (`#error` in `vulkan-backend.h`) |
| License | C headers (`vulkan_core.h`, `vk_video/*`, ...) are `Apache-2.0`. The C++ headers (`vulkan.hpp`, `vulkan_*.hpp`, `*.cppm`) are `Apache-2.0 OR MIT`. See `thirdparty/Vulkan-Headers/LICENSE.md` and `LICENSES/` |

Newer Vulkan-Headers tags exist (up to v1.4.365 when this was vendored). The tag NVRHI pins and tests against is used on purpose. It already provides every Vulkan 1.4 symbol.

## What was kept

```
LICENSE.txt, ThirdPartyLicenses.txt
include/nvrhi/nvrhi.h, nvrhiHLSL.h, utils.h, validation.h, vulkan.h
include/nvrhi/common/aftermath.h, containers.h, misc.h, resource.h, resourcebindingmap.h
src/common/aftermath.cpp, format-info.cpp, misc.cpp, state-tracking.cpp/.h, utils.cpp, versioning.h
src/validation/*            (validation layer)
src/vulkan/*                (Vulkan backend)
tools/nvrhi.natvis          (MSVC debugger visualizers, added to Windows builds like upstream)
thirdparty/Vulkan-Headers/include/**            (vulkan/, vk_video/; the *.cppm module files are kept but not compiled)
thirdparty/Vulkan-Headers/LICENSE.md, LICENSES/Apache-2.0.txt, LICENSES/MIT.txt
```

## What was removed

- D3D11/D3D12 backends: `include/nvrhi/d3d11.h`, `include/nvrhi/d3d12.h`, `src/d3d11/`, `src/d3d12/`, `src/common/dxgi-format.*`.
- Build system and packaging: `CMakeLists.txt`, `cmake/` (also the NVAPI, Aftermath and RTXMU fetch logic), `src/nvrhiConfig.cmake.in`.
- Tests, docs, CI and repo metadata: `tests/`, `doc/`, `README.md`, `CLA.txt`, `.github/`, `.gitignore`, `.gitmodules`.
- Vulkan-Headers: everything outside `include/` and the license files (`registry/`, `tests/`, CMake/GN build files, docs).

## Local modifications

**None.** Every file matches upstream byte for byte, apart from line endings. Files were normalised to LF to follow the repository's `.gitattributes` (`* text=auto eol=lf`). This was checked with `diff --strip-trailing-cr` against both upstream checkouts.

## premake project (mirrors upstream CMake)

The upstream configuration this mirrors is `NVRHI_WITH_VULKAN=ON`, `NVRHI_WITH_VALIDATION=ON`, `NVRHI_WITH_DX11/DX12/RTXMU/NVAPI/AFTERMATH=OFF`, `NVRHI_BUILD_SHARED=OFF`. One deviation: upstream builds `nvrhi` (common + validation) and `nvrhi_vk` as two static libraries. Here they are merged into one `NVRHI` static library.

Defines set by `premake5.lua`:

| Define | Platforms | Upstream visibility | Consumers must define it too? |
|---|---|---|---|
| `VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1` | all | hard-coded in `src/vulkan/vulkan-backend.h` | **yes** |
| `VK_USE_PLATFORM_WIN32_KHR` | Windows | PUBLIC | **yes** |
| `NOMINMAX` | Windows | PRIVATE | **yes** (see below) |
| `NVRHI_WITH_AFTERMATH=0` | all | PRIVATE | no |

`NVRHI_WITH_VULKAN` does nothing here. No NVRHI header or source tests it; it is only a CMake option name. It is not defined, and consumers do not need it.

Upstream defines no platform macros on Linux or macOS. Shared-resource export there uses `VK_KHR_external_memory_fd`, which is in `vulkan_core.h`.

`NDEBUG` is **deliberately not set** by this project. See "Hard consistency rules" below.

## Consumer requirements

### Include directories (relative to the repository root)

```
Vendor/NVRHI/include
Vendor/NVRHI/thirdparty/Vulkan-Headers/include
```

You can add these with premake `externalincludedirs` instead of `includedirs`. That keeps vendor headers out of the engine's warning level (`/external:I` on MSVC, `-isystem` on GCC/Clang). For reference, a C++23 consumer at `/W4` using plain `includedirs` built with zero warnings.

### Public defines

```lua
defines { "VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1" }
filter "system:windows"
    defines { "VK_USE_PLATFORM_WIN32_KHR", "NOMINMAX" }
filter {}
```

- On Windows, `VK_USE_PLATFORM_WIN32_KHR` makes `<vulkan/vulkan.h>` include `<windows.h>`. Any TU that includes `nvrhi/vulkan.h` or `vulkan.hpp` gets it. That is why `NOMINMAX` is required for consumers even though upstream only sets it privately. Without it, the `min`/`max` macros break `std::min`, `std::numeric_limits<T>::max()`, glm and so on.
- Do **not** define `VK_USE_PLATFORM_XLIB_KHR`, `_XCB_KHR`, `_WAYLAND_KHR` or `_METAL_EXT` (Linux/macOS). GLFW creates the surface (`glfwCreateWindowSurface`). The Xlib define would leak X11 macros (`None`, `Bool`, `Status`, `Success`) into every TU.
- Do **not** define `NVRHI_SHARED_LIBRARY_INCLUDE` or `NVRHI_SHARED_LIBRARY_BUILD`. This is a static library.

### Hard consistency rules (ODR)

1. **`NDEBUG` must be identical** for NVRHI and every consumer TU that includes `vulkan.hpp`. `vk::detail::DispatchLoaderBase` has extra members when `NDEBUG` is *not* defined. That changes the layout of the shared global `vk::detail::defaultDispatchLoaderDynamic`. If NVRHI and the TU that defines the dispatcher storage disagree, NVRHI reads function pointers at the wrong offsets and crashes. This was reproduced while vendoring: NVRHI built with `NDEBUG` and the app without it segfaulted at start-up. If `NDEBUG` is wanted (e.g. for Dist), set it at **workspace scope** in the root `premake5.lua` so every project inherits it. Never set it per project. That setup (`filter "configurations:Dist" defines { "NDEBUG" }` at workspace scope) was verified.
2. **Do not define any `VULKAN_HPP_*` configuration macro** apart from `VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1`. That includes `VULKAN_HPP_NO_EXCEPTIONS`, `VULKAN_HPP_NO_CONSTRUCTORS`, `VULKAN_HPP_NO_STRUCT_CONSTRUCTORS`, `VULKAN_HPP_NO_SETTERS`, `VULKAN_HPP_DISABLE_ENHANCED_MODE`, `VULKAN_HPP_NAMESPACE` and `VULKAN_HPP_NO_SMART_HANDLE`. NVRHI is compiled with vulkan.hpp's defaults and depends on constructors, setters, enhanced mode and exceptions. Changing one requires adding it to `premake5.lua` too.
3. Platform defines (`VK_USE_PLATFORM_*`, `VK_ENABLE_BETA_EXTENSIONS`) do not change the dispatcher layout, because vulkan.hpp uses placeholders. Still keep them identical to NVRHI for ODR hygiene.
4. NVRHI is compiled as C++17 and consumers as C++23. That is fine: the dispatcher layout does not depend on the language version. This was verified with a C++23 consumer.

C++ exceptions must stay enabled (`/EHsc`, the premake default). vulkan.hpp throws `vk::SystemError`, and `vk::detail::DynamicLoader` throws `std::runtime_error` if the loader is missing.

### Dynamic dispatcher (mandatory)

In static-library mode NVRHI neither defines nor initializes the vulkan.hpp default dispatcher. That only happens in upstream's `NVRHI_SHARED_LIBRARY_BUILD`, and `DeviceDesc::vulkanLibraryName` is ignored in this mode. The application must do both:

```cpp
// Exactly ONE .cpp file in the final executable (e.g. the engine's Vulkan device code):
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE   // defines vk::detail::defaultDispatchLoaderDynamic
```

```cpp
// Initialization order:
vk::detail::DynamicLoader loader;   // loads vulkan-1.dll / libvulkan.so.1 / libvulkan.1.dylib (or MoltenVK); keep alive until after vkDestroyInstance
auto getInstanceProcAddr = loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddr);   // global commands: enumerateInstanceVersion, createInstance, ...

uint32_t loaderVersion = vk::enumerateInstanceVersion();   // check >= VK_API_VERSION_1_3; request 1.4 when available
vk::Instance instance = vk::createInstance(instanceInfo);
VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);              // instance-level commands

vk::Device device = physicalDevice.createDevice(deviceInfo);
VULKAN_HPP_DEFAULT_DISPATCHER.init(device);                // device-level commands (core + KHR aliases resolved)

nvrhi::vulkan::DeviceDesc desc;   // instance, physicalDevice, device, queues + family indices,
                                  // enabled instance/device extension name lists, bufferDeviceAddressSupported, errorCB
nvrhi::DeviceHandle nvrhiDevice = nvrhi::vulkan::createDevice(desc);
nvrhiDevice = nvrhi::validation::createValidationLayer(nvrhiDevice);   // recommended for Debug/Release
```

- `vk::detail::DynamicLoader` is the correct type for this Vulkan-Headers version (VK_HEADER_VERSION 301+). The older `vk::DynamicLoader` is gone.
- GLFW (3.4+): call `glfwInitVulkanLoader(getInstanceProcAddr)` **before** `glfwInit()`. GLFW then uses the same loader instead of loading its own copy.
- Device features NVRHI's Vulkan backend relies on: Vulkan 1.3 `synchronization2` (`vkCmdPipelineBarrier2`) and `dynamicRendering` (`vkCmdBeginRendering`), plus Vulkan 1.2 `timelineSemaphore` (queue sync). Enable them through the `VkPhysicalDeviceVulkan12Features`/`13Features` chain. Presentation also needs `VK_KHR_swapchain`. Pass the enabled extension names in `DeviceDesc` so NVRHI can detect optional features (debug utils, ray tracing, mesh shaders, ...).
- macOS (MoltenVK/KosmicKrisp): enable the instance extension `VK_KHR_portability_enumeration` with `vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR`. Enable the device extension `"VK_KHR_portability_subset"` when the device advertises it. Use the string literal: its macro lives in `vulkan_beta.h`, so do not define `VK_ENABLE_BETA_EXTENSIONS` just for this.

### Link requirements

| Platform | Link | Runtime |
|---|---|---|
| Windows | `NVRHI` only. **Do not link `vulkan-1.lib`**: the loader is loaded at runtime with `LoadLibraryA("vulkan-1.dll")` | `vulkan-1.dll` (installed with the GPU driver) |
| Linux | `NVRHI`, `dl` (for `dlopen`; part of libc since glibc 2.34, harmless on Ubuntu 24.04) | `libvulkan.so.1` (package `libvulkan1`) |
| macOS | `NVRHI` (`dlopen` is in libSystem) | `libvulkan.1.dylib` + MoltenVK/KosmicKrisp ICD, found via the Vulkan SDK or bundled with the app |

## Verification (2026-10-05)

A throwaway premake workspace included this `premake5.lua` by absolute path and added a C++23 `/W4` console app linking `NVRHI`. It was built with VS 2026 (MSVC 14.51, v145) for x64 Debug, Release and Dist, with zero compiler warnings. The test, which passed in all three configurations:

- Checked `nvrhi::verifyHeaderVersion()`, `nvrhi::vulkan::convertFormat`, `nvrhi::getFormatInfo` and `nvrhi::utils::FormatToString`.
- Took the addresses of `nvrhi::vulkan::createDevice` and `nvrhi::validation::createValidationLayer`.
- Loaded the loader through `vk::detail::DynamicLoader` and printed `vk::enumerateInstanceVersion()` (1.4.341).
- Created a headless Vulkan 1.4 instance and device with `VK_LAYER_KHRONOS_validation` on an RTX 5070 Ti Laptop GPU.
- Wrapped the device with `nvrhi::vulkan::createDevice` and `nvrhi::validation::createValidationLayer`.
- Ran a `writeBuffer` → `copyBuffer` → readback round trip, verified byte for byte with zero NVRHI or Khronos validation errors.

premake `gmake` generation for `--os=linux` and `--os=macosx` also succeeded. Linux and macOS compilation could not be run on this machine. The file list and defines were cross-checked against upstream `CMakeLists.txt`, whose CI builds on Ubuntu.

## Updating

1. There are no tags. Get the new head with `git ls-remote https://github.com/NVIDIA-RTX/NVRHI HEAD`, then `git clone --depth 1 https://github.com/NVIDIA-RTX/NVRHI src` (or fetch that SHA).
2. Diff upstream `CMakeLists.txt` against the previous commit: the `include_common`, `src_common`, `include_validation`, `src_validation`, `include_vk` and `src_vk` lists, the `target_compile_definitions` for `nvrhi` and `nvrhi_vk`, and `NVRHI_VULKAN_HEADERS_GIT_TAG`.
3. Replace `include/nvrhi/` (without `d3d11.h`/`d3d12.h`), `src/common/` (without `dxgi-format.*`), `src/validation/`, `src/vulkan/`, `tools/nvrhi.natvis`, `LICENSE.txt` and `ThirdPartyLicenses.txt`.
4. If NVRHI's Vulkan-Headers tag changed, `git clone --depth 1 --branch <tag> https://github.com/KhronosGroup/Vulkan-Headers`. Replace `thirdparty/Vulkan-Headers/include/`, `LICENSE.md` and `LICENSES/`.
5. Normalise line endings to LF. Update the `files` list and defines in `premake5.lua`, then update this file (SHAs, versions, date).
6. Rebuild Debug, Release and Dist and run the GPU smoke test described above. Also rebuild the engine and run its tests.
