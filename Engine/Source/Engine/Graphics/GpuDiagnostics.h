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

		// Set by message's "Device Removed!", by SetDeviceLost, or by the injected device-lost fault. GraphicsDevice checks it
		// after every submission and then calls RaiseDeviceLost (§8.1). Never cleared: recovery is out of scope (§8.14).
		[[nodiscard]] bool IsDeviceLost() const;
		// injected=true publishes synthetic loss and the lost flag together. Both are sticky, including across later
		// native-loss reports/SetDeviceLost(false) and ResetCounts. A reader observing synthetic loss through IsDeviceLost
		// also observes IsDeviceLossInjected; native-only loss never sets that marker. Never changes GetInjectedFault.
		void SetDeviceLost(bool injected = false);
		// True once loss is injected by SetDeviceLost(true) or OnSubmitted's CLI fault. GraphicsDevice must never query
		// native device-fault details for that device: synthetic loss does not put the Vulkan device into a lost state.
		[[nodiscard]] bool IsDeviceLossInjected() const;

		[[nodiscard]] GpuFault GetInjectedFault() const { return m_InjectedFault; }

		// Called by GraphicsDevice after each executeCommandLists. With GpuFault::DeviceLost, the first call sets the
		// device-lost flag, so the check that follows the submission takes the device-loss path (§8.14 item 8).
		void OnSubmitted();

		// GpuFault::OomTexture: whether creating a texture with `desc` must fail as if out of device memory. True for
		// sampled-only textures (neither isRenderTarget nor isUAV), so render targets, the swapchain and storage images keep
		// working while every asset-style texture creation, through either upload path, fails (§8.14 item 8).
		[[nodiscard]] bool ShouldFailTextureCreation(const nvrhi::TextureDesc& desc) const;

		// GpuFault::Hang: whether bounded GPU waits must hang (FramePacer.h): WaitForSubmission skips its completed-submission
		// early return, FramePacer::BeginFrame skips pollEventQuery once its slot has a recorded submission, and every wait
		// slice reports VK_TIMEOUT without waiting, so the first wait ends the process with FatalError(GpuHang) whatever the
		// GPU's progress.
		[[nodiscard]] bool ShouldInjectHang() const { return m_InjectedFault == GpuFault::Hang; }
	private:
		GpuFault m_InjectedFault = GpuFault::None;
		std::atomic<uint64_t> m_ErrorCount{ 0 };
		std::atomic<uint64_t> m_WarningCount{ 0 };
		std::atomic<uint8_t> m_DeviceLossState{ 0 }; // lost and synthetic bits are published in one atomic operation
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
