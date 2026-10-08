#include "EnginePCH.h"
#include "Engine/Physics/PhysicsEngine.h"

// M11 contract stubs (Docs/Decisions/0014-m11-decisions.md): stream A implements Jolt's process-level state. Initialize
// succeeds without doing anything, so ProcessContext's Physics step, which the contract wired, keeps every process
// starting until then.

namespace Engine {

	Status PhysicsEngine::Initialize(const PhysicsEngineSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void PhysicsEngine::Shutdown()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool PhysicsEngine::IsInitialized()
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	uint32_t PhysicsEngine::GetWorkerThreadCount()
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Status PhysicsEngine::SetWorkerThreadCount(uint32_t /*count*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the physics engine is not implemented yet (M11 stream A)");
	}

	uint32_t PhysicsEngine::GetLiveWorldCount()
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t PhysicsEngine::GetLiveBodyCount()
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
