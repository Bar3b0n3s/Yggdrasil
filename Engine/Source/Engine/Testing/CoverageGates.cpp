#include "EnginePCH.h"
#include "Engine/Testing/CoverageGates.h"

#include "Engine/Core/Error.h"

namespace Engine {

	std::string_view CoverageGateName(CoverageGate /*gate*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<CoverageGate> ParseCoverageGateName(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	bool IsPerModeCoverageGate(CoverageGate /*gate*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	std::optional<CoverageGate> GetCoverageGateOfKind(std::string_view /*kind*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::vector<TestSuiteSettings::Mode> ToTestModes(RunModes /*modes*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	RunModes ToRunModes(TestSuiteSettings::Mode /*mode*/)
	{
		ENGINE_CONTRACT_STUB();
		return RunModes::None;
	}

	Result<std::vector<TestCoverageGateResult>> EvaluateCoverageGates(std::span<const TestCoverageCounter> /*counters*/,
		TestSuiteSettings::Mode /*mode*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M14 coverage gate evaluation contract"));
	}

	std::vector<TestCoverageGateResult> MakeUnevaluatedCoverageGates()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<std::vector<TestCoverageCounter>> MakeScriptApiCoverageCounters(const ScriptApiCoverage& /*current*/,
		const ScriptApiCoverage& /*baseline*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M14 script API coverage counter contract"));
	}

	struct SceneCoverageRecorder::State
	{
	};

	SceneCoverageRecorder::SceneCoverageRecorder(const TypeRegistry& /*types*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	SceneCoverageRecorder::~SceneCoverageRecorder() = default;

	Status SceneCoverageRecorder::Record(const Scene& /*scene*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M14 scene coverage contract"));
	}

	std::vector<TestCoverageCounter> SceneCoverageRecorder::GetCounters() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void SceneCoverageRecorder::Reset()
	{
		ENGINE_CONTRACT_STUB();
	}

	struct FormatCoverageRecorder::State
	{
	};

	FormatCoverageRecorder::FormatCoverageRecorder()
	{
		ENGINE_CONTRACT_STUB();
	}

	FormatCoverageRecorder::~FormatCoverageRecorder() = default;

	void FormatCoverageRecorder::RecordLoad(const AssetManager& /*assets*/, AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<TestCoverageCounter> FormatCoverageRecorder::GetCounters() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void FormatCoverageRecorder::Reset()
	{
		ENGINE_CONTRACT_STUB();
	}

}
