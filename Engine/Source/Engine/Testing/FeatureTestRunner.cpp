#include "EnginePCH.h"
#include "Engine/Testing/FeatureTestRunner.h"

#include "Engine/Core/Error.h"

namespace Engine {

	struct FeatureTestRunner::State
	{
	};

	FeatureTestRunner::FeatureTestRunner()
	{
		ENGINE_CONTRACT_STUB();
	}

	FeatureTestRunner::~FeatureTestRunner()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<TestInventory> FeatureTestRunner::Discover(IFeatureTestHost& /*host*/, TestSuiteSettings::Mode /*mode*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 test inventory contract"));
	}

	Status FeatureTestRunner::Begin(IFeatureTestHost& /*host*/, const FeatureTestRunOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 test runner start contract"));
	}

	Result<bool> FeatureTestRunner::Advance()
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 test runner advancement contract"));
	}

	Result<TestRunResult> FeatureTestRunner::GetResult() const
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 test runner result contract"));
	}

	Result<ReplayDocument> FeatureTestRunner::TakeRecording()
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 test runner recording contract"));
	}

	void FeatureTestRunner::Cancel()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool FeatureTestRunner::IsRunning() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	std::string FeatureTestRunner::GetPhase() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
