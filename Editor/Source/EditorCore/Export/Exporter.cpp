#include "EditorPCH.h"
#include "EditorCore/Export/Exporter.h"

#include "Engine/Core/Assert.h"

#include <utility>

namespace Engine {

	struct Exporter::State
	{
		ExportPhase Phase = ExportPhase::Validate;
	};

	Exporter::Exporter(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	Exporter::~Exporter() = default;

	Result<Scope<Exporter>> Exporter::Start(EditorContext& /*editor*/, const ExportSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "exports are not implemented yet (M7 stream D)");
	}

	std::optional<Result<ExportReport>> Exporter::Poll()
	{
		ENGINE_CONTRACT_STUB();
		return Result<ExportReport>(MakeError(ErrorCode::Unsupported, "exports are not implemented yet (M7 stream D)"));
	}

	void Exporter::Cancel()
	{
		ENGINE_CONTRACT_STUB();
	}

	ExportPhase Exporter::GetPhase() const
	{
		return m_State->Phase;
	}

	std::string Exporter::GetDefaultOutputDirectory(std::string_view /*projectName*/, ExportConfiguration /*configuration*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string_view Exporter::GetPlatformName()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string Exporter::GetBuildOutputDirectoryName(ExportConfiguration /*configuration*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string_view ExportConfigurationToString(ExportConfiguration configuration)
	{
		switch (configuration)
		{
			case ExportConfiguration::Debug:   return "Debug";
			case ExportConfiguration::Release: return "Release";
			case ExportConfiguration::Dist:    return "Dist";
		}

		ENGINE_ASSERT(false, "Unknown ExportConfiguration {}", std::to_underlying(configuration));
		return "Unknown";
	}

	std::string_view ExportPhaseToString(ExportPhase phase)
	{
		switch (phase)
		{
			case ExportPhase::Validate:      return "Validate";
			case ExportPhase::Cook:          return "Cook";
			case ExportPhase::WritePaks:     return "WritePaks";
			case ExportPhase::CopyRuntime:   return "CopyRuntime";
			case ExportPhase::WriteManifest: return "WriteManifest";
			case ExportPhase::SmokeTest:     return "SmokeTest";
			case ExportPhase::MoveToOutput:  return "MoveToOutput";
			case ExportPhase::Done:          return "Done";
		}

		ENGINE_ASSERT(false, "Unknown ExportPhase {}", std::to_underlying(phase));
		return "Unknown";
	}

}
