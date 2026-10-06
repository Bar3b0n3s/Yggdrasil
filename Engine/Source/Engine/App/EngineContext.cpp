#include "EnginePCH.h"
#include "Engine/App/EngineContext.h"

// M2 contract stub (Roadmap rule 3): stream D (application and frame loop) implements Create (the user:// mount and the
// window). Until then Create fails with Unsupported, so no context exists. The constructor is complete: it only constructs
// the infallible services.

namespace Engine {

	EngineContext::EngineContext(ConstructionKey /*key*/, const EngineContextSpecification& specification)
		: m_JobSystem(specification.WorkerCount, m_MainThreadQueue)
	{
	}

	EngineContext::~EngineContext() = default;

	Result<Scope<EngineContext>> EngineContext::Create(const EngineContextSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EngineContext::Create is not implemented yet");
	}

	std::string_view EngineContextStepToString(EngineContextStep /*step*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
