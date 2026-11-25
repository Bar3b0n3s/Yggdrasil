#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Platform/DisplayModes.h"
#include "Engine/Platform/Input/InputState.h"
#include "Engine/Platform/Window.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Scripting/ScriptPerformanceStats.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class AssetManager;
	class AudioSystem;
	class DebugDrawList;
	class IFieldSchemaSource;
	class NavigationWorld;
	class Physics2DSystem;
	class PhysicsSystem;
	class Random;
	class Scene;
	class SessionMusic;
	class TypeRegistry;
	class UiSystem;

	// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 3): what a world pause does to the world's playing voices
	// (Time.PauseWorld's `sounds`, play.setWorldPaused's): Pause holds the voices of every group but Music and Ui (with
	// today's groups, the Sfx group's) until the world resumes, Finish lets them play on to their end; the Music and Ui
	// groups' voices play on either way (M40, Docs/Decisions/0057-m40-music.md decision 14, amending ADR 0050 decision 3:
	// music and stingers sound under a pause menu).
	enum class WorldPauseSounds : uint8_t
	{
		Pause,
		Finish
	};

	struct ScriptFrameState
	{
		InputPhase Phase = InputPhase::Step;
		uint64_t Tick = 0;
		uint64_t Frame = 0;
		double DeltaTime = 0.0;
		double FixedDeltaTime = 1.0 / 60.0;
		double TimeScale = 1.0;
		float InterpolationAlpha = 1.0f;
		// M35 (decisions 1 and 2): whether the world pause holds this phase (the fixed step that runs, or the frame phase
		// after it), so OnFixedUpdate, OnUpdate, OnLateUpdate and tasks run only for the behaviours that run while the world
		// is paused; and the session ticks the world pause has held the world's clock so far, so the world's tick (what
		// Time.GetTick and Time.GetTime read, and what a behaviour's tasks count) is Tick - WorldHeldTicks. A host without a
		// world pause keeps both defaults, so its world's clock is its session's.
		bool WorldPaused = false;
		uint64_t WorldHeldTicks = 0;
	};

	// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 2): the world's tick of `frame`, which holds while the world
	// is paused: the session's tick less the ticks the world pause has held it. The held ticks never exceed the session's (the
	// world's clock starts with the session's and only falls behind it). Time.GetTick and Time.GetTime, Wind:Sample's default
	// time and the tasks on the world's clock all read it.
	[[nodiscard]] inline uint64_t GetWorldTick(const ScriptFrameState& frame)
	{
		return frame.Tick - std::min(frame.WorldHeldTicks, frame.Tick);
	}

	struct ScriptEnvironment
	{
		bool IsEditor = false;
		bool IsHeadless = true;
		bool IsFocused = false;
		std::string Platform{};
		std::string Version{};
		glm::vec2 WindowSize{ 0.0f };
	};

	struct ScriptActionState
	{
		bool Down = false;
		bool Pressed = false;
		bool Released = false;
		float Axis = 0.0f;
	};

	// The only upward-facing boundary of Scripting (§3). A Session adapter supplies these lower-layer services; the VM
	// never includes Session, Testing, App or editor headers. Main-thread-only borrowed back-reference, outliving its VM.
	// Methods receiving external values validate them and return located errors; they never assert on a script value.
	class IScriptHost
	{
	public:
		virtual ~IScriptHost() = default;

		[[nodiscard]] virtual Scene& GetScene() = 0;
		[[nodiscard]] virtual const TypeRegistry& GetTypes() const = 0;
		[[nodiscard]] virtual AssetManager* GetAssets() = 0;
		[[nodiscard]] virtual const IFieldSchemaSource* GetFieldSchemas() const = 0;
		[[nodiscard]] virtual PhysicsSystem* GetPhysics() = 0;
		// M21 (Docs/Decisions/0032-m21-contract.md decision 10): the session's 2D physics, which the RigidBody2D proxy and the
		// Physics2D module use (stream P2). Null by default, where there is none (the edit evaluation host keeps the default);
		// the play session and the script test fixture override it.
		[[nodiscard]] virtual Physics2DSystem* GetPhysics2D() { return nullptr; }
		[[nodiscard]] virtual AudioSystem* GetAudio() = 0;
		[[nodiscard]] virtual DebugDrawList* GetDebugDraw() = 0;
		[[nodiscard]] virtual Random& GetRandom() = 0;
		[[nodiscard]] virtual const InputState& GetInput() const = 0;
		[[nodiscard]] virtual ScriptFrameState GetFrameState() const = 0;
		[[nodiscard]] virtual ScriptEnvironment GetEnvironment() const = 0;
		// Nonzero identity of this scene incarnation; never reused while a proxy from an older VM might exist.
		[[nodiscard]] virtual uint64_t GetSceneGeneration() const = 0;
		[[nodiscard]] virtual const Json& GetLoadParameters() const = 0;
		// Combines physical and injected actions in the current phase. Unknown names return INPUT_UNKNOWN_ACTION with hints.
		[[nodiscard]] virtual Result<ScriptActionState> GetAction(std::string_view name) const = 0;

		// Immediate scene mutation, seeded IDs and entity-cap checks. The binding creates script instances and calls
		// OnCreate before returning. A null parent means root. Instantiation uses the shared PrefabInstantiator path.
		[[nodiscard]] virtual Result<UUID> CreateEntity(std::string_view name, UUID parent) = 0;
		[[nodiscard]] virtual Result<UUID> Instantiate(AssetHandle prefab, const std::optional<glm::vec3>& position,
			const std::optional<glm::quat>& rotation, UUID parent) = 0;
		virtual void MarkTeleported(UUID entity) = 0;
		[[nodiscard]] virtual Status SetTimeScale(double scale) = 0;
		[[nodiscard]] virtual Status SetCursorMode(CursorMode mode) = 0;
		[[nodiscard]] virtual CursorMode GetCursorMode() const = 0;

		// Requests never destroy their own running VM. Scene.Load is consumed at the end of a frame, after protected calls
		// unwind: a fixed step's request after its tick (the frame runs no further step), a startup callback's after the
		// next tick, a frame-phase request at that frame's end (ADR 0038 decision 15); it starts a fresh VM with the
		// parameters and preserves the session's clock and replay owner.
		// Takes ownership of the detached parameters. Callers and hosts move the tree into pending storage so accepted
		// MaxJsonDepth values never require a recursive JSON copy at this boundary.
		[[nodiscard]] virtual Status RequestSceneLoad(AssetHandle scene, Json parameters) = 0;
		// Scene.Reload (M20, W-P06; Docs/Decisions/0031-m20-contract.md decision 5): requests, like RequestSceneLoad, the
		// replacement of the current scene when a Scene.Load would apply, by the document the current scene was started from: the
		// session's start document (in editor play the play copy, unsaved edits included) while the current scene is the
		// one the session started with, otherwise the document of the scene asset the last Scene.Load loaded, as it was
		// then. `parameters` replaces the load parameters; nullopt keeps the current scene's (GetLoadParameters). One
		// transition at a time: a second Scene.Load or Scene.Reload while one is pending is refused with InvalidState, as a
		// second RequestSceneLoad is.
		[[nodiscard]] virtual Status RequestSceneReload(std::optional<Json> parameters) = 0;
		// Scene.GetCurrent (M20): the scene asset the current scene came from (the editor's open scene or the play.start
		// scene, a suite's scene, the Runtime's start scene, then the asset each Scene.Load names; a Scene.Reload keeps it);
		// the invalid handle for a scene without an asset (an editor scene never saved).
		[[nodiscard]] virtual AssetHandle GetCurrentScene() const = 0;
		virtual void RequestQuit(int32_t exitCode) = 0;
		virtual void RequestPause() = 0;
		// Publish to Script logging, EventLog and host observability. fatal means second/hard memory breach; stop safely.
		virtual void OnScriptError(const ScriptError& error, bool fatal) = 0;
		// Called immediately before an eval/test driver mutates gameplay state or the shared random stream, including
		// on a call that later fails. Invalidates input-only recording; normal gameplay callbacks do not call this.
		// Test input injection is recorded input, so it is not an external state mutation.
		virtual void OnExternalMutation(std::string_view reason) = 0;
		// True in lockstep, recording, replay or test runs. Explicit Test.ReloadScript uses the tested synchronous override.
		[[nodiscard]] virtual bool IsReloadDeferred() const = 0;
		// §13.2 watchdog phase: the innermost gameplay script entry of the main thread changed. `script` is its authored
		// source path (empty for a test case body) and `callback` the callback, "Task" or "Test.Case"; both are empty once no
		// gameplay entry is running (one-shot evaluations never report one). Called on every protected entry and exit, so
		// implementations must be cheap; hosts without a phase marker keep the default, which ignores it.
		virtual void OnScriptPhase(std::string_view /*script*/, std::string_view /*callback*/) {}
		// M22 (C22.2, Docs/Decisions/0033-m22-contract.md decision 28): the play session's UI (Scene/UiSystem.h), which the
		// Ui script module reads and focuses through; a play session's host returns PlaySession::GetUi, and every other host
		// (the edit evaluation, test fixtures without a session) keeps the default, null, for which the module reports no
		// focus, hover or press and no rectangle, and refuses Ui.SetFocus.
		[[nodiscard]] virtual UiSystem* GetUi() { return nullptr; }
		// M22 (C22.3, Docs/Decisions/0033-m22-contract.md decision 46; stream T): the committed characters latched into
		// `phase`'s view (UTF-8; PlayInput::GetText, the text latch), which Input.GetText reads in that phase; valid until the
		// session's next latch. A play session's host returns its PlayInput's, and every other host (the edit evaluation,
		// test fixtures without a session) keeps the default, empty, so Input.GetText reads no text there.
		[[nodiscard]] virtual std::string_view GetText(InputPhase /*phase*/) const { return {}; }
		// ADR 0038 decision 14: called by every start flush (ScriptEngine::StartPending) before each round of OnStart, outside
		// any physics dispatch. A play session's host creates, rebuilds and removes the physics bodies of both worlds whose
		// components changed since the last synchronization or pre-step (PhysicsSystem::SynchronizeBodies,
		// Physics2DSystem::SynchronizeBodies, after a transform update), so an entity has its body from its OnStart on,
		// wherever it was created; every other host (the edit evaluation, test fixtures without a session) keeps the
		// default, which does nothing.
		virtual void SynchronizePhysicsBodies() {}
		// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 1): the world pause (Time.PauseWorld,
		// Time.ResumeWorld). RequestWorldPause records the request, which takes effect at the session's next fixed step, so
		// a tick runs wholly paused or wholly not; a later request before that step replaces it. A play session's host
		// forwards it to PlaySession::RequestWorldPause; every other host (the edit evaluation, test fixtures without a
		// session) keeps the default, which refuses it with Unsupported. IsWorldPaused is the state the next fixed step runs
		// with (the latest request, or the state in force when none is pending); false by default.
		[[nodiscard]] virtual Status RequestWorldPause(bool /*paused*/, WorldPauseSounds /*sounds*/)
		{
			return MakeError(ErrorCode::Unsupported, "this script host has no world to pause");
		}
		[[nodiscard]] virtual bool IsWorldPaused() const { return false; }
		// M28 (Docs/Decisions/0043-m28-game-services.md decision 4): Application.GetStats's counters. A play session's host
		// answers from its session and its host's frame statistics; every other host (the edit evaluation, test fixtures
		// without a session) keeps the default, Unsupported.
		[[nodiscard]] virtual Result<ScriptPerformanceStats> GetPerformanceStats() const
		{
			return MakeError(ErrorCode::Unsupported, "this host has no performance readout");
		}
		// M28 (Docs/Decisions/0043-m28-game-services.md decision 6): Input.PostSignal, a live frame-time signal for the next
		// fixed step: true when the session takes live signals (ordinary play and recordings), false where it ignores them
		// (replays, suites, FeatureTest runs and lockstep sessions without a recorder, which take only played-back and
		// injected signals). Errors: InvalidArgument for a name the signal's rules refuse, or a tick already full of signals.
		// Every other host keeps the default, Unsupported.
		[[nodiscard]] virtual Result<bool> PostSignal(std::string_view /*name*/)
		{
			return MakeError(ErrorCode::Unsupported, "this host takes no signals: Input.PostSignal works in a running play session");
		}
		// Input.WasSignalled: whether `name` was signalled in `phase`'s view (PlayInput::WasSignalled). Default: false.
		[[nodiscard]] virtual bool WasSignalled(InputPhase /*phase*/, std::string_view /*name*/) const { return false; }
		// M28 (Docs/Decisions/0043-m28-game-services.md decision 3): Application.GetDisplayModes, GetDisplaySettings and
		// SetDisplaySettings. A play session answers them itself where the calls do nothing (the editor, headless runs and every
		// fixed-view session) and forwards the others to its host; every other host keeps the defaults: Unsupported, so the
		// module reports no modes, the view as a window and refuses a request.
		[[nodiscard]] virtual Result<std::vector<DisplayVideoMode>> GetDisplayModes() const
		{
			return MakeError(ErrorCode::Unsupported, "this host has no display");
		}
		[[nodiscard]] virtual Result<DisplaySettings> GetDisplaySettings() const { return MakeError(ErrorCode::Unsupported, "this host has no display"); }
		[[nodiscard]] virtual Status RequestDisplaySettings(const DisplaySettings& /*settings*/)
		{
			return MakeError(ErrorCode::Unsupported, "this host has no display: display settings apply in a windowed Runtime");
		}
		// M28 (decision 8): the SaveData module and Test.SetSaveData over a play session's store (Session/SaveDataStore.h).
		// GetSaveValue returns the key's value, or null when the key is absent; SetSaveValue deletes it with null;
		// ReplaceSaveData replaces every value; CommitSaveData writes the store where its host keeps it; GetSaveDataLoadError
		// is why the store the session started with could not be read. Every other host keeps the defaults, Unsupported,
		// which the module raises, but for CommitSaveData, which SaveData.Commit returns as false and the reason.
		// Defined in ScriptHost.cpp, where Json is complete.
		[[nodiscard]] virtual Result<Json> GetSaveValue(std::string_view key) const;
		[[nodiscard]] virtual Status SetSaveValue(std::string_view /*key*/, const Json& /*value*/) { return MakeError(ErrorCode::Unsupported, "{}", NoSaveDataReason); }
		[[nodiscard]] virtual Status ReplaceSaveData(const Json& /*values*/) { return MakeError(ErrorCode::Unsupported, "{}", NoSaveDataReason); }
		[[nodiscard]] virtual Result<std::vector<std::string>> GetSaveKeys() const { return MakeError(ErrorCode::Unsupported, "{}", NoSaveDataReason); }
		[[nodiscard]] virtual Status CommitSaveData() { return MakeError(ErrorCode::Unsupported, "{}", NoSaveDataReason); }
		[[nodiscard]] virtual Result<std::optional<std::string>> GetSaveDataLoadError() const
		{
			return MakeError(ErrorCode::Unsupported, "{}", NoSaveDataReason);
		}
		// M30 (Docs/Decisions/0045-m30-navigation.md decision 11): the play session's navigation (Navigation/NavigationWorld.h),
		// built from the current scene's baked navigation mesh, which the Navigation script module queries; null when the
		// scene has none. A play session's host returns PlaySession::GetNavigation, and every other host (the edit
		// evaluation, test fixtures without a session) keeps the default, null, for which every query finds nothing.
		[[nodiscard]] virtual NavigationWorld* GetNavigation() { return nullptr; }
		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 1): Loc.SetLocale's change of the session's locale, `locale`
		// empty or a tag (StringTableData.h's IsLocaleTag; the binding checks it first): a play session's host records it as
		// the session's (PlaySession::SetLocale), which a Scene.Load's and a Scene.Reload's next VM starts in, and sets its VM's
		// copy (ScriptEngine::SetLocale); the binding then dispatches OnLocaleChanged (ScriptEngine::NotifyLocaleChanged).
		// Every other host (the edit evaluation, test fixtures without a session) keeps the default, Unsupported.
		[[nodiscard]] virtual Status SetLocale(std::string_view /*locale*/)
		{
			return MakeError(ErrorCode::Unsupported, "this host has no session locale: Loc.SetLocale works in a play session");
		}
		// M40 (Docs/Decisions/0057-m40-music.md decision 5): the play session's music (Scene/SessionMusic.h), which
		// Audio.PlayMusic, Audio.StopMusic, Audio.GetMusic and Test.GetMusic drive. A play session's host returns
		// PlaySession::GetMusic (null in Simulate, which runs no scripts; a session without an audio engine has one that
		// records requests only), and every other host (the edit evaluation, test fixtures without a session) keeps the
		// default, null, for which those calls raise "audio is unavailable".
		[[nodiscard]] virtual SessionMusic* GetMusic() { return nullptr; }
	private:
		static constexpr std::string_view NoSaveDataReason = "this host has no save data: SaveData works in a play session";
	};

}
