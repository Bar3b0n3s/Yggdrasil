#include "EnginePCH.h"
#include "Engine/App/ProcessContext.h"

// M2 contract stub (Roadmap rule 3): stream D (application and frame loop) implements the process-level steps, their
// teardown, the fatal-error handler and its hook. Until then Create fails with Unsupported and no context exists.

namespace Engine {

	ProcessContext::ProcessContext(ConstructionKey /*key*/, ProcessContextSpecification specification)
		: m_Specification(std::move(specification))
	{
	}

	ProcessContext::~ProcessContext()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<ProcessContext>> ProcessContext::Create(const ProcessContextSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProcessContext::Create is not implemented yet");
	}

	ProcessContext* ProcessContext::GetCurrent()
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	void ProcessContext::SetFatalErrorHook(FatalErrorHook /*hook*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::string_view ProcessContextStepToString(ProcessContextStep /*step*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string_view GetBuildDescription()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
