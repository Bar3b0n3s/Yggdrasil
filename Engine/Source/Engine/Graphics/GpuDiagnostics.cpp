#include "EnginePCH.h"
#include "Engine/Graphics/GpuDiagnostics.h"

#include "Engine/Core/Assert.h"

// M5 contract stub (Roadmap rule 3): stream B (swapchain, present, frame pacing, fault handling) implements message
// routing, the device-lost detection of NVRHI's "Device Removed!" message, the injected faults and the VkResult mapping.
// The counters and the flag are implemented.

namespace Engine {

	GpuDiagnostics::GpuDiagnostics(GpuFault injectedFault)
		: m_InjectedFault(injectedFault)
	{
		ENGINE_CORE_ASSERT(GpuTestHooksEnabled || injectedFault == GpuFault::None, "--gpu-inject-fault does not exist in Dist");
	}

	GpuDiagnostics::~GpuDiagnostics() = default;

	void GpuDiagnostics::ReportNvrhiMessage(nvrhi::MessageSeverity /*severity*/, const char* /*messageText*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void GpuDiagnostics::ReportMessage(GpuMessageSeverity /*severity*/, std::string_view /*source*/, std::string_view /*text*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	uint64_t GpuDiagnostics::GetErrorCount() const
	{
		return m_ErrorCount.load();
	}

	uint64_t GpuDiagnostics::GetWarningCount() const
	{
		return m_WarningCount.load();
	}

	void GpuDiagnostics::ResetCounts()
	{
		m_ErrorCount.store(0);
		m_WarningCount.store(0);
	}

	bool GpuDiagnostics::IsDeviceLost() const
	{
		return m_IsDeviceLost.load();
	}

	void GpuDiagnostics::SetDeviceLost()
	{
		m_IsDeviceLost.store(true);
	}

	void GpuDiagnostics::OnSubmitted()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool GpuDiagnostics::ShouldFailTextureCreation(const nvrhi::TextureDesc& /*desc*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	FatalErrorKind GetFatalErrorKind(VkResult /*result*/)
	{
		ENGINE_CONTRACT_STUB();
		return FatalErrorKind::Gpu;
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
