-- NVRHI (NVIDIA Rendering Hardware Interface), vendored.
-- One static library containing the common sources, the validation layer and the
-- Vulkan backend only (no D3D11/D3D12, RTXMU, NVAPI or Aftermath SDK).
-- Mirrors upstream CMakeLists.txt with NVRHI_WITH_VULKAN=ON, NVRHI_WITH_VALIDATION=ON,
-- NVRHI_WITH_DX11/DX12/RTXMU/NVAPI/AFTERMATH=OFF, NVRHI_BUILD_SHARED=OFF.
-- See VENDOR.md for the defines and include directories consumers must use.
--
-- OutputDir is a global defined by the root workspace.

project "NVRHI"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		-- Public headers (include_common, include_validation, include_vk)
		"include/nvrhi/nvrhi.h",
		"include/nvrhi/nvrhiHLSL.h",
		"include/nvrhi/utils.h",
		"include/nvrhi/validation.h",
		"include/nvrhi/vulkan.h",
		"include/nvrhi/common/aftermath.h",
		"include/nvrhi/common/containers.h",
		"include/nvrhi/common/misc.h",
		"include/nvrhi/common/resource.h",
		"include/nvrhi/common/resourcebindingmap.h",

		-- src_common
		"src/common/aftermath.cpp",
		"src/common/format-info.cpp",
		"src/common/misc.cpp",
		"src/common/state-tracking.cpp",
		"src/common/state-tracking.h",
		"src/common/utils.cpp",

		-- src_validation
		"src/validation/validation-backend.h",
		"src/validation/validation-commandlist.cpp",
		"src/validation/validation-device.cpp",

		-- src_vk
		"src/common/versioning.h",
		"src/vulkan/vulkan-allocator.cpp",
		"src/vulkan/vulkan-backend.h",
		"src/vulkan/vulkan-buffer.cpp",
		"src/vulkan/vulkan-commandlist.cpp",
		"src/vulkan/vulkan-compute.cpp",
		"src/vulkan/vulkan-constants.cpp",
		"src/vulkan/vulkan-device.cpp",
		"src/vulkan/vulkan-graphics.cpp",
		"src/vulkan/vulkan-meshlets.cpp",
		"src/vulkan/vulkan-queries.cpp",
		"src/vulkan/vulkan-queue.cpp",
		"src/vulkan/vulkan-raytracing.cpp",
		"src/vulkan/vulkan-resource-bindings.cpp",
		"src/vulkan/vulkan-shader.cpp",
		"src/vulkan/vulkan-staging-texture.cpp",
		"src/vulkan/vulkan-state-tracking.cpp",
		"src/vulkan/vulkan-texture.cpp",
		"src/vulkan/vulkan-upload.cpp",
	}

	includedirs
	{
		"include",
		"thirdparty/Vulkan-Headers/include",
	}

	defines
	{
		-- PUBLIC: vulkan-backend.h hardcodes this before including vulkan.hpp; consumers must match it.
		"VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1",
		-- PRIVATE (upstream: NVRHI_WITH_AFTERMATH=$<BOOL:OFF>)
		"NVRHI_WITH_AFTERMATH=0",
	}

	filter "system:windows"
		systemversion "latest"
		multiprocessorcompile "On"
		-- PUBLIC upstream. Required for vkGetMemoryWin32HandleKHR (shared resources); makes vulkan.h include <windows.h>.
		-- NOMINMAX is PRIVATE upstream, but consumers need it too because <windows.h> comes in through vulkan.h.
		defines { "VK_USE_PLATFORM_WIN32_KHR", "NOMINMAX" }
		-- Debugger visualizers (upstream adds this file to MSVC builds only).
		files { "tools/nvrhi.natvis" }

	filter "system:linux"
		pic "On"

	filter "system:macosx"
		-- No platform defines: upstream sets none, and external memory uses the core opaque-fd path.

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
		symbols "on"

	filter "configurations:Dist"
		runtime "Release"
		optimize "full"
		symbols "off"

	-- NDEBUG is deliberately NOT set here: vulkan.hpp's DispatchLoaderBase changes layout with NDEBUG,
	-- so it must be identical for NVRHI and every consumer TU. Set it (if wanted) at workspace scope.

	filter {}
