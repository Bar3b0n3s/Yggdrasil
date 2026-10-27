#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Graphics/GraphicsSpecification.h"

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan_core.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

// GPU error accounting, device-loss detection and fault injection (Architecture §8.1 "Device loss surfaces three ways",
// §8.14 items 6 and 8). GraphicsDevice owns one GpuDiagnostics, hands it to NVRHI as DeviceDesc::errorCB (and to the
// validation layer wrapper), and forwards the VK_EXT_debug_utils messenger's messages to it. Every GPU test requires zero
// errors and zero warnings over its device's whole life, teardown included (GraphicsDevice::Destroy, §15.3).

namespace Engine {

	// The severity of one message, from NVRHI's MessageSeverity or the debug messenger's severity bits.
	enum class GpuMessageSeverity : uint8_t
	{
		Info,
		Warning,
		Error,
		Fatal
	};

	// The counted messages of a device (GpuDiagnostics::GetCounts, GraphicsDevice::Destroy).
	struct GpuMessageCounts
	{
		uint64_t Errors = 0;   // Error and Fatal messages
		uint64_t Warnings = 0; // Warning messages
	};

	// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 46): what an injected fault does to one bounded GPU wait
	// (GpuDiagnostics::TakeBoundedWaitFault, WaitForSubmission in FramePacer.h).
	enum class GpuInjectedHang : uint8_t
	{
		// The wait waits for the GPU.
		None,
		// Every slice of the 10 s budget and of the reset wait times out without waiting: GpuHang (GpuFault::Hang).
		Hang,
		// Every slice of the 10 s budget times out without waiting, then the reset wait's first slice reports the device
		// lost, an injected loss (GpuFault::HangReset, HangResetPersistent).
		HangReset
	};

	// The NVRHI message callback (nvrhi::IMessageCallback) and the debug messenger's sink.
	//
	// Thread safety: every member may be called from any thread. Messages arrive on whichever thread the validation layer
	// or the driver reports from, so the counters and the flag are atomics and the logging goes through the thread-safe
	// Engine logger.
	class GpuDiagnostics final : public nvrhi::IMessageCallback
	{
	public:
		// `injectedFault` is GraphicsSpecification::InjectFault: None in Dist (asserted).
		explicit GpuDiagnostics(GpuFault injectedFault = GpuFault::None);
		~GpuDiagnostics() override;

		GpuDiagnostics(const GpuDiagnostics&) = delete;
		GpuDiagnostics& operator=(const GpuDiagnostics&) = delete;

		// nvrhi::IMessageCallback (the name is NVRHI's): ReportNvrhiMessage.
		void message(nvrhi::MessageSeverity severity, const char* messageText) override { ReportNvrhiMessage(severity, messageText); }

		// Routes NVRHI's `messageText` (null is an empty message) to the Engine logger at the matching level, prefixed
		// "NVRHI: ", and counts it (ReportMessage). NVRHI's "Device Removed!" error, which it reports after catching
		// vk::DeviceLostError inside Queue::submit, also sets the device-lost flag (§8.1).
		void ReportNvrhiMessage(nvrhi::MessageSeverity severity, const char* messageText);

		// The debug messenger's sink (and message's shared path): logs `text` at the matching level, prefixed with
		// `source` ("Vulkan validation: ..."), and counts it. Error and Fatal messages increment GetErrorCount, Warning
		// messages GetWarningCount; Info messages are logged at Trace and not counted. Synchronization hazards arrive as
		// validation errors and count as errors (§15.3).
		void ReportMessage(GpuMessageSeverity severity, std::string_view source, std::string_view text);

		// The debug messenger's callback (VK_EXT_debug_utils, GraphicsDevice.cpp) delegates here. A loader installation
		// message (IsLoaderInstallationMessage) is logged as a Warn entry "Vulkan loader (<severity>): <text>" and not
		// counted; every other message goes to ReportMessage with the source "Vulkan validation", at the severity of its
		// highest bit (error, warning, otherwise info), its text prefixed with "[<messageIdName>] " unless the ID is empty
		// or already in the text.
		void ReportDebugUtilsMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
			std::string_view messageIdName, std::string_view text);

		// Error and Fatal messages since construction (or the last ResetCounts).
		[[nodiscard]] uint64_t GetErrorCount() const;
		[[nodiscard]] uint64_t GetWarningCount() const;
		// Both counters at once.
		[[nodiscard]] GpuMessageCounts GetCounts() const;
		// Zeroes both counters (a test that provokes a validation message on purpose resets afterwards).
		void ResetCounts();

		// Set by message's "Device Removed!", by SetDeviceLost, or by the injected device-lost faults. GraphicsDevice checks it
		// after every submission (§8.1). M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 28 and 29): the device is
		// lost while it is set (GraphicsDevice::IsLost), and GraphicsDevice::Recover clears it (ClearDeviceLost) once the
		// device exists again; a recovery whose device creation fails leaves it set.
		[[nodiscard]] bool IsDeviceLost() const;
		// injected=true publishes synthetic loss and the lost flag together. Both are sticky, including across later
		// native-loss reports/SetDeviceLost(false) and ResetCounts, until ClearDeviceLost. A reader observing synthetic loss
		// through IsDeviceLost also observes IsDeviceLossInjected; native-only loss never sets that marker. Never changes
		// GetInjectedFault.
		void SetDeviceLost(bool injected = false);
		// True once loss is injected by SetDeviceLost(true), InjectUnrecoverableDeviceLoss or OnSubmitted's CLI fault, until
		// ClearDeviceLost. GraphicsDevice must never query native device-fault details for that device: synthetic loss does
		// not put the Vulkan device into a lost state.
		[[nodiscard]] bool IsDeviceLossInjected() const;
		// M18 (C18.3, decision 29): clears the device-lost flag and the injected-loss marker, which GraphicsDevice::Recover
		// calls once the device was created again, before it recreates the device's owners (a loss reported after this is a
		// second loss), and never after a failed creation. Never clears ShouldFailDeviceRecreation, and never re-arms a CLI
		// fault: the fault's arming lives apart from the flag (m_InjectedFaultArming), and this leaves it alone, so an
		// injected device-lost fault strikes once per process.
		void ClearDeviceLost();

		// M18 (C18.3, decision 30): the test hooks' unrecoverable loss (debug.deviceLost {unrecoverable: true}, the loss-test
		// helper): SetDeviceLost(true), and ShouldFailDeviceRecreation from now on, for the diagnostics' lifetime.
		void InjectUnrecoverableDeviceLoss();
		// M18 (C18.3, decision 30): whether every device creation of a recovery must fail as if the device could not be
		// created again, without calling Vulkan and without the reset wait: true under GpuFault::DeviceLostUnrecoverable and
		// after InjectUnrecoverableDeviceLoss.
		[[nodiscard]] bool ShouldFailDeviceRecreation() const;

		[[nodiscard]] GpuFault GetInjectedFault() const { return m_InjectedFault; }

		// Called by GraphicsDevice after each executeCommandLists. With GpuFault::DeviceLost or DeviceLostUnrecoverable, one
		// call sets the device-lost flag (injected), so the check that follows the submission takes the device-loss path
		// (§8.14 item 8). M18 (C18.3, decision 30): the first call after OnGarbageCollected, once per process, so an
		// application's startup submissions (the DFG LUT, the first uploads) and its first frame are unaffected; a call before
		// the first OnGarbageCollected leaves the fault armed for later, and no call strikes again after the one that did.
		void OnSubmitted();
		// M18 (C18.3, decision 30): called by GraphicsDevice::RunGarbageCollection, once per frame; the first call arms
		// OnSubmitted's injected device-lost faults (ArmInjectedFault). M18 (C18.4, decision 46): it arms C18.4's faults the
		// same way (hang-reset, oom-rendertarget, oom-pipeline and oom-swapchain, each also -persistent), so an application's
		// startup and first frame run without them.
		void OnGarbageCollected();
		// M18 (C18.4, decision 46): arms the injected faults that the first garbage collection arms (the device-lost faults
		// and C18.4's), as OnGarbageCollected's first call does; arming again changes nothing, and no fault that struck once
		// strikes again. GraphicsDevice::Create calls it when the environment variable GpuFaultAtStartupVariable is "1"
		// (non-Dist), so those faults strike an application's startup, whose failures stay fatal.
		void ArmInjectedFault();

		// GpuFault::OomTexture: whether creating a texture with `desc` must fail as if out of device memory. True for
		// sampled-only textures (neither isRenderTarget nor isUAV), so render targets, the swapchain and storage images keep
		// working while every asset-style texture creation, through either upload path, fails (§8.14 item 8).
		[[nodiscard]] bool ShouldFailTextureCreation(const nvrhi::TextureDesc& desc) const;

		// GpuFault::OomSpecialization: whether creating a specialized shader must fail as if out of device memory, so a debug
		// view's first render reaches its pipeline-creation failure path (§8.14 items 7 and 8).
		[[nodiscard]] bool ShouldFailShaderSpecialization() const { return m_InjectedFault == GpuFault::OomSpecialization; }

		// M18 (C18.4, decision 46): whether the next TakeBoundedWaitFault would return anything but None, without taking it:
		// FramePacer::BeginFrame waits through WaitForSubmission instead of polling its slot's event query when it would.
		[[nodiscard]] bool HasBoundedWaitFault() const;
		// M18 (C18.4, decision 46): what an injected fault does to the bounded wait that is starting (WaitForSubmission calls
		// it once per wait): Hang under GpuFault::Hang, at every wait from the start (M5's fault, unarmed); HangReset under
		// GpuFault::HangReset at the first wait after OnGarbageCollected armed it, once per process (the struck bit of
		// m_InjectedFaultArming, which ClearDeviceLost leaves alone), and under GpuFault::HangResetPersistent at every wait
		// after the first OnGarbageCollected; otherwise None. A wait whose fault is not None skips its completed-submission
		// early return. Takes the once-per-process strike.
		[[nodiscard]] GpuInjectedHang TakeBoundedWaitFault();

		// M18 (C18.4, decision 46): whether this creation of a render-target or storage texture (isRenderTarget or isUAV) must
		// fail as if the device were out of memory: under GpuFault::OomRenderTarget the first such creation after the faults
		// were armed (ArmInjectedFault), once per process, and under GpuFault::OomRenderTargetPersistent every one after.
		// GraphicsDevice::CreateTexture calls it once per creation of a render-target or storage texture (sampled-only and
		// staging textures are not render targets: GpuFault::OomTexture covers the former). Takes the once-per-process strike.
		[[nodiscard]] bool TakeRenderTargetCreationFault();
		// M18 (C18.4, decision 46): the same for the creation of a graphics or compute pipeline (GraphicsDevice::
		// CreateGraphicsPipeline, CreateComputePipeline) under GpuFault::OomPipeline and OomPipelinePersistent.
		[[nodiscard]] bool TakePipelineCreationFault();
		// M18 (C18.4, decision 46): the same for the creation of a swapchain (Swapchain's vkCreateSwapchainKHR, which then
		// reports VK_ERROR_OUT_OF_DEVICE_MEMORY without calling Vulkan) under GpuFault::OomSwapchain and
		// OomSwapchainPersistent.
		[[nodiscard]] bool TakeSwapchainCreationFault();
	private:
		// M18 (C18.4): the shared rule of the Take functions: whether a fault of `onceFault` or `persistentFault` strikes now,
		// once armed (ArmInjectedFault), the once fault only at its first call.
		[[nodiscard]] bool TakeArmedFault(GpuFault onceFault, GpuFault persistentFault);
	private:
		GpuFault m_InjectedFault = GpuFault::None;
		std::atomic<uint64_t> m_ErrorCount{ 0 };
		std::atomic<uint64_t> m_WarningCount{ 0 };
		std::atomic<uint8_t> m_DeviceLossState{ 0 }; // lost and synthetic bits are published in one atomic operation
		// M18 (C18.3): InjectUnrecoverableDeviceLoss's marker.
		std::atomic<bool> m_FailDeviceRecreation{ false };
		// M18 (C18.3, decision 30): the injected device-lost faults' once-per-process state, apart from m_DeviceLossState so
		// that ClearDeviceLost never re-arms them: an armed bit (OnGarbageCollected's first call) and a struck bit (the
		// OnSubmitted call that set the flag; no later call strikes again). M18 (C18.4, decision 46): C18.4's faults share it
		// (a process injects one fault): their once spellings strike at the first Take call after the arming, which sets the
		// struck bit, and their -persistent spellings need only the armed bit.
		std::atomic<uint8_t> m_InjectedFaultArming{ 0 };
	};

	// The fatal error a failed Vulkan result becomes (§4.6 item 2, §8.14 item 6): VK_ERROR_DEVICE_LOST is DeviceLost,
	// VK_ERROR_OUT_OF_HOST_MEMORY and VK_ERROR_OUT_OF_DEVICE_MEMORY are OutOfMemory, anything else Gpu. Used by the
	// frame-boundary catch of vk::SystemError in App/FrameLoop.cpp and by the VkResult paths of Swapchain and FramePacer.
	[[nodiscard]] FatalErrorKind GetFatalErrorKind(VkResult result);

	// "ErrorDeviceLost (-4)": vulkan.hpp's name of `result` and its value, how every message of the engine names a VkResult.
	[[nodiscard]] std::string VkResultToString(VkResult result);

	// Whether a debug messenger message is one of the Khronos loader's notes about the machine's installation rather
	// than about the application's use of Vulkan: the message ID name "Loader Message" without the validation or
	// performance type. Examples are a third-party implicit layer built for an older API version, an unreadable manifest
	// or no driver at all. They are logged without counting (ADR 0009 decision 26): a machine without a usable driver
	// fails GraphicsDevice::Create with an error value instead. The loader's validation of API calls carries the
	// validation type and counts like the layer's messages.
	[[nodiscard]] bool IsLoaderInstallationMessage(VkDebugUtilsMessageTypeFlagsEXT types, std::string_view messageIdName);

	// "Info", "Warning", "Error" or "Fatal".
	[[nodiscard]] std::string_view GpuMessageSeverityToString(GpuMessageSeverity severity);

}
