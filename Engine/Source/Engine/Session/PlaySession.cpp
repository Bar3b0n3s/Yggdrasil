#include "EnginePCH.h"
#include "Engine/Session/PlaySession.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <utility>

namespace Engine {

	struct PlaySession::State
	{
		Scope<Scene> RuntimeScene;
		UUIDGenerator IdGenerator = UUIDGenerator::CreateDeterministic(0);
		Random Stream{ 0 };
		PlayInput Input;
		RenderSnapshot LastExtraction;
		ProjectSettings Project;
	};

	std::string_view PlaySessionPhaseToString(PlaySessionPhase phase)
	{
		switch (phase)
		{
			case PlaySessionPhase::InterpolationSnapshot:   return "InterpolationSnapshot";
			case PlaySessionPhase::ApplyInput:              return "ApplyInput";
			case PlaySessionPhase::StartFlush:              return "StartFlush";
			case PlaySessionPhase::FixedUpdate:             return "FixedUpdate";
			case PlaySessionPhase::Tasks:                   return "Tasks";
			case PlaySessionPhase::PreStepTransformUpdate:  return "PreStepTransformUpdate";
			case PlaySessionPhase::PhysicsPreStep:          return "PhysicsPreStep";
			case PlaySessionPhase::PhysicsStep:             return "PhysicsStep";
			case PlaySessionPhase::PhysicsPostStep:         return "PhysicsPostStep";
			case PlaySessionPhase::DestroyFlush:            return "DestroyFlush";
			case PlaySessionPhase::PostStepTransformUpdate: return "PostStepTransformUpdate";
			case PlaySessionPhase::LatchFrame:              return "LatchFrame";
			case PlaySessionPhase::FrameStartFlush:         return "FrameStartFlush";
			case PlaySessionPhase::Update:                  return "Update";
			case PlaySessionPhase::LateUpdate:              return "LateUpdate";
			case PlaySessionPhase::FrameDestroyFlush:       return "FrameDestroyFlush";
			case PlaySessionPhase::FrameTransformUpdate:    return "FrameTransformUpdate";
			case PlaySessionPhase::AudioUpdate:             return "AudioUpdate";
			case PlaySessionPhase::RenderExtraction:        return "RenderExtraction";
		}

		ENGINE_CORE_ASSERT(false, "Unknown PlaySessionPhase {}", std::to_underlying(phase));
		return "Unknown";
	}

	PlaySession::PlaySession(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	PlaySession::~PlaySession() = default;

	Result<Scope<PlaySession>> PlaySession::Create(const PlaySessionSpecification& /*specification*/, const Json& /*sceneDocument*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "play sessions are not implemented yet (M7 stream A)");
	}

	Result<Scope<PlaySession>> PlaySession::CreateFromScene(const PlaySessionSpecification& /*specification*/, const Scene& /*editScene*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "play sessions are not implemented yet (M7 stream A)");
	}

	void PlaySession::FixedStep()
	{
		ENGINE_CONTRACT_STUB();
	}

	void PlaySession::FrameUpdate(const FrameTime& /*frame*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PlaySession::Tick()
	{
		ENGINE_CONTRACT_STUB();
	}

	void PlaySession::AdvanceLoopStep()
	{
		ENGINE_CONTRACT_STUB();
	}

	void PlaySession::AdvanceLoopFrame(const FrameTime& /*frame*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Scene& PlaySession::GetScene()
	{
		ENGINE_CONTRACT_STUB();
		ENGINE_CORE_ASSERT(m_State->RuntimeScene != nullptr, "the play session has no scene");
		return *m_State->RuntimeScene;
	}

	const Scene& PlaySession::GetScene() const
	{
		ENGINE_CONTRACT_STUB();
		ENGINE_CORE_ASSERT(m_State->RuntimeScene != nullptr, "the play session has no scene");
		return *m_State->RuntimeScene;
	}

	PlayMode PlaySession::GetMode() const
	{
		ENGINE_CONTRACT_STUB();
		return PlayMode::Play;
	}

	uint64_t PlaySession::GetTick() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	double PlaySession::GetFixedDelta() const
	{
		ENGINE_CONTRACT_STUB();
		return 1.0 / 60.0;
	}

	const ProjectSettings& PlaySession::GetProjectSettings() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Project;
	}

	uint64_t PlaySession::GetSeed() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Random& PlaySession::GetRandom()
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Stream;
	}

	UUIDGenerator& PlaySession::GetIdGenerator()
	{
		ENGINE_CONTRACT_STUB();
		return m_State->IdGenerator;
	}

	PlayInput& PlaySession::GetInput()
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Input;
	}

	const PlayInput& PlaySession::GetInput() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Input;
	}

	uint64_t PlaySession::ComputeStateHash() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t PlaySession::GetMaxEntities() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Status PlaySession::CheckEntityCapacity(size_t /*additional*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the play session's entity cap is not implemented yet (M7 stream A)");
	}

	Result<Entity> PlaySession::CreateEntity(std::string_view /*name*/, Entity /*parent*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "runtime spawns are not implemented yet (M7 stream A)");
	}

	Result<Entity> PlaySession::CreateEntity(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "runtime spawns are not implemented yet (M7 stream A)");
	}

	void PlaySession::MarkTeleported(Entity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool PlaySession::IsPaused() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void PlaySession::SetPaused(bool /*paused*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	double PlaySession::GetTimeScale() const
	{
		ENGINE_CONTRACT_STUB();
		return 1.0;
	}

	Status PlaySession::SetTimeScale(double /*timeScale*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the play session's time scale is not implemented yet (M7 stream A)");
	}

	bool PlaySession::IsLockstep() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	ClientId PlaySession::GetLockstepOwner() const
	{
		ENGINE_CONTRACT_STUB();
		return NoClient;
	}

	void PlaySession::SetLockstep(bool /*lockstep*/, ClientId /*owner*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool PlaySession::IsStepping() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void PlaySession::SetStepping(bool /*stepping*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool PlaySession::IsModified() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void PlaySession::MarkModified()
	{
		ENGINE_CONTRACT_STUB();
	}

	void PlaySession::SetViewSize(uint32_t /*width*/, uint32_t /*height*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PlaySession::SetExtractionEnabled(bool /*enabled*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool PlaySession::IsExtractionEnabled() const
	{
		ENGINE_CONTRACT_STUB();
		return true;
	}

	const RenderSnapshot& PlaySession::GetLastExtraction() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->LastExtraction;
	}

	uint64_t PlaySession::GetExtractionCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Result<RenderSnapshot> PlaySession::ExtractView(const RenderExtractionRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "views of a play session are not implemented yet (M7 stream A)");
	}

	float PlaySession::GetViewAlpha() const
	{
		ENGINE_CONTRACT_STUB();
		return 1.0f;
	}

}
