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

	// The faults --gpu-inject-fault forces in non-Dist builds (§8.14 item 8; GpuDiagnostics). Values are appended, never
	// reordered.
	enum class GpuFault : uint8_t
	{
		None,
		// "device-lost": the device-lost flag is set after a submission. M18 (C18.3, Docs/Decisions/0029-m18-contract.md
		// decision 30): at the first submission after the device's first RunGarbageCollection (an application's second
		// frame, once its startup objects exist), once per process; the frame boundary recovers it (GraphicsDevice::Recover)
		// and the process keeps running.
		DeviceLost,
		OomTexture, // "oom-texture": creating a sampled-only texture fails as if the device were out of memory
		// "hang": every bounded GPU wait times out, its 10 s budget and then its reset wait (M18, C18.4: no loss report
		// arrives), so the first wait with a submission ends in GpuHang: the hang that stays fatal.
		Hang,
		// "oom-specialization": creating a specialized shader fails as if the device were out of memory. Only the debug views'
		// forward pipelines, created on first use (§8.5), specialize shaders, so startup is unaffected.
		OomSpecialization,
		// M18 (C18.3, decision 30), "device-lost-unrecoverable": the same loss as DeviceLost, and every device creation of its
		// recovery fails (GpuDiagnostics::ShouldFailDeviceRecreation) without the reset wait, so the recovery ends in
		// FatalError(DeviceLost) after its one retry: the editor's autosave, a crash report and exit code 4.
		DeviceLostUnrecoverable,
		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 46): the hangs a driver or the operating system resets
		// (GpuDiagnostics::TakeBoundedWaitFault). "hang-reset": the first bounded wait after the device's first garbage
		// collection times out its 10 s budget, and the first slice of its reset wait reports the device lost, which the frame
		// boundary recovers like any loss; once per process. "hang-reset-persistent": every bounded wait after the first
		// garbage collection does so, the recovered device's included, so a device that hangs again after its recovery is
		// recovered again.
		HangReset,
		HangResetPersistent,
		// M18 (C18.4, decision 46): out-of-memory after startup, one kind of creation each (GpuDiagnostics::
		// TakeRenderTargetCreationFault, TakePipelineCreationFault, TakeSwapchainCreationFault). "oom-rendertarget": the first
		// creation of a render-target or storage texture (isRenderTarget or isUAV) after the device's first garbage collection
		// fails as if the device were out of memory, once per process, so the request's retry after its trim succeeds.
		// "oom-pipeline": the same for the first graphics or compute pipeline; "oom-swapchain": for the first swapchain
		// creation (a resize's recreation). Their "-persistent" spellings fail every such creation after the first garbage
		// collection, the retry's included, so the request fails alone with ErrorCode::OutOfMemory. None of them strikes an
		// application's startup, unless GpuFaultAtStartupVariable arms it at the device's creation (decision 46).
		OomRenderTarget,
		OomRenderTargetPersistent,
		OomPipeline,
		OomPipelinePersistent,
		OomSwapchain,
		OomSwapchainPersistent
	};

	// M18 (C18.4, decision 46): the environment variable that arms the injected faults that otherwise arm at the device's
	// first garbage collection (the device-lost faults, the hang resets and the oom-* faults of C18.4) at the device's
	// creation instead, so they strike an application's startup: "1" arms them, anything else (or unset) leaves the
	// garbage-collection arming. GraphicsDevice::Create reads it in non-Dist builds (GpuTestHooksEnabled), like
	// ENGINE_VULKAN_LOADER, and a process test sets it to prove that out-of-memory at startup stays fatal (exit code 4, a
	// crash report and the editor's autosave hook) while the same fault after startup fails only its request.
	inline constexpr std::string_view GpuFaultAtStartupVariable = "ENGINE_GPU_FAULT_AT_STARTUP";

	// M18 (C18.3, decision 29; C18.4, decisions 40 and 41): the default reset wait (GraphicsSpecification::
	// ResetWaitMilliseconds), long enough for the operating systems' GPU resets: a Windows TDR takes about 2 s plus the
	// driver's recovery, a Linux kernel GPU hang check up to about 10 s.
	inline constexpr uint32_t DefaultGpuResetWaitMilliseconds = 15'000;
	// M18 (C18.4, decision 41): the longest reset wait --gpu-reset-wait accepts, two minutes, so a hang that no reset ends
	// still ends the process in bounded time (a Windows TdrDelay raised up to about 110 s still fits).
	inline constexpr uint32_t MaxGpuResetWaitMilliseconds = 120'000;

	// Whether Vulkan and NVRHI validation is on by default in this configuration (§2.2): on in Debug, off in Release
	// (--gpu-validation turns it on; the GPU tests always do), never in Dist.
	inline constexpr bool DefaultGpuValidation =
#if defined(ENGINE_DEBUG)
		true;
#else
		false;
#endif

	// Whether this build honours the GPU test hooks: --gpu-inject-fault and the ENGINE_VULKAN_LOADER environment variable
	// (§8.14 item 8, Roadmap M5), and (M18, C18.4) the ENGINE_GPU_FAULT_AT_STARTUP environment variable
	// (GpuFaultAtStartupVariable). Dist builds ignore them.
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
		// Turns GraphicsDeviceInfo::DepthClamp off: directional shadow casters are then clipped at the extended light near
		// plane instead of pancaked per fragment (ShadowPass). Pancaking needs no device feature, so the default keeps it on
		// every device. Honoured in every configuration, with no command-line option, so callers can deterministically
		// exercise the extended-near shadow fallback.
		bool DisableDepthClamp = false;
		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 29; C18.4, decisions 40 and 41): the reset wait, the one
		// setting of the operating system's GPU reset window. A bounded wait whose 10 s budget timed out waits up to this long
		// more, in GpuWaitSliceNanoseconds slices, for a loss report before the hang is fatal (WaitForSubmission, FramePacer.h),
		// and a device-loss recovery whose first device creation fails waits this long before its one retry
		// (GraphicsDevice::Recover). 0 waits neither: a hang is fatal when its budget times out, and the recovery retries at
		// once. Set by --gpu-reset-wait <milliseconds> in every configuration, Dist included, because the reset window
		// belongs to the machine that runs the process, not to a project (decision 41); the option accepts 0 to
		// MaxGpuResetWaitMilliseconds. A specification set in code may hold any value (tests set UINT32_MAX to prove that a
		// path takes no wait), and GetGpuResetWaitSliceCount (FramePacer.h) handles every uint32_t. Tests never wait it: an
		// injected hang reset reports the loss at the reset wait's first slice, and an injected recreation failure retries
		// at once.
		uint32_t ResetWaitMilliseconds = DefaultGpuResetWaitMilliseconds;
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

	// The --gpu-inject-fault spelling: "none", "device-lost", "oom-texture", "hang", "oom-specialization",
	// "device-lost-unrecoverable", "hang-reset", "hang-reset-persistent", "oom-rendertarget", "oom-rendertarget-persistent",
	// "oom-pipeline", "oom-pipeline-persistent", "oom-swapchain" or "oom-swapchain-persistent".
	[[nodiscard]] constexpr std::string_view GpuFaultToString(GpuFault fault)
	{
		switch (fault)
		{
			case GpuFault::None:                      return "none";
			case GpuFault::DeviceLost:                return "device-lost";
			case GpuFault::OomTexture:                return "oom-texture";
			case GpuFault::Hang:                      return "hang";
			case GpuFault::OomSpecialization:         return "oom-specialization";
			case GpuFault::DeviceLostUnrecoverable:   return "device-lost-unrecoverable";
			case GpuFault::HangReset:                 return "hang-reset";
			case GpuFault::HangResetPersistent:       return "hang-reset-persistent";
			case GpuFault::OomRenderTarget:           return "oom-rendertarget";
			case GpuFault::OomRenderTargetPersistent: return "oom-rendertarget-persistent";
			case GpuFault::OomPipeline:               return "oom-pipeline";
			case GpuFault::OomPipelinePersistent:     return "oom-pipeline-persistent";
			case GpuFault::OomSwapchain:              return "oom-swapchain";
			case GpuFault::OomSwapchainPersistent:    return "oom-swapchain-persistent";
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
		if (text == "oom-specialization")
			return GpuFault::OomSpecialization;
		if (text == "device-lost-unrecoverable")
			return GpuFault::DeviceLostUnrecoverable;
		if (text == "hang-reset")
			return GpuFault::HangReset;
		if (text == "hang-reset-persistent")
			return GpuFault::HangResetPersistent;
		if (text == "oom-rendertarget")
			return GpuFault::OomRenderTarget;
		if (text == "oom-rendertarget-persistent")
			return GpuFault::OomRenderTargetPersistent;
		if (text == "oom-pipeline")
			return GpuFault::OomPipeline;
		if (text == "oom-pipeline-persistent")
			return GpuFault::OomPipelinePersistent;
		if (text == "oom-swapchain")
			return GpuFault::OomSwapchain;
		if (text == "oom-swapchain-persistent")
			return GpuFault::OomSwapchainPersistent;
		return std::nullopt;
	}

}
