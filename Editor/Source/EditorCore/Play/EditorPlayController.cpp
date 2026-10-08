#include "EditorPCH.h"
#include "EditorCore/Play/EditorPlayController.h"

#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Automation/Methods/PlayMethods.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"

#include <cmath>
#include <format>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		// play.start's "scene" (the path rules of EditorMethodContext::ResolveProjectPath, §13.2 "Paths"): a project-relative
		// path or a project:// path to a .scene file. Errors: InvalidArgument located at /scene.
		static Result<VfsPath> ResolvePlayScenePath(std::string_view path)
		{
			const auto locate = [](std::string message, std::string hint)
			{
				ErrorLocation location;
				location.JsonPointer = "/scene";
				ErrorIssue issue{ .JsonPointer = "/scene", .Message = message, .Hint = hint, .Suggestions = {} };
				return std::unexpected(
					Error(ErrorCode::InvalidArgument, std::move(message)).WithHint(std::move(hint)).WithLocation(std::move(location)).WithIssue(std::move(issue)));
			};
			constexpr std::string_view SchemeSeparator = "://";
			Result<VfsPath> parsed = path.find(SchemeSeparator) != std::string_view::npos ? VfsPath::Parse(path) : VfsPath::Create("project", path);
			if (!parsed)
			{
				return locate(std::format("'{}' is not a valid project path: {}", path, parsed.error().GetMessageText()),
					"give a project-relative path with '/' separators, such as \"Assets/Scenes/Main.scene\"");
			}
			if (parsed->GetScheme() != "project")
				return locate(std::format("'{}' is outside the project: only project:// paths are accepted", path), "give a project-relative path");
			if (parsed->IsRoot() || parsed->GetExtension() != ".scene")
				return locate(std::format("'{}' does not name a '.scene' file", path), "name a scene such as \"Assets/Scenes/Level2.scene\"");
			return std::move(*parsed);
		}

		// The scene document's Seed, for the session seed; 0 when it has none (the strict load then reports the document).
		static uint32_t ReadSceneSeed(const Json& document)
		{
			const std::optional<JsonReader> seed = JsonReader(document).FindMember("Seed");
			return seed.has_value() ? seed->ReadUInt32().value_or(0u) : 0u;
		}

	}

	struct EditorPlayController::State
	{
		EditorContext* Editor = nullptr; // documented back-reference: owns the controller
		Scope<PlaySession> Session;
		// The serial of the next session (PlaySessionSpecification::Serial): sessions are counted from 1 and never share one.
		uint64_t NextSerial = 1;
		// True while this controller holds the asset manager's reload deferral (§7.5 race rule 4): from the start of a lockstep
		// session to its end.
		bool DefersReloads = false;

		// Appends PlayStateChanged with the current state (§4.9).
		void AppendStateChanged();
		// Destroys the session and gives back what it held: deferred reloads are applied, and the selection is restored by
		// UUID (§5.6: ids that are entities of the edit scene stay selected).
		void EndSession();
	};

	void EditorPlayController::State::AppendStateChanged()
	{
		// Through the editor, which suppresses events during a dry run.
		Editor->AppendEvent(Utils::MakePlayStateChangedEvent(Session.get()));
	}

	void EditorPlayController::State::EndSession()
	{
		Session.reset();
		if (DefersReloads)
		{
			// Destroyed first, so the reloads held during lockstep reach the edit state only.
			Editor->GetAssets().SetReloadsDeferred(false);
			DefersReloads = false;
		}
		const std::span<const UUID> selection = Editor->GetSelection();
		Editor->SetSelection(std::vector<UUID>(selection.begin(), selection.end()));
	}

	EditorPlayController::EditorPlayController(EditorContext& editor)
		: m_State(CreateScope<State>())
	{
		m_State->Editor = &editor;
	}

	EditorPlayController::~EditorPlayController()
	{
		if (m_State->Session != nullptr)
			m_State->EndSession();
	}

	Status EditorPlayController::Start(const PlayStartOptions& options)
	{
		State& state = *m_State;
		EditorContext& editor = *state.Editor;
		if (state.Session != nullptr)
			return std::unexpected(Error(ErrorCode::InvalidState, "already playing: a play session runs").WithHint("stop it first with play.stop"));
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "no project open; call project.create or project.open");
		if (options.ScenePath.empty() && !editor.HasScene())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "no scene open to play")
					.WithHint("open one with scene.open {path}, create one with scene.new {path}, or name one with play.start {scene}"));
		}
		if (editor.GetAssets().HasErrorDiagnostics())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "the project has asset errors: fix them first (§7.2: they block play)")
					.WithHint("project.validate lists them with their fixes"));
		}
		if (!std::isfinite(options.TimeScale) || options.TimeScale < 0.0 || options.TimeScale > PlaySession::MaxTimeScale)
		{
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("the time scale must be 0 to {} (got {})", PlaySession::MaxTimeScale, options.TimeScale)));
		}

		// The serializer copy of §5.6: the open edit scene, or the named scene file, as one canonical document.
		Json document;
		std::string sceneName;
		if (options.ScenePath.empty())
		{
			Result<Json> copied = SceneSerializer::ToJson(editor.GetScene());
			if (!copied)
				return std::unexpected(std::move(copied).error().WithContext("while copying the edit scene for play"));
			document = std::move(*copied);
			sceneName = editor.GetScene().GetName();
		}
		else
		{
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolvePlayScenePath(options.ScenePath));
			Result<std::string> text = editor.GetVfs().ReadText(path);
			if (!text)
				return std::unexpected(std::move(text).error().WithContext(std::format("while reading the scene '{}' to play", path.GetPath())));
			Result<Json> parsed = JsonReader::Parse(*text);
			if (!parsed)
				return std::unexpected(std::move(parsed).error().WithContext(std::format("while reading the scene '{}' to play", path.GetPath())));
			document = std::move(*parsed);
			sceneName = std::string(path.GetPath());
		}

		const ProjectSettings& project = editor.GetProject().GetSettings();
		PlaySessionSpecification specification;
		specification.Registry = &editor.GetTypeRegistry();
		specification.Assets = &editor.GetAssets();
		specification.Mode = options.Mode;
		specification.Seed = options.Seed.value_or(PlaySession::ComputeSessionSeed(project.Simulation.Seed, Utils::ReadSceneSeed(document)));
		specification.Project = project;
		// The game view at the game's window size until the editor's game viewport sets its own (M10).
		specification.ViewWidth = project.Window.Width;
		specification.ViewHeight = project.Window.Height;
		specification.Serial = state.NextSerial;
		ENGINE_TRY_ASSIGN(Scope<PlaySession> session, PlaySession::Create(specification, document));
		++state.NextSerial;

		session->SetPaused(options.Paused);
		ENGINE_TRY(session->SetTimeScale(options.TimeScale));
		if (options.Lockstep)
		{
			session->SetLockstep(true, options.LockstepOwner);
			// §7.5 race rule 4: reloads wait until the lockstep session ends, so an agent's stepped run sees fixed assets.
			EditorAssetManager& assets = editor.GetAssets();
			if (!assets.AreReloadsDeferred())
			{
				assets.SetReloadsDeferred(true);
				state.DefersReloads = true;
			}
		}
		state.Session = std::move(session);
		state.AppendStateChanged();
		ENGINE_INFO("Play session started: {} of '{}', seed {}{}{}", options.Mode == PlayMode::Simulate ? "Simulate" : "Play", sceneName,
			state.Session->GetSeed(), options.Lockstep ? ", lockstep" : "", options.Paused ? ", paused" : "");
		return {};
	}

	Status EditorPlayController::Stop()
	{
		State& state = *m_State;
		if (state.Session == nullptr)
			return std::unexpected(Error(ErrorCode::InvalidState, "not playing: there is no play session").WithHint("start one with play.start"));
		const uint64_t tick = state.Session->GetTick();
		state.EndSession();
		state.AppendStateChanged();
		ENGINE_INFO("Play session stopped after {} ticks", tick);
		return {};
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
		if (m_State->Session != nullptr)
			m_State->Session->AdvanceLoopStep();
	}

	void EditorPlayController::OnUpdate(const FrameTime& frame)
	{
		if (m_State->Session != nullptr)
			m_State->Session->AdvanceLoopFrame(frame);
	}

	double EditorPlayController::GetFrameTimeScale() const
	{
		return m_State->Session != nullptr ? m_State->Session->GetTimeScale() : 1.0;
	}

	bool EditorPlayController::IsFrameThrottleSuspended() const
	{
		return m_State->Session != nullptr && m_State->Session->IsStepping();
	}

	std::optional<FrameLoopConfig> EditorPlayController::GetFrameLoopConfig() const
	{
		if (m_State->Session == nullptr)
			return std::nullopt;
		const SimulationSettings& simulation = m_State->Session->GetProjectSettings().Simulation;
		FrameLoopConfig config;
		config.FixedHz = simulation.FixedHz;
		config.MaxStepsPerFrame = simulation.MaxStepsPerFrame;
		return config;
	}

	void EditorPlayController::OnClientDisconnected(ClientId client)
	{
		State& state = *m_State;
		PlaySession* session = state.Session.get();
		if (session == nullptr)
			return;
		// §13.2: the session never stays locked in "Agent controls time".
		const Utils::DisconnectedLockstep released = Utils::ReleaseDisconnectedLockstep(*session, client);
		if (!released.Released)
			return;
		if (released.StateChanged)
			state.AppendStateChanged();
		ENGINE_INFO("The lockstep owner (client {}) disconnected: lockstep released and play paused at tick {}", client, session->GetTick());
	}

	std::string EditorPlayController::GetPlayStateName() const
	{
		return std::string(PlayRunStateToString(GetPlayRunState(m_State->Session.get())));
	}

	std::optional<uint64_t> EditorPlayController::GetTick() const
	{
		if (m_State->Session == nullptr)
			return std::nullopt;
		return m_State->Session->GetTick();
	}

}
