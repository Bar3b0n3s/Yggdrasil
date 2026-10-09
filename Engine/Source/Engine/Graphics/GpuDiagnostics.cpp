#include "EnginePCH.h"
#include "Engine/Graphics/GpuDiagnostics.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"

#include <vulkan/vulkan.hpp>

namespace Engine {

	namespace Utils {

		// What NVRHI's Vulkan backend reports through its message callback after catching vk::DeviceLostError inside
		// Queue::submit (§8.1); the vendored source is pinned, so the text is exact.
		constexpr std::string_view NvrhiDeviceRemovedMessage = "Device Removed!";
		// VkDebugUtilsMessengerCallbackDataEXT::pMessageIdName of the Khronos loader's own messages.
		constexpr std::string_view LoaderMessageIdName = "Loader Message";
		constexpr uint8_t DeviceLostFlag = 1;
		constexpr uint8_t InjectedDeviceLossFlag = 2;

		static GpuMessageSeverity FromNvrhiSeverity(nvrhi::MessageSeverity severity)
		{
			switch (severity)
			{
				case nvrhi::MessageSeverity::Info:    return GpuMessageSeverity::Info;
				case nvrhi::MessageSeverity::Warning: return GpuMessageSeverity::Warning;
				case nvrhi::MessageSeverity::Error:   return GpuMessageSeverity::Error;
				case nvrhi::MessageSeverity::Fatal:   return GpuMessageSeverity::Fatal;
			}

			// An unknown severity from a newer NVRHI is reported as an error rather than dropped.
			ENGINE_CORE_ASSERT(false, "Unknown nvrhi::MessageSeverity {}", std::to_underlying(severity));
			return GpuMessageSeverity::Error;
		}

	}

	GpuDiagnostics::GpuDiagnostics(GpuFault injectedFault)
		: m_InjectedFault(injectedFault)
	{
		ENGINE_CORE_ASSERT(GpuTestHooksEnabled || injectedFault == GpuFault::None, "--gpu-inject-fault does not exist in Dist");
	}

	GpuDiagnostics::~GpuDiagnostics() = default;

	void GpuDiagnostics::ReportNvrhiMessage(nvrhi::MessageSeverity severity, const char* messageText)
	{
		const std::string_view text = messageText != nullptr ? std::string_view(messageText) : std::string_view();
		const GpuMessageSeverity mapped = Utils::FromNvrhiSeverity(severity);
		ReportMessage(mapped, "NVRHI", text);
		if (text == Utils::NvrhiDeviceRemovedMessage && (mapped == GpuMessageSeverity::Error || mapped == GpuMessageSeverity::Fatal))
			SetDeviceLost();
	}

	void GpuDiagnostics::ReportMessage(GpuMessageSeverity severity, std::string_view source, std::string_view text)
	{
		switch (severity)
		{
			case GpuMessageSeverity::Info:
			{
				ENGINE_CORE_TRACE("{}: {}", source, text);
				return;
			}
			case GpuMessageSeverity::Warning:
			{
				m_WarningCount.fetch_add(1);
				ENGINE_CORE_WARN("{}: {}", source, text);
				return;
			}
			case GpuMessageSeverity::Error:
			{
				m_ErrorCount.fetch_add(1);
				ENGINE_CORE_ERROR("{}: {}", source, text);
				return;
			}
			case GpuMessageSeverity::Fatal:
			{
				m_ErrorCount.fetch_add(1);
				ENGINE_CORE_CRITICAL("{}: {}", source, text);
				return;
			}
		}

		// A corrupted severity still counts: GPU tests must never pass over a message they could not classify.
		ENGINE_CORE_ASSERT(false, "Unknown GpuMessageSeverity {}", std::to_underlying(severity));
		m_ErrorCount.fetch_add(1);
		ENGINE_CORE_ERROR("{}: {}", source, text);
	}

	void GpuDiagnostics::ReportDebugUtilsMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
		std::string_view messageIdName, std::string_view text)
	{
		GpuMessageSeverity level = GpuMessageSeverity::Info;
		if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
			level = GpuMessageSeverity::Error;
		else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
			level = GpuMessageSeverity::Warning;

		if (IsLoaderInstallationMessage(types, messageIdName))
		{
			ENGINE_CORE_WARN("Vulkan loader ({}): {}", GpuMessageSeverityToString(level), text);
			return;
		}
		// The message ID ("VUID-vkCmdDraw-None-08600", "SYNC-HAZARD-WRITE-AFTER-WRITE") names the rule; recent layers keep
		// it out of the text.
		if (messageIdName.empty() || text.contains(messageIdName))
			ReportMessage(level, "Vulkan validation", text);
		else
			ReportMessage(level, "Vulkan validation", std::format("[{}] {}", messageIdName, text));
	}

	uint64_t GpuDiagnostics::GetErrorCount() const
	{
		return m_ErrorCount.load();
	}

	uint64_t GpuDiagnostics::GetWarningCount() const
	{
		return m_WarningCount.load();
	}

	GpuMessageCounts GpuDiagnostics::GetCounts() const
	{
		return GpuMessageCounts{ .Errors = GetErrorCount(), .Warnings = GetWarningCount() };
	}

	void GpuDiagnostics::ResetCounts()
	{
		m_ErrorCount.store(0);
		m_WarningCount.store(0);
	}

	bool GpuDiagnostics::IsDeviceLost() const
	{
		return (m_DeviceLossState.load(std::memory_order_acquire) & Utils::DeviceLostFlag) != 0;
	}

	void GpuDiagnostics::SetDeviceLost(bool injected)
	{
		const uint8_t flags = injected ? Utils::DeviceLostFlag | Utils::InjectedDeviceLossFlag : Utils::DeviceLostFlag;
		m_DeviceLossState.fetch_or(flags, std::memory_order_release);
	}

	bool GpuDiagnostics::IsDeviceLossInjected() const
	{
		return (m_DeviceLossState.load(std::memory_order_acquire) & Utils::InjectedDeviceLossFlag) != 0;
	}

	void GpuDiagnostics::OnSubmitted()
	{
		if (m_InjectedFault != GpuFault::DeviceLost)
			return;
		// The first submission sets the flag; the check after it (GraphicsDevice::ExecuteCommandList) ends the process.
		const uint8_t previous = m_DeviceLossState.fetch_or(Utils::DeviceLostFlag | Utils::InjectedDeviceLossFlag, std::memory_order_acq_rel);
		if ((previous & Utils::DeviceLostFlag) == 0)
			ENGINE_CORE_WARN("Injected GPU fault (device-lost): the device is reported lost after this submission");
	}

	bool GpuDiagnostics::ShouldFailTextureCreation(const nvrhi::TextureDesc& desc) const
	{
		return m_InjectedFault == GpuFault::OomTexture && !desc.isRenderTarget && !desc.isUAV;
	}

	FatalErrorKind GetFatalErrorKind(VkResult result)
	{
		switch (result)
		{
			case VK_ERROR_DEVICE_LOST:
				return FatalErrorKind::DeviceLost;
			case VK_ERROR_OUT_OF_HOST_MEMORY:
			case VK_ERROR_OUT_OF_DEVICE_MEMORY:
				return FatalErrorKind::OutOfMemory;
			default:
				return FatalErrorKind::Gpu;
		}
	}

	std::string VkResultToString(VkResult result)
	{
		return std::format("{} ({})", vk::to_string(static_cast<vk::Result>(result)), std::to_underlying(result));
	}

	bool IsLoaderInstallationMessage(VkDebugUtilsMessageTypeFlagsEXT types, std::string_view messageIdName)
	{
		constexpr VkDebugUtilsMessageTypeFlagsEXT ValidationTypes =
			VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		return (types & ValidationTypes) == 0 && messageIdName == Utils::LoaderMessageIdName;
	}

	std::string_view GpuMessageSeverityToString(GpuMessageSeverity severity)
	{
		switch (severity)
		{
			case GpuMessageSeverity::Info:    return "Info";
			case GpuMessageSeverity::Warning: return "Warning";
			case GpuMessageSeverity::Error:   return "Error";
			case GpuMessageSeverity::Fatal:   return "Fatal";
		}

		ENGINE_CORE_ASSERT(false, "Unknown GpuMessageSeverity {}", std::to_underlying(severity));
		return "Unknown";
	}

}
