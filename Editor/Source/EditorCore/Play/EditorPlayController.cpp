#include "EditorPCH.h"
#include "EditorCore/Play/EditorPlayController.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Session/PlaySession.h"

namespace Engine {

	struct EditorPlayController::State
	{
		EditorContext* Editor = nullptr; // documented back-reference: owns the controller
		Scope<PlaySession> Session;
	};

	EditorPlayController::EditorPlayController(EditorContext& editor)
		: m_State(CreateScope<State>())
	{
		m_State->Editor = &editor;
	}

	EditorPlayController::~EditorPlayController() = default;

	Status EditorPlayController::Start(const PlayStartOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "play mode is not implemented yet (M7 stream A)");
	}

	Status EditorPlayController::Stop()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::InvalidState, "not playing");
	}

	bool EditorPlayController::IsPlaying() const
	{
		return m_State->Session != nullptr;
	}

	PlaySession* EditorPlayController::GetSession() const
	{
		return m_State->Session.get();
	}

	void EditorPlayController::OnFixedStep()
	{
		ENGINE_CONTRACT_STUB();
	}

	void EditorPlayController::OnUpdate(const FrameTime& /*frame*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	double EditorPlayController::GetFrameTimeScale() const
	{
		ENGINE_CONTRACT_STUB();
		return 1.0;
	}

	bool EditorPlayController::IsFrameThrottleSuspended() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	std::optional<FrameLoopConfig> EditorPlayController::GetFrameLoopConfig() const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	void EditorPlayController::OnClientDisconnected(ClientId /*client*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::string EditorPlayController::GetPlayStateName() const
	{
		ENGINE_CONTRACT_STUB();
		return "Edit";
	}

	std::optional<uint64_t> EditorPlayController::GetTick() const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

}
