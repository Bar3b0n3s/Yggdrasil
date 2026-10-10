#include "EnginePCH.h"
#include "Engine/Scripting/Watchdog.h"

namespace Engine {

	struct ScriptWatchdog::State
	{
	};

	ScriptWatchdog::ScriptWatchdog()
	{
		ENGINE_CONTRACT_STUB();
	}

	ScriptWatchdog::~ScriptWatchdog()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ScriptWatchdog::Enter(uint32_t budgetMs, double nowSeconds)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(budgetMs);
		static_cast<void>(nowSeconds);
		return MakeError(ErrorCode::Unsupported, "ScriptWatchdog is an M13 contract stub");
	}

	void ScriptWatchdog::Leave()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool ScriptWatchdog::CheckInterrupt(int gc, double nowSeconds) noexcept
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(gc);
		static_cast<void>(nowSeconds);
		return false;
	}

	uint32_t ScriptWatchdog::GetDepth() const noexcept
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	double ScriptWatchdog::GetDeadlineSeconds() const noexcept
	{
		ENGINE_CONTRACT_STUB();
		return 0.0;
	}

	Result<uint32_t> ScriptWatchdog::ResolveCallbackBudget(uint32_t configuredMs, bool isTestRun)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(configuredMs);
		static_cast<void>(isTestRun);
		return MakeError(ErrorCode::Unsupported, "ScriptWatchdog is an M13 contract stub");
	}

}
