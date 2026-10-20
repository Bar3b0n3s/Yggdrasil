#include "EditorPCH.h"
#include "EditorCore/Play/EditorPlayController.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "EditorCore/Viewport/EditorViewportState.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/AssetDependencyGraph.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Automation/Methods/PlayMethods.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Window.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayRecorder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		using MixerVolumes = std::array<float, AudioGroupCount>;

		static MixerVolumes ReadMixerVolumes(AudioEngine& audio)
		{
			MixerVolumes values{};
			for (uint32_t index = 0; index < AudioGroupCount; ++index)
				values[index] = audio.GetGroupVolume(static_cast<AudioGroup>(index));
			return values;
		}

		static void RestoreMixerVolumes(AudioEngine& audio, const MixerVolumes& values)
		{
			for (uint32_t index = 0; index < AudioGroupCount; ++index)
			{
				const Status restored = audio.SetGroupVolume(static_cast<AudioGroup>(index), values[index]);
				ENGINE_VERIFY(restored.has_value(), "restoring a previously valid mixer volume must succeed");
			}
		}

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

	struct EditorPlayController::State final : IPlaySessionHost
	{
		EditorContext* Editor = nullptr; // documented back-reference: owns the controller
		Scope<PlaySession> Session;
		ReplayHeader Header{};
		std::optional<Utils::MixerVolumes> MixerBaseline{};
		std::optional<CursorMode> CursorBaseline{};
		CursorMode SavedTestCursor = CursorMode::Normal;
		std::set<AssetHandle> PendingScriptReloads{};
		bool ScriptReloadsArrived = false; // a script publication was queued since the last safe point
		uint64_t ImportEventCursor = 0;
		ScriptErrorStream Errors{};
		ScriptEnvironment Environment{ .IsEditor = true, .Version = std::string(EngineVersionString) };
		uint64_t ReplayInputSerial = 0;
		ClientId RecordingOwner = NoClient;
		EditorFeatureTestHost* TestOwner = nullptr; // borrowed exclusive lease, released before controller destruction
		PlaySession* TestSession = nullptr;         // runner-owned, unpublished before destruction
		bool SessionViewFixed = false;              // Session keeps its deterministic view (PreparedSession::FixedView, SyncGameView)
		bool SavedPaused = false;
		bool SavedLockstep = false;
		ClientId SavedOwner = NoClient;
		bool TestDefersReloads = false;
		std::vector<UUID> SavedSelection{};
		SceneTarget SavedSelectionTarget = SceneTarget::Edit;
		Watchdog* PhaseMarker = nullptr; // documented back-reference: the automation server's watchdog, cleared before it goes
		std::string PhaseText{};         // reused for every script phase, so reporting one does not allocate

		ScriptEnvironment GetScriptEnvironment() const override
		{
			ScriptEnvironment result = Environment;
#if defined(ENGINE_PLATFORM_WINDOWS)
			result.Platform = "Windows";
#elif defined(ENGINE_PLATFORM_LINUX)
			result.Platform = "Linux";
#elif defined(ENGINE_PLATFORM_MACOS)
			result.Platform = "macOS";
#endif
			if (const Window* window = Editor->GetEngine().GetWindow())
			{
				result.IsHeadless = GlfwLibrary::GetMode() == WindowMode::Headless;
				result.IsFocused = window->IsFocused();
			}
			// Scripts measure the game view, never the editor window (PlaySession.h, view coordinates); a session replaces
			// this with its own view: the Game panel's image for ordinary play (SyncGameView), the fixed deterministic view
			// otherwise.
			result.WindowSize = glm::vec2(GetGameViewSize(Editor->HasProject() ? Editor->GetProject().GetSettings().Window : WindowSettings{}));
			return result;
		}
		// The editor's game view: the Game panel's rendered image when it is available, otherwise the project's window
		// size, which a session keeps until the panel publishes an image.
		glm::uvec2 GetGameViewSize(const WindowSettings& window) const
		{
			if (const Result<glm::uvec2> size = Editor->GetViewportState().GetPixelSize(ViewportView::Game))
				return *size;
			return { std::max(window.Width, 1u), std::max(window.Height, 1u) };
		}
		Status SetScriptCursorMode(CursorMode mode) override
		{
			Window* window = Editor->GetEngine().GetWindow();
			if (window == nullptr)
				return MakeError(ErrorCode::Unsupported, "the editor context has no window");
			window->SetCursorMode(mode);
			return {};
		}
		CursorMode GetScriptCursorMode() const override
		{
			const Window* window = Editor->GetEngine().GetWindow();
			return window != nullptr ? window->GetCursorMode() : CursorMode::Normal;
		}
		void OnScriptError(const ScriptError& error, bool /*fatal*/) override
		{
			// The script engine logs the occurrence once; the editor only retains history and publishes the notification,
			// with the bounded message the history keeps.
			const ScriptError& retained = Errors.Add(error);
			Editor->AppendEvent({ .Tick = retained.Tick, .Type = EngineEventType::ScriptErrorRaised, .Id = retained.Entity, .Path = retained.Script, .Name = {}, .Message = retained.Message });
		}
		void OnScriptPhase(std::string_view script, std::string_view callback) override
		{
			if (PhaseMarker == nullptr)
				return;
			if (script.empty() && callback.empty())
			{
				PhaseMarker->SetScriptPhase({});
				return;
			}
			FormatScriptPhase(PhaseText, script, callback);
			PhaseMarker->SetScriptPhase(PhaseText);
		}
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
		// A selection of a runtime scene that Scene.Load replaced is cleared while the session can still tell.
		Editor->PruneSelection();
		Session.reset();
		if (CursorBaseline)
		{
			if (Window* window = Editor->GetEngine().GetWindow())
				window->SetCursorMode(*CursorBaseline);
			CursorBaseline.reset();
		}
		PendingScriptReloads.clear();
		if (MixerBaseline)
		{
			Utils::RestoreMixerVolumes(*Editor->GetEngine().GetAudioEngine(), *MixerBaseline);
			MixerBaseline.reset();
		}
		ReplayInputSerial = 0;
		RecordingOwner = NoClient;
		SessionViewFixed = false;
		Header = {};
		if (DefersReloads)
		{
			// Destroyed first, so the reloads held during lockstep reach the edit state only.
			Editor->GetAssets().SetReloadsDeferred(false);
			DefersReloads = false;
			// Releasing deferral schedules imports. Publish the whole dependency chain before a new Play can snapshot it.
			Editor->GetAssets().WaitIdle();
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

	Result<EditorPlayController::PreparedSession> EditorPlayController::PrepareSession(const PlayStartOptions& options, ProjectSettings project,
		bool emptyScene, IPlaySessionTestHook* hook, IScriptTestHost* testHost, bool ownsAudio, bool scratch, ScriptApiRegistry* api, bool activate,
		bool fixedView)
	{
		EditorContext& editor = *m_State->Editor;
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "no project is open");
		if (!std::isfinite(options.TimeScale) || options.TimeScale < 0.0 || options.TimeScale > PlaySession::MaxTimeScale)
			return MakeError(ErrorCode::InvalidArgument, "timeScale is outside the supported range");
		Json document;
		std::optional<VfsPath> scenePath;
		if (emptyScene)
		{
			auto ids = UUIDGenerator::CreateDeterministic(0);
			auto scene = Scene::Create({ .Name = "Test", .Registry = &editor.GetTypeRegistry(), .IdGenerator = &ids });
			ENGINE_TRY_ASSIGN(document, SceneSerializer::ToJson(*scene));
		}
		else if (options.ScenePath.empty())
		{
			if (!editor.HasScene())
				return MakeError(ErrorCode::InvalidState, "no scene is open");
			ENGINE_TRY_ASSIGN(document, SceneSerializer::ToJson(editor.GetScene()));
			scenePath = editor.GetScenePath();
		}
		else
		{
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolvePlayScenePath(options.ScenePath));
			ENGINE_TRY_ASSIGN(const std::string text, editor.GetVfs().ReadText(path));
			ENGINE_TRY_ASSIGN(document, JsonReader::Parse(text));
			scenePath = path;
		}
		if (options.PauseOnError.has_value())
			project.Scripting.PauseOnError = *options.PauseOnError;
		if (api == nullptr)
		{
			ENGINE_TRY_ASSIGN(api, GetScriptApi());
		}
		// The scene asset the played document came from (Scene.GetCurrent; M20, Docs/Decisions/0031-m20-contract.md decision
		// 5): the open scene's or play.start's file, none for an empty test scene or an open scene never saved.
		AssetHandle sceneAsset;
		if (scenePath.has_value())
		{
			if (const AssetRecord* record = editor.GetAssets().GetRegistry().FindBySourcePath(*scenePath))
				sceneAsset = record->Metadata.Handle;
		}
		PlaySessionSpecification specification;
		specification.Registry = &editor.GetTypeRegistry();
		specification.Assets = &editor.GetAssets();
		specification.SceneAsset = sceneAsset;
		specification.Mode = options.Mode;
		specification.Seed = options.Seed.value_or(PlaySession::ComputeSessionSeed(project.Simulation.Seed, Utils::ReadSceneSeed(document)));
		specification.Project = project;
		// A deterministic session (a test run's suites and replays, recording, replay, lockstep) keeps the project's window
		// size for its whole life, as the Runtime's do and as test screenshots are taken, so its results and hashes never
		// depend on the panel; ordinary live play starts with the Game view's image, which SyncGameView keeps it at.
		fixedView = fixedView || testHost != nullptr || scratch || options.Lockstep;
		const glm::uvec2 view = fixedView ? GetDeterministicViewSize(project.Window) : m_State->GetGameViewSize(project.Window);
		specification.ViewWidth = view.x;
		specification.ViewHeight = view.y;
		specification.Serial = m_State->NextSerial++;
		specification.Audio = scratch ? nullptr : editor.GetEngine().GetAudioEngine();
		specification.OwnsAudioTime = ownsAudio;
		specification.ScriptApi = api;
		ENGINE_TRY_ASSIGN(specification.ScriptSchemas, editor.GetScriptSchemaSnapshot());
		specification.Parameters = options.Parameters;
		specification.Environment = m_State->GetScriptEnvironment();
		specification.ScriptRunMode = RunModes::Editor;
		specification.TestMode = testHost != nullptr;
		specification.TestHook = hook;
		specification.TestHost = testHost;
		specification.Host = scratch ? nullptr : m_State.get();
		PreparedSession result;
		result.FixedView = fixedView;
		result.Header.Parameters = options.Parameters.Get().is_null() ? VariantValue(Json::object()) : options.Parameters;
		result.Header.Seed = specification.Seed;
		result.Header.FixedHz = project.Simulation.FixedHz;
		result.Header.EngineVersion = std::string(EngineVersionString);
		// The run mode that recorded it, never the build configuration: a Debug and a Release editor record the same bytes
		// (§9.1, ADR 0022 "Integration").
		result.Header.Config = "Editor";
		if (scenePath.has_value())
		{
			result.Header.Scene.Path = std::string(scenePath->GetPath());
			result.Header.Scene.Handle = sceneAsset;
		}
		// The session keeps the played document as its start document (Scene.Reload) without another copy.
		ENGINE_TRY_ASSIGN(result.Session, PlaySession::Prepare(specification, CreateRef<const Json>(std::move(document))));
		ENGINE_TRY(result.Session->SetTimeScale(options.TimeScale));
		if (options.Lockstep)
			result.Session->SetLockstep(true, options.LockstepOwner);
		if (!activate)
			result.Session->SetPaused(options.Paused);
		if (activate)
		{
			result.Session->Activate();
			result.PausedDuringStartup = result.Session->IsPaused();
			result.Session->SetPaused(options.Paused || result.PausedDuringStartup);
		}
		return result;
	}

	Status EditorPlayController::ValidatePlayRequest(const PlayStartOptions& options) const
	{
		EditorContext& editor = *m_State->Editor;
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "no project is open");
		if (!std::isfinite(options.TimeScale) || options.TimeScale < 0.0 || options.TimeScale > PlaySession::MaxTimeScale)
			return MakeError(ErrorCode::InvalidArgument, "timeScale is outside the supported range");
		if (!options.Parameters.IsNull() && !options.Parameters.Get().is_object())
			return MakeError(ErrorCode::InvalidArgument, "play parameters must be an object");
		if (options.ScenePath.empty())
		{
			if (!editor.HasScene())
				return MakeError(ErrorCode::InvalidState, "no scene is open");
			return {};
		}
		ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolvePlayScenePath(options.ScenePath));
		ENGINE_TRY_ASSIGN(const std::string text, editor.GetVfs().ReadText(path));
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		static_cast<void>(document);
		return {};
	}

	Status EditorPlayController::CheckPlayScripts(const ProjectSettings& project)
	{
		if (!project.Scripting.BlockPlayOnTypeErrors)
			return {};
		ENGINE_TRY_ASSIGN(EditorScriptService * scripts, m_State->Editor->AcquireScriptService());
		ENGINE_TRY_ASSIGN(const auto checked, scripts->Check({}));
		for (const ScriptDiagnostic& diagnostic : checked.Diagnostics)
		{
			if (diagnostic.Severity == DiagnosticSeverity::Error)
				return std::unexpected(Error(ErrorCode::Validation, std::format("script errors block Play: {}", diagnostic.Message))
						.WithLocation({ .File = diagnostic.File, .Line = diagnostic.Line, .Column = diagnostic.Column, .JsonPointer = std::nullopt, .Entity = {} })
						.WithHint("fix script.check diagnostics or disable Scripting.BlockPlayOnTypeErrors"));
		}
		return {};
	}

	Status EditorPlayController::Start(const PlayStartOptions& options)
	{
		State& state = *m_State;
		EditorContext& editor = *state.Editor;
		if (state.TestOwner != nullptr || state.Session != nullptr)
			return MakeError(ErrorCode::InvalidState, "a play or test session is already active");
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "no project is open");
		if (editor.GetAssets().HasErrorDiagnostics())
			return std::unexpected(Error(ErrorCode::InvalidState, "the project has asset errors: fix them first").WithHint("project.validate lists the failures"));
		ENGINE_TRY(ValidatePlayRequest(options));
		ENGINE_TRY(CheckPlayScripts(editor.GetProject().GetSettings()));
		ENGINE_TRY(editor.PrepareForPlay());
		std::optional<Utils::MixerVolumes> mixer;
		if (AudioEngine* audio = editor.GetEngine().GetAudioEngine())
			mixer = Utils::ReadMixerVolumes(*audio);
		const CursorMode cursor = state.GetScriptCursorMode();
		ENGINE_TRY_ASSIGN(PreparedSession prepared, PrepareSession(options, editor.GetProject().GetSettings()));
		state.CursorBaseline = cursor;
		state.PendingScriptReloads.clear();
		state.ImportEventCursor = editor.GetEngine().GetEventLog().GetNextSeq();
		state.MixerBaseline = mixer;
		state.Header = std::move(prepared.Header);
		state.Session = std::move(prepared.Session);
		state.SessionViewFixed = prepared.FixedView;
		if (options.Lockstep && !editor.GetAssets().AreReloadsDeferred())
		{
			editor.GetAssets().SetReloadsDeferred(true);
			state.DefersReloads = true;
		}
		// The selection follows into the runtime copy by UUID, so panels inspect and edit the running scene (§5.6).
		editor.RetargetSelectionToPlay();
		state.AppendStateChanged();
		return {};
	}

	Status EditorPlayController::Stop()
	{
		State& state = *m_State;
		if (state.TestOwner != nullptr)
			return MakeError(ErrorCode::InvalidState, "a test run owns the play session");
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
		return GetSession() != nullptr;
	}

	PlaySession* EditorPlayController::GetSession() const
	{
		return m_State->TestOwner != nullptr ? m_State->TestSession : m_State->Session.get();
	}

	void EditorPlayController::OnFixedStep()
	{
		if (m_State->TestOwner == nullptr && m_State->Session != nullptr)
			m_State->Session->AdvanceLoopStep();
	}

	void EditorPlayController::OnUpdate(const FrameTime& frame)
	{
		if (m_State->TestOwner == nullptr && m_State->Session != nullptr)
			m_State->Session->AdvanceLoopFrame(frame);
	}

	void EditorPlayController::SyncGameView()
	{
		// A test run's sessions and every other deterministic session keep their fixed view (PrepareSession).
		State& state = *m_State;
		if (state.TestOwner != nullptr || state.Session == nullptr || state.SessionViewFixed)
			return;
		if (const Result<glm::uvec2> size = state.Editor->GetViewportState().GetPixelSize(ViewportView::Game))
			state.Session->SetViewSize(size->x, size->y);
	}

	Status EditorPlayController::CheckReloadTypes(AssetHandle script)
	{
		State& state = *m_State;
		PlaySession* session = GetSession();
		ScriptEngine* scripts = session != nullptr ? session->GetScripts() : nullptr;
		if (scripts == nullptr || !state.Editor->HasProject())
			return {};
		// Type errors always block test runs (§11.9); an ordinary session follows the project's setting.
		if (!scripts->IsTestMode() && !state.Editor->GetProject().GetSettings().Scripting.BlockPlayOnTypeErrors)
			return {};
		// A chain error is Reload's to report, with the running code kept.
		const Result<std::vector<AssetHandle>> chain = scripts->GetReloadChain(script);
		if (!chain)
			return {};
		const EditorAssetManager& assets = state.Editor->GetAssets();
		for (const AssetHandle member : *chain)
		{
			const Result<ScriptImportCheck> check = assets.GetScriptCheck(member);
			if (!check || !check->Performed)
				continue;
			const auto finding = std::ranges::find_if(check->Diagnostics, [](const ScriptDiagnostic& diagnostic)
			{
				return diagnostic.Code == "SCRIPT_TYPE_ERROR" && diagnostic.Severity == DiagnosticSeverity::Error;
			});
			if (finding == check->Diagnostics.end())
				continue;
			ScriptError error;
			error.Kind = ScriptErrorKind::Type;
			error.Script = finding->File;
			error.Line = finding->Line;
			error.Column = finding->Column;
			error.Message = finding->Message;
			error.Callback = "Reload";
			error.Tick = session->GetTick();
			state.OnScriptError(error, false);
			return std::unexpected(Error(ErrorCode::Script, std::format("type errors block the reload, so the running code is kept: {}", finding->Message))
					.WithLocation({ .File = finding->File, .Line = finding->Line, .Column = finding->Column, .JsonPointer = std::nullopt, .Entity = {} })
					.WithHint(scripts->IsTestMode() ? std::string("fix the script's type errors (script.check); type errors always block test runs")
													: std::string("fix the script's type errors (script.check) or disable Scripting.BlockPlayOnTypeErrors")));
		}
		return {};
	}

	void EditorPlayController::OnAssetReload(AssetHandle source)
	{
		if (PlaySession* session = GetSession())
			session->MarkModified();
		if (m_State->TestOwner != nullptr || m_State->Session == nullptr)
			return;
		const AssetRecord* record = m_State->Editor->GetAssets().GetRegistry().Find(source);
		if (record != nullptr && record->Metadata.Type == AssetType::Script)
		{
			m_State->PendingScriptReloads.insert(source);
			m_State->ScriptReloadsArrived = true;
		}
	}

	void EditorPlayController::OnSafePoint()
	{
		State& state = *m_State;
		PlaySession* session = state.Session.get();
		if (state.Editor->IsDryRun() || state.TestOwner != nullptr || session == nullptr
			|| session->IsLockstep() || session->IsStepping() || state.ReplayInputSerial != 0
			|| (session->GetRecorder() != nullptr && session->GetRecorder()->IsRecording()))
			return;
		if (session->GetQuitRequest().has_value())
		{
			const Status stopped = Stop();
			ENGINE_VERIFY(stopped.has_value(), "ordinary play must release its quit request");
			return;
		}

		EditorAssetManager& assets = state.Editor->GetAssets();
		// Publication may have scheduled dependent imports after invoking OnAssetReload. Finish the whole batch before
		// entering Reload, which loads its complete closure. Never drain from inside the publication callback itself.
		// Only new notifications drain: a chain kept pending by a failed import never blocks later frames on unrelated work.
		const bool arrived = std::exchange(state.ScriptReloadsArrived, false);
		if (arrived && !state.PendingScriptReloads.empty())
			assets.WaitIdle();
		EventLog& events = state.Editor->GetEngine().GetEventLog();
		const auto failures = events.Read(state.ImportEventCursor, std::array{ EngineEventType::AssetImportFailed }, events.GetCapacity());
		state.ImportEventCursor = failures.NextCursor;
		std::set<std::tuple<std::string, uint32_t, uint32_t, std::string>> reported;
		const auto report = [&state, &reported](const ScriptError& error)
		{
			// A failed required module can fail several dependent imports in this batch. Publish the failing location once.
			if (reported.emplace(error.Script, error.Line, error.Column, error.Message).second)
				state.OnScriptError(error, false);
		};
		for (const EngineEvent& failure : failures.Events)
		{
			const AssetRecord* record = assets.GetRegistry().Find(failure.Id);
			if (record == nullptr || record->Metadata.Type != AssetType::Script)
				continue;
			bool located = false;
			if (const auto check = assets.GetScriptCheck(failure.Id); check)
			{
				for (const ScriptDiagnostic& diagnostic : check->Diagnostics)
				{
					if (diagnostic.Code != "SCRIPT_COMPILE_ERROR" || diagnostic.Severity != DiagnosticSeverity::Error)
						continue;
					ScriptError error;
					error.Kind = ScriptErrorKind::Compile;
					error.Script = diagnostic.File;
					error.Line = diagnostic.Line;
					error.Column = diagnostic.Column;
					error.Message = diagnostic.Message;
					error.Callback = "Reload";
					error.Tick = session->GetTick();
					report(error);
					located = true;
					break;
				}
			}
			if (!located)
			{
				ScriptError error;
				error.Kind = ScriptErrorKind::Compile;
				error.Script = failure.Path;
				error.Line = 1;
				error.Column = 1;
				error.Message = failure.Message;
				error.Callback = "Reload";
				error.Tick = session->GetTick();
				report(error);
			}
		}
		ScriptEngine* scripts = session->GetScripts();
		if (scripts == nullptr)
		{
			state.PendingScriptReloads.clear();
			return;
		}
		std::set<AssetHandle> failedImports;
		for (const AssetDiagnostic& diagnostic : assets.GetDiagnostics())
			if (diagnostic.Severity == DiagnosticSeverity::Error && diagnostic.Code == AssetImportFailedCode)
				failedImports.insert(diagnostic.Asset);
		const AssetDependencyGraph& graph = assets.GetDependencyGraph();
		const std::vector<AssetHandle> pending(state.PendingScriptReloads.begin(), state.PendingScriptReloads.end());
		for (const AssetHandle handle : graph.GetReimportOrder(pending))
		{
			if (!state.PendingScriptReloads.contains(handle))
				continue;
			const auto closure = graph.GetReimportOrder(std::array{ handle });
			// Gate on the chain Reload would actually patch (loaded modules and attached behaviours), not on every project
			// dependent: an unloaded test suite whose import failed cannot hold back the live game. A chain error is left
			// to Reload, which reports it.
			const auto chain = scripts->GetReloadChain(handle);
			if (chain && std::any_of(chain->begin(), chain->end(), [&failedImports](AssetHandle asset)
			{
				return failedImports.contains(asset);
			}))
				continue; // Keep the entire live chain until its next successful import; never install a partial chain.
			// The type gate (ADR 0022 decision 15): with Scripting.BlockPlayOnTypeErrors, a chain whose latest import check has
			// type errors keeps its running code and publishes one `type` error; a new source publication retries.
			if (!CheckReloadTypes(handle).has_value())
			{
				for (const AssetHandle affected : closure)
					state.PendingScriptReloads.erase(affected);
				if (chain)
				{
					for (const AssetHandle affected : *chain)
						state.PendingScriptReloads.erase(affected);
				}
				continue;
			}
			const auto reloaded = scripts->Reload(handle);
			if (reloaded && reloaded->Deferred)
				continue;
			// Reload publishes its own located runtime errors and rolls back its whole chain on failure. A new source
			// publication retries; do not repeat a failed candidate each frame or retry a dependent from the same batch.
			for (const AssetHandle affected : closure)
				state.PendingScriptReloads.erase(affected);
			if (reloaded)
				for (const AssetHandle affected : reloaded->Scripts)
					state.PendingScriptReloads.erase(affected);
		}
		if (session->GetQuitRequest().has_value())
		{
			const Status stopped = Stop();
			ENGINE_VERIFY(stopped.has_value(), "ordinary play must release its reload quit request");
		}
	}

	void EditorPlayController::OnInputEvent(const Event& event, bool gameFocused)
	{
		if (const auto* focus = std::get_if<WindowFocusEvent>(&event); focus != nullptr && !focus->Focused)
			if (Window* window = m_State->Editor->GetEngine().GetWindow(); window != nullptr && IsPlaying())
				window->SetCursorMode(CursorMode::Normal);
		PlaySession* session = GetSession();
		if (session == nullptr || !gameFocused || IsLiveInputSuppressed() || session->IsLockstep())
			return;
		// Positions arrive in the Game image's pixels, which are an ordinary session's view; a fixed view (a recording or a
		// lockstep session its owner has resumed) gets them scaled from the image into its own pixels.
		if (const auto* move = std::get_if<MouseMoveEvent>(&event); move != nullptr && m_State->SessionViewFixed)
		{
			const Result<glm::uvec2> image = m_State->Editor->GetViewportState().GetPixelSize(ViewportView::Game);
			if (image && *image != session->GetViewSize())
			{
				if (const std::optional<glm::vec2> position = MapToViewCoordinates(move->Position, glm::vec2(0.0f), glm::vec2(*image), session->GetViewSize()))
					session->GetInput().QueueDeviceEvent(MouseMoveEvent{ .Position = *position });
				return;
			}
		}
		session->GetInput().QueueDeviceEvent(event);
	}

	double EditorPlayController::GetFrameTimeScale() const
	{
		return m_State->Session != nullptr ? m_State->Session->GetTimeScale() : 1.0;
	}

	bool EditorPlayController::IsFrameThrottleSuspended() const
	{
		return m_State->TestOwner != nullptr || (m_State->Session != nullptr && m_State->Session->IsStepping());
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
		if (state.RecordingOwner == client)
		{
			if (ReplayRecorder* recorder = session->GetRecorder(); recorder != nullptr && recorder->IsRecording())
			{
				recorder->Cancel();
				session->SetPaused(true);
				state.AppendStateChanged();
			}
			state.RecordingOwner = NoClient;
		}
		if (session->GetLockstepOwner() == client)
		{
			if (ReplayRecorder* recorder = session->GetRecorder())
				recorder->Cancel();
			ReleaseReplayInput(session->GetSerial());
		}
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
		return std::string(PlayRunStateToString(GetPlayRunState(GetSession())));
	}

	std::optional<uint64_t> EditorPlayController::GetTick() const
	{
		if (GetSession() == nullptr)
			return std::nullopt;
		return GetSession()->GetTick();
	}

	ScriptErrorStream& EditorPlayController::GetScriptErrors()
	{
		return m_State->Errors;
	}

	Result<ScriptApiRegistry*> EditorPlayController::GetScriptApi()
	{
		return &m_State->Editor->GetEngine().GetScriptApiRegistry();
	}

	void EditorPlayController::SetScriptEnvironment(ScriptEnvironment environment)
	{
		environment.IsEditor = true;
		m_State->Environment = std::move(environment);
	}

	ScriptEnvironment EditorPlayController::GetScriptEnvironment() const
	{
		return m_State->GetScriptEnvironment();
	}
	bool EditorPlayController::IsTestRunActive() const
	{
		return m_State->TestOwner != nullptr;
	}
	bool EditorPlayController::IsLiveInputSuppressed() const
	{
		return IsTestRunActive() || m_State->ReplayInputSerial != 0;
	}

	void EditorPlayController::ReleaseReplayInput(uint64_t sessionSerial)
	{
		if (m_State->ReplayInputSerial == sessionSerial)
			m_State->ReplayInputSerial = 0;
	}

	Result<ReplayHeader> EditorPlayController::DescribeReplayHeader() const
	{
		if (!m_State->Session || !m_State->Header.Scene.Handle.IsValid())
			return MakeError(ErrorCode::InvalidState, "recording needs a saved scene asset");
		return m_State->Header;
	}

	Status EditorPlayController::CheckTestAdmission(ClientId client, bool ownerRestart) const
	{
		if (IsTestRunActive())
			return MakeError(ErrorCode::InvalidState, "another test run is active");
		if (!m_State->Editor->HasProject())
			return MakeError(ErrorCode::InvalidState, "no project is open");
		if (const PlaySession* session = m_State->Session.get())
		{
			const bool recording = session->GetRecorder() && session->GetRecorder()->IsRecording();
			// §13.6: the recording's owner restarts its own recording with restart:true; every other driver waits for it.
			const bool ownRecording = recording && ownerRestart && client != NoClient && m_State->RecordingOwner == client;
			if (session->IsStepping() || m_State->ReplayInputSerial != 0
				|| (session->IsLockstep() && session->GetLockstepOwner() != client)
				|| (recording && !ownRecording))
				return MakeError(ErrorCode::InvalidState, "another driver owns the play session");
		}
		return {};
	}

	Status EditorPlayController::AcquireTestRun(EditorFeatureTestHost& owner, ClientId client)
	{
		ENGINE_TRY(CheckTestAdmission(client));
		State& state = *m_State;
		const auto selected = state.Editor->GetSelection();
		state.SavedSelection.assign(selected.begin(), selected.end());
		state.SavedSelectionTarget = state.Editor->GetSelectionTarget();
		state.SavedTestCursor = state.GetScriptCursorMode();
		if (Window* window = state.Editor->GetEngine().GetWindow())
			window->SetCursorMode(CursorMode::Normal);
		if (state.Session)
		{
			state.SavedPaused = state.Session->IsPaused();
			state.SavedLockstep = state.Session->IsLockstep();
			state.SavedOwner = state.Session->GetLockstepOwner();
			state.Session->SetPaused(true);
			state.Session->SetLockstep(false);
		}
		state.TestOwner = &owner;
		state.TestDefersReloads = !state.Editor->GetAssets().AreReloadsDeferred();
		if (state.TestDefersReloads)
			state.Editor->GetAssets().SetReloadsDeferred(true);
		return {};
	}

	void EditorPlayController::PublishTestSession(EditorFeatureTestHost& owner, PlaySession* session)
	{
		ENGINE_ASSERT(m_State->TestOwner == &owner, "test publication requires the run lease");
		if (m_State->TestOwner == &owner)
			m_State->TestSession = session;
	}

	void EditorPlayController::ReleaseTestRun(EditorFeatureTestHost& owner)
	{
		State& state = *m_State;
		if (state.TestOwner != &owner)
			return;
		ENGINE_ASSERT(state.TestSession == nullptr, "unpublish the test session before releasing its host");
		state.TestOwner = nullptr;
		if (Window* window = state.Editor->GetEngine().GetWindow())
			window->SetCursorMode(state.SavedTestCursor);
		state.ImportEventCursor = state.Editor->GetEngine().GetEventLog().GetNextSeq();
		if (state.Session)
		{
			state.Session->SetLockstep(state.SavedLockstep, state.SavedOwner);
			state.Session->SetPaused(state.SavedPaused);
		}
		// Ids that vanished while the run owned the editor are dropped; restoring a selection never aborts the editor.
		state.Editor->RestoreSelection(std::move(state.SavedSelection), state.SavedSelectionTarget);
		if (state.TestDefersReloads)
			state.Editor->GetAssets().SetReloadsDeferred(false);
		state.TestDefersReloads = false;
	}

	Status EditorPlayController::StartRecording(const PlayStartOptions& options, bool restart)
	{
		State& state = *m_State;
		EditorContext& editor = *state.Editor;
		ENGINE_TRY(CheckTestAdmission(options.LockstepOwner, restart));
		if (state.Session && !restart)
			return MakeError(ErrorCode::InvalidState, "pass restart:true to replace the play session");
		PlayStartOptions recording = options;
		if (recording.ScenePath.empty())
		{
			// The replay names the scene by its saved asset and every replay loads that file (§6.7), so the recording
			// starts from the same file, never from unsaved edits the replay could not reproduce.
			if (!editor.HasScene())
				return MakeError(ErrorCode::InvalidState, "no scene is open");
			if (!editor.GetScenePath().has_value())
				return std::unexpected(Error(ErrorCode::InvalidState, "recording needs a saved scene asset").WithHint("save the scene with scene.save, then start the recording"));
			if (editor.IsSceneDirty())
			{
				return std::unexpected(Error(ErrorCode::InvalidState, "the open scene has unsaved changes: a recording starts from the saved scene asset its replay names")
						.WithHint("save the scene with scene.save, then start the recording"));
			}
			recording.ScenePath = std::string(editor.GetScenePath()->GetPath());
		}
		ENGINE_TRY(ValidatePlayRequest(recording));
		ENGINE_TRY(CheckPlayScripts(editor.GetProject().GetSettings()));
		ENGINE_TRY(editor.PrepareForPlay());
		ENGINE_TRY(ReplaceSession(recording, editor.GetProject().GetSettings()));
		state.RecordingOwner = options.LockstepOwner;
		return {};
	}

	void EditorPlayController::SetPhaseMarker(Watchdog& marker)
	{
		if (m_State->PhaseMarker != nullptr && m_State->PhaseMarker != &marker)
			m_State->PhaseMarker->SetScriptPhase({});
		m_State->PhaseMarker = &marker;
	}

	void EditorPlayController::ClearPhaseMarker(const Watchdog& marker)
	{
		if (m_State->PhaseMarker != &marker)
			return;
		m_State->PhaseMarker->SetScriptPhase({});
		m_State->PhaseMarker = nullptr;
	}

	Status EditorPlayController::ReplaceSession(const PlayStartOptions& options, ProjectSettings project)
	{
		State& state = *m_State;
		EditorContext& editor = *state.Editor;
		if (editor.GetAssets().HasErrorDiagnostics())
			return MakeError(ErrorCode::InvalidState, "the project has asset errors: fix them before replacing play");
		// Prepare without callbacks or mixer/time ownership. Every fallible step leaves the original session untouched.
		AudioEngine* audio = editor.GetEngine().GetAudioEngine();
		const auto baseline = state.MixerBaseline.has_value() ? state.MixerBaseline
			: audio != nullptr                                ? std::optional(Utils::ReadMixerVolumes(*audio))
															  : std::nullopt;
		const CursorMode cursor = state.GetScriptCursorMode();
		// Recording and replay sessions are deterministic: they keep the fixed view, lockstep or not.
		ENGINE_TRY_ASSIGN(PreparedSession prepared, PrepareSession(options, std::move(project), false, nullptr, nullptr, false, false, nullptr, false, true));
		if (!prepared.Header.Scene.Handle.IsValid())
			return MakeError(ErrorCode::InvalidState, "recording and replay require a saved scene asset");
		state.Session.reset();
		if (!state.CursorBaseline)
			state.CursorBaseline = cursor;
		if (Window* window = editor.GetEngine().GetWindow())
			window->SetCursorMode(*state.CursorBaseline);
		state.PendingScriptReloads.clear();
		state.ImportEventCursor = editor.GetEngine().GetEventLog().GetNextSeq();
		if (baseline)
			Utils::RestoreMixerVolumes(*audio, *baseline);
		state.MixerBaseline = baseline;
		state.Header = std::move(prepared.Header);
		state.Session = std::move(prepared.Session);
		state.SessionViewFixed = prepared.FixedView;
		state.ReplayInputSerial = 0;
		state.RecordingOwner = NoClient;
		state.Session->Activate();
		// Startup faults, Debug.Break and fatal stop retain their pause even when the launch options request running.
		state.Session->SetPaused(options.Paused || state.Session->IsPaused());
		if (options.Lockstep && !editor.GetAssets().AreReloadsDeferred())
		{
			editor.GetAssets().SetReloadsDeferred(true);
			state.DefersReloads = true;
		}
		// An edit selection follows into the new runtime scene; the replaced runtime scene's selection is cleared.
		editor.RetargetSelectionToPlay();
		state.AppendStateChanged();
		return {};
	}

	Status EditorPlayController::StartReplay(const ReplayHeader& header, ClientId owner)
	{
		ENGINE_TRY(CheckTestAdmission(owner));
		EditorContext& editor = *m_State->Editor;
		const AssetRecord* scene = editor.GetAssets().GetRegistry().Find(header.Scene.Handle);
		if (scene == nullptr || scene->Metadata.Type != AssetType::Scene)
			return MakeError(ErrorCode::NotFound, "the replay's scene asset is missing");
		PlayStartOptions options;
		options.ScenePath = std::string(scene->SourcePath.GetPath());
		options.Seed = header.Seed;
		options.Parameters = header.Parameters;
		options.Lockstep = true;
		options.LockstepOwner = owner;
		options.Paused = true;
		ProjectSettings project = editor.GetProject().GetSettings();
		project.Simulation.FixedHz = header.FixedHz;
		ENGINE_TRY(ValidatePlayRequest(options));
		ENGINE_TRY(CheckPlayScripts(project));
		// Every RPC path into play cancels the active gestures and saves the recovery copy first (EditorApp::BeforePlay).
		ENGINE_TRY(editor.PrepareForPlay());
		ENGINE_TRY(ReplaceSession(options, std::move(project)));
		m_State->ReplayInputSerial = m_State->Session->GetSerial();
		return {};
	}

}
