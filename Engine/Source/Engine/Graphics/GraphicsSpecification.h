#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// How an application renders (Architecture §4.1 ApplicationSpecification::Renderer and ::Graphics, §8.1, §8.14). Plain
// data and pure conversions: the engine command line fills it (ApplyEngineCommandLine), EngineContext hands it to
// GraphicsDevice::Create, and the GPU tests build it directly (Tests/Source/Support/HeadlessGpuFixture.h).

namespace Engine {

	// Whether a context renders at all (§4.1).
	enum class RendererMode : uint8_t
	{
		None,  // logic only: no Vulkan loader, no GraphicsDevice, screenshot methods return Unsupported (§13.9)
		Vulkan // a GraphicsDevice on the process's Vulkan loader
	};

	// The highest Vulkan API version the instance requests (§8.1). The engine needs 1.3; 1.4 adds the optional,
	// feature-gated paths of §8.1 (host image copy), each with a 1.3 fallback that --vulkan-api=1.3 forces.
	enum class VulkanApiVersion : uint8_t
	{
		Vulkan13,
		Vulkan14
	};

	// The faults --gpu-inject-fault forces in non-Dist builds (§8.14 item 8; GpuDiagnostics).
	enum class GpuFault : uint8_t
	{
		None,
		DeviceLost, // "device-lost": the device-lost flag is set after the next submission
		OomTexture, // "oom-texture": creating a sampled-only texture fails as if the device were out of memory
		Hang        // "hang": every bounded GPU wait times out until its budget is spent, so the first one ends in GpuHang
	};

	// Whether Vulkan and NVRHI validation is on by default in this configuration (§2.2): on in Debug, off in Release
	// (--gpu-validation turns it on; the GPU tests always do), never in Dist.
	inline constexpr bool DefaultGpuValidation =
#if defined(ENGINE_DEBUG)
		true;
#else
		false;
#endif

	// Whether this build honours the GPU test hooks: --gpu-inject-fault and the ENGINE_VULKAN_LOADER environment variable
	// (§8.14 item 8, Roadmap M5). Dist builds ignore both.
	inline constexpr bool GpuTestHooksEnabled =
#if defined(ENGINE_DIST)
		false;
#else
		true;
#endif

	struct GraphicsSpecification
	{
		// VK_LAYER_KHRONOS_validation, VK_EXT_debug_utils and NVRHI's validation layer (§8.1). Ignored in Dist, which never
		// validates. A missing validation layer is an error (Unsupported) when this is on, so a test never silently runs
		// without it.
		bool Validation = DefaultGpuValidation;
		// VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT on top of Validation (§15.3): the GPU tests turn it on.
		// Ignored without Validation.
		bool SynchronizationValidation = false;
		// --vulkan-api: the instance requests min(this, what vk::enumerateInstanceVersion reports).
		VulkanApiVersion MaxApiVersion = VulkanApiVersion::Vulkan14;
		// --gpu=<index|substring>: a physical-device index in enumeration order, or a case-insensitive substring of the
		// device name. Empty: the ENGINE_GPU environment variable, read by GraphicsDevice::Create, then the best-scoring
		// device (DeviceSelection.h).
		std::string GpuOverride{};
		// Frames the CPU may record ahead of the GPU (§4.1: 2). At least 1.
		uint32_t FramesInFlight = 2;
		// FIFO presentation, else MAILBOX, else IMMEDIATE (§8.1). Off prefers MAILBOX, then IMMEDIATE, then FIFO.
		bool VSync = true;
		// --gpu-inject-fault. Must be None in Dist builds (GpuTestHooksEnabled); GraphicsDevice::Create rejects any other
		// value there with InvalidArgument.
		GpuFault InjectFault = GpuFault::None;
	};

	// "None" or "Vulkan".
	[[nodiscard]] constexpr std::string_view RendererModeToString(RendererMode mode)
	{
		switch (mode)
		{
			case RendererMode::None:   return "None";
			case RendererMode::Vulkan: return "Vulkan";
		}
		return "Unknown";
	}

	// The --renderer spelling, which session.info reports too: "vulkan" or "none".
	[[nodiscard]] constexpr std::string_view RendererModeToCommandLine(RendererMode mode)
	{
		switch (mode)
		{
			case RendererMode::None:   return "none";
			case RendererMode::Vulkan: return "vulkan";
		}
		return "unknown";
	}

	// The --renderer spelling: "vulkan" or "none" (lower case, exactly). nullopt for anything else.
	[[nodiscard]] constexpr std::optional<RendererMode> RendererModeFromCommandLine(std::string_view text)
	{
		if (text == "vulkan")
			return RendererMode::Vulkan;
		if (text == "none")
			return RendererMode::None;
		return std::nullopt;
	}

	// "1.3" or "1.4", the --vulkan-api spelling.
	[[nodiscard]] constexpr std::string_view VulkanApiVersionToString(VulkanApiVersion version)
	{
		switch (version)
		{
			case VulkanApiVersion::Vulkan13: return "1.3";
			case VulkanApiVersion::Vulkan14: return "1.4";
		}
		return "Unknown";
	}

	// Parses the --vulkan-api spelling "1.3" or "1.4"; nullopt for anything else.
	[[nodiscard]] constexpr std::optional<VulkanApiVersion> VulkanApiVersionFromString(std::string_view text)
	{
		if (text == "1.3")
			return VulkanApiVersion::Vulkan13;
		if (text == "1.4")
			return VulkanApiVersion::Vulkan14;
		return std::nullopt;
	}

	// The --gpu-inject-fault spelling: "none", "device-lost", "oom-texture" or "hang".
	[[nodiscard]] constexpr std::string_view GpuFaultToString(GpuFault fault)
	{
		switch (fault)
		{
			case GpuFault::None:       return "none";
			case GpuFault::DeviceLost: return "device-lost";
			case GpuFault::OomTexture: return "oom-texture";
			case GpuFault::Hang:       return "hang";
		}
		return "unknown";
	}

	// Parses the --gpu-inject-fault spelling (exactly, lower case); nullopt for anything else.
	[[nodiscard]] constexpr std::optional<GpuFault> GpuFaultFromString(std::string_view text)
	{
		if (text == "none")
			return GpuFault::None;
		if (text == "device-lost")
			return GpuFault::DeviceLost;
		if (text == "oom-texture")
			return GpuFault::OomTexture;
		if (text == "hang")
			return GpuFault::Hang;
		return std::nullopt;
	}

}
