#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Reflection/VariantValue.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Session/PlayInput.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The play session (Architecture §5.6, §5.7, §13.6): a runtime copy of a scene and the simulation that runs it in the
// documented step order. The editor creates one per Play or Simulate from the open edit scene (serializer copy, §5.6), the
// Runtime one from the start scene of its Game.pak (§14.3): the same SceneSerializer::FromJson path, strict, so "works in
// the editor, broken in export" cannot happen. The session owns its runtime scene, its seeded UUIDGenerator and Random
// (§4.8, §4.12), its game input (PlayInput), physics, scripts, audio and the game view's last render snapshot.
//
// Frozen by the M7 contract (Docs/Decisions/0012-m7-decisions.md decisions 2 to 5); the M12 contract added the audio hook
// (PlaySessionSpecification::Audio and OwnsAudioTime, GetAudioSystem, IsAudioTimeOwned and the "Audio" paragraph below;
// Docs/Decisions/0015-m12-decisions.md). M13 adds scripting, replay recording and frame-end scene replacement
// (Docs/Decisions/0019-m13-contract.md).

namespace Engine {

	class AnimationSystem;
	class AssetManager;
	class AudioEngine;
	class AudioSystem;
	class Entity;
	class Physics2DSystem;
	class PhysicsSystem;
	class ReplayRecorder;
	class Scene;
	class ScriptApiRegistry;
	class ScriptEngine;
	class ScriptFieldSchemaSource;
	class IScriptTestHost;
	class NativeModuleTypes;
	class NativeWorld;
	class ParticleSystem;
	class TypeRegistry;
	class UiSystem;
	struct RenderExtractionRequest;

	// Play runs scripts and audio; Simulate is Play without them (§5.6), viewed through the editor camera.
	enum class PlayMode : uint8_t
	{
		Play,
		Simulate
	};

	// The documented step order (§5.7), one value per phase in the order a tick runs them. A tick is one fixed step; the
	// frame phase runs once per frame, and once per tick in lockstep and with a ManualClock (Tick()). M35
	// (Docs/Decisions/0050-m35-pause-entity-state.md decision 2) classifies each phase as the world's or the UI's: while the
	// world is paused the world's phases (NativeStep, SpriteAnimation, AnimationSystem, PhysicsPreStep to Physics2DPostStep,
	// Particles) do not run, FixedUpdate, Tasks, Update and LateUpdate run only for the behaviours that run while the world
	// is paused, and every other phase (the UI's) runs as always; a phase added later is classified when it lands.
	enum class PlaySessionPhase : uint8_t
	{
		// Fixed step.
		InterpolationSnapshot,   // 0: TransformSystem::Update when a transform changed outside the session's phases since its
								 //    last update (automation at the safe point); then PreviousWorldTransform <- WorldTransform
								 //    for every entity and InterpolationResetTag cleared (§5.2)
		ApplyInput,              // 1: the events queued for this tick, then the step view latched (PlayInput::ApplyTick)
		TextLatch,               // 1a: PlayInput::LatchStepText: the tick's committed characters (its Text events) latched into
								 //     the step view, so UiInput and the tick's scripts read the same text (M22 C22.3,
								 //     Docs/Decisions/0033-m22-contract.md decision 46); in Play and Simulate
		UiInput,                 // 1b: UiSystem::ProcessInput with the step view's pointer, wheel, stick and the Ui settings'
								 //     navigation bindings, against the current UI layout: focus, hover, press, widgets and the
								 //     UI events queued (M22 C22.2, Docs/Decisions/0033-m22-contract.md decision 24); in Play and
								 //     Simulate, with or without a canvas (without one it reads no input and keeps the interaction
								 //     state empty). C22.3 (decisions 47 and 50) adds the step's text and editing keys and, while
								 //     the layout holds a World canvas, the pointer's ray through the primary camera
		StartFlush,              // 2: the start flush (M13; empty), the only one in a tick (the frame phase runs none; the
								 //    session's and a scene transition's startup run their own, followed by a destroy flush):
								 //    in rounds until no instance waits, the physics bodies of changed components synchronized
								 //    (both worlds), then OnStart of the instances created or enabled since the last flush,
								 //    canonical order, those the round's own callbacks create starting in the next round
								 //    (ScriptEngine::StartPending; Docs/Decisions/0038-m27-rolling-ball.md decision 14)
		UiEvents,                // 2a: the queued UI events delivered to their callbacks, bubbling (ScriptEngine::DispatchUiEvents;
								 //     M22 C22.2, decision 27): after OnStart, so a new instance starts before its first UI
								 //     callback, and before FixedUpdate, which sees the interaction; Play only (Simulate drops
								 //     the queue after UiInput)
		FixedUpdate,             // 3: script OnFixedUpdate, (ExecutionOrder, canonical) order (M13; empty)
		Tasks,                   // 4: Task.Wait*/Task.Delay threads due at this tick, then the test case thread (M13; empty)
		NativeStep,              // 4a: the native module's systems (NativeWorld::Step; M19, Docs/Decisions/0030-m19-contract.md
								 //     decision 28): only in Play with a native module (PlaySessionSpecification::Native)
		SpriteAnimation,         // 4b: SpriteAnimationSystem::Step, every playing SpriteAnimator advanced one tick (M21,
								 //     Docs/Decisions/0032-m21-contract.md decision 5); Play and Simulate
		AnimationSystem,         // 4c: AnimationSystem::Step, every Animator's layers advanced, its pose sampled, root motion
								 //     applied and events delivered (M23, Docs/Decisions/0034-m23-contract.md decision 29); Play
								 //     and Simulate
		PreStepTransformUpdate,  // 5: TransformSystem::Update, before PhysicsSystem::PreStep
		PhysicsPreStep,          // 5: PhysicsSystem::PreStep: create, rebuild or remove bodies; teleports; MoveKinematic (M11)
		PhysicsStep,             // 6: PhysicsSystem::Step: PhysicsWorld::Step(fixedDt) (M11)
		PhysicsPostStep,         // 7: PhysicsSystem::PostStep: dynamic poses written back; sorted collision and trigger events (M11)
		Physics2DPreStep,        // 7a: TransformSystem::Update when the 3D post-step changed a transform, then
								 //     Physics2DSystem::PreStep (M21, Docs/Decisions/0032-m21-contract.md decision 10)
		Physics2DStep,           // 7b: Physics2DSystem::Step: Physics2DWorld::Step(fixedDt) (M21)
		Physics2DPostStep,       // 7c: Physics2DSystem::PostStep: 2D poses written back; sorted 2D collision and trigger events (M21)
		DestroyFlush,            // 8: OnDestroy, synthesized exits, body/voice/instance removal (M11-M13), then entity destruction,
								 //    children first (Scene::FlushPendingDestroys)
		PostStepTransformUpdate, // 9: TransformSystem::Update
		UiLayout,                // 10: UiSystem::LayoutStep at the session's scene resolution (M22, Docs/Decisions/0033-m22-contract.md
								 //     decision 3): after every phase that can change UI data, so the layout matches the state
								 //     the step left; in Play and Simulate
		// Frame phase.
		LatchFrame,           // 1: PlayInput::LatchFrame
		Update,               // 2: script OnUpdate(frameDt) (M13; empty)
		LateUpdate,           // 2: script OnLateUpdate(frameDt) (M13; empty)
		FrameDestroyFlush,    // 2: destroy flush, as DestroyFlush
		FrameTransformUpdate, // 2: TransformSystem::Update; entities whose world matrix changed outside the fixed steps get
							  //    InterpolationResetTag (see "Render interpolation" below)
		FrameUiLayout,        // 2a: UiSystem::LayoutFrame (M22, decision 3): UI written in OnUpdate or OnLateUpdate is laid out
							  //     as this frame draws it; in Play and Simulate
		AudioUpdate,          // 3: AudioSystem::Update, then, while the session owns audio time, the simulation-time pulls of
							  //    the ticks run since the last pull (M12; see "Audio" below)
		Particles,            // 3a: ParticleSystem::Step, every ParticleEmitter's presentation-only simulation advanced one fixed
							  //     step per tick run since the last frame (M26, C26.1, Docs/Decisions/0037-m26-contract.md
							  //     decision 6; ADR 0028's phase-3 ledger): after every phase that moves an emitter or queues an
							  //     emit, right before the extraction that copies the particles; in Play and Simulate, whether
							  //     or not extraction is enabled; it writes nothing the state hash covers
		RenderExtraction      // 4: the game view's snapshot (ExtractRenderSnapshot), unless extraction is disabled
	};

	// The phase's enumerator name ("InterpolationSnapshot" ...).
	[[nodiscard]] std::string_view PlaySessionPhaseToString(PlaySessionPhase phase);

	// View coordinates: the pixels of the session's view (SetViewSize), origin at the top-left corner, +x right, +y down.
	// Ordinary live play follows the host's game view, the image its game camera renders into: the Runtime's window
	// framebuffer, the editor's Game panel image (a fixed resolution or the panel's extent times the framebuffer scale,
	// letterbox bars excluded). A deterministic session (a test run's suites and replays, the verification of a test-run
	// recording, input.record and input.replay sessions, the Runtime's --replay, and every session started in lockstep)
	// keeps GetDeterministicViewSize for its whole life in every host, whatever the panel, the window or the display's scale,
	// so its results and state hashes never depend on them. Scripts see cursor positions, Application.GetWindowSize and
	// Camera:ScreenToWorldRay/WorldToScreen in view coordinates; hosts map device cursor positions into the session's view
	// before PlayInput::QueueDeviceEvent, while injected input (input.inject, play.step, replays, Test.Inject*) already
	// uses them. This maps `position`, given in the coordinates of a display rectangle at `displayMin` of `displaySize`
	// that shows the whole view stretched over it, to the view's pixels: linear and unclamped, so a cursor beyond the image
	// keeps moving continuously. nullopt for a non-finite position, a rectangle without area or an empty view. Pure.
	[[nodiscard]] std::optional<glm::vec2> MapToViewCoordinates(const glm::vec2& position, const glm::vec2& displayMin,
		const glm::vec2& displaySize, const glm::uvec2& viewSize);
	// M21 (Docs/Decisions/0032-m21-contract.md decision 6): MapToViewCoordinates for a display rectangle that shows an image of
	// `imageSize` pixels, stretched over it, which renders the session's scene with the output settings `output` (the
	// session's last extraction's), when the session's view of `viewSize` renders it with the same settings. The point goes
	// to the image's pixels, through the image's output mapping to its scene point (MapViewPointToScenePoint of
	// ComputeRenderOutputMapping), from the image's scene resolution to the view's, and through the view's mapping to the
	// view's pixels, so it names the scene pixel the image shows there: under a PixelPerfectCamera the two mappings have
	// different integer scales and letterboxes, and a plain stretch would name another pixel. When both mappings are the
	// identity (no PixelPerfectCamera: each scene is its own view) or the image is the view's size, this is
	// MapToViewCoordinates exactly. Linear and unclamped; nullopt as MapToViewCoordinates, or for an image without pixels.
	// Pure.
	[[nodiscard]] std::optional<glm::vec2> MapImageToViewCoordinates(const glm::vec2& position, const glm::vec2& displayMin,
		const glm::vec2& displaySize, const glm::uvec2& imageSize, const glm::uvec2& viewSize, const RenderOutputSettings& output);
	// The fixed view of a deterministic session (see MapToViewCoordinates): the project's window size in logical pixels,
	// at least 1 x 1. Pure.
	[[nodiscard]] glm::uvec2 GetDeterministicViewSize(const WindowSettings& window);

	class PlaySession;

	// Optional production test driver, distinct from the phase-entry instrumentation below. Borrowed main-thread
	// back-reference, outliving the session. AfterTasks runs after normal tasks in phase 4, never re-enters Tick.
	// OnQuit records intent only: teardown waits until the active protected call and frame have unwound.
	class IPlaySessionTestHook
	{
	public:
		virtual ~IPlaySessionTestHook() = default;
		virtual void AfterTasks(PlaySession& session, const SimStep& step) = 0;
		virtual void OnQuit(int32_t exitCode) = 0;
	};

	// Optional application-facing services of scripts; Session does not include App or editor code. Outlives the
	// session. A missing host uses the specification's Environment, rejects cursor changes and exposes quit intent
	// through GetQuitRequest. Error publication is still retained in ScriptEngine's owned error stream. Either way the
	// environment's WindowSize that scripts see is the session's view size (view coordinates, see MapToViewCoordinates).
	class IPlaySessionHost
	{
	public:
		virtual ~IPlaySessionHost() = default;
		[[nodiscard]] virtual ScriptEnvironment GetScriptEnvironment() const = 0;
		[[nodiscard]] virtual Status SetScriptCursorMode(CursorMode mode) = 0;
		[[nodiscard]] virtual CursorMode GetScriptCursorMode() const = 0;
		virtual void OnScriptError(const ScriptError& error, bool fatal) = 0;
		// IScriptHost::OnScriptPhase of the session's VM, forwarded for the host's automation watchdog (§13.2 Busy names
		// "Script:<Script>.<Callback>"). Called on every protected script entry and exit: keep it cheap.
		virtual void OnScriptPhase(std::string_view script, std::string_view callback) = 0;
	};

	// Instrumentation of the step order (§5.7 "documented and tested"; "PlaySession: step order matches the documented
	// sequence"): the session calls OnPhase at the start of every phase it runs. The observer may change the session's
	// scene during FixedUpdate, Update, LateUpdate and PhysicsStep to exercise phase-dependent behavior
	// ("Interpolation: script-moved entities interpolate; frame-phase writes, teleports and new entities do not").
	// Production code installs none. Simulate mode skips the script and audio
	// phases (StartFlush, UiEvents, FixedUpdate, Tasks, Update, LateUpdate, AudioUpdate) entirely, so they are
	// not reported there. The world's phases are not run, so not reported, while the world is paused (M35; see "World pause"
	// in PlaySession's comment). NativeStep (M19) runs, and is reported, only in Play mode with a native module. UiLayout and
	// FrameUiLayout (M22), UiInput (M22 C22.2) and TextLatch (M22 C22.3), run, and are reported, in both modes, with or without
	// a canvas.
	class IPlaySessionObserver
	{
	public:
		virtual ~IPlaySessionObserver() = default;

		// `tick` is the tick being stepped for fixed-step phases, and the session's GetTick() (the next tick) for
		// frame-phase phases.
		virtual void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) = 0;
	};

	// What play.start asks a host for (§13.5 play.start {mode, lockstep?, seed?, scene?, paused?, timeScale?}); the host turns
	// it into a PlaySessionSpecification with its project's settings (EditorPlayController::Start).
	struct PlayStartOptions
	{
		PlayMode Mode = PlayMode::Play;
		// Ticks advance only through play.step (§13.6), and the requesting client owns lockstep.
		bool Lockstep = false;
		ClientId LockstepOwner = NoClient;
		// The session seed; nullopt: ComputeSessionSeed(Simulation.Seed, the scene's Seed).
		std::optional<uint64_t> Seed{};
		// A project-relative scene path to play instead of the open edit scene; empty: the open edit scene.
		std::string ScenePath{};
		bool Paused = false;
		double TimeScale = 1.0;             // [0, PlaySession::MaxTimeScale]
		VariantValue Parameters{};          // absent normalizes to an empty JSON object
		std::optional<bool> PauseOnError{}; // absent inherits project settings
	};

	// Everything a session needs to start. The project's settings are copied in whole, so a session never reads the project
	// again (a settings change during play takes effect at the next Play).
	struct PlaySessionSpecification
	{
		// Required, frozen, outliving the session (documented back-reference, §4.7): the context's registry.
		const TypeRegistry* Registry = nullptr;
		// The context's asset manager, outliving the session; may be null for scenes without assets. Prefab instantiation
		// and Scene.Load from scripts require it.
		AssetManager* Assets = nullptr;
		PlayMode Mode = PlayMode::Play;
		// The session seed (§4.8, §4.12): ComputeSessionSeed(Project Simulation.Seed, Scene.Seed), or play.start's seed. Seeds
		// the runtime UUIDGenerator (UUIDGenerator::CreateDeterministic) and the session's Random stream.
		uint64_t Seed = 0;
		// The project's settings (§6.1): the editor's open project, the Runtime's Game.pak Metadata "Project" document (§14.1).
		// M7 reads Simulation.FixedHz (FixedDelta = 1 / FixedHz; Create checks it against [1, FrameLoopConfig::MaxFixedHz] as
		// the project loader does), Simulation.MaxStepsPerFrame (the editor's loop, EditorPlayController::GetFrameLoopConfig),
		// Simulation.MaxEntities (>= 1, checked) and Input.Actions (PlayInput::Create); Simulation.Seed only through Seed below.
		ProjectSettings Project{};
		// The session's view in pixels, for its extraction and its scripts' view coordinates (see MapToViewCoordinates: the
		// host's game view for ordinary live play, GetDeterministicViewSize for a deterministic session; SetViewSize changes
		// it). Both >= 1.
		uint32_t ViewWidth = 1600;
		uint32_t ViewHeight = 900;
		// Test instrumentation (see IPlaySessionObserver); null in production. Outlives the session.
		IPlaySessionObserver* Observer = nullptr;
		// The host's number for this session, never repeated during the host's lifetime (EditorPlayController counts its
		// sessions from 1; the Runtime has one): what code that outlives a call recognizes its session by (a pending
		// play.step, the editor's transient play-scene undos), since a new session may reuse an ended one's address.
		uint64_t Serial = 0;
		// M12: the context's audio engine (EngineContext::GetAudioEngine), outliving the session; null: a silent session (no
		// AudioSystem). See "Audio" in PlaySession's comment.
		AudioEngine* Audio = nullptr;
		// M12: the session owns the engine's simulation time from activation to its end, in lockstep or not: a test run
		// (§10.1 "or a test run is active"; M13's FeatureTest runner sets it for every suite, ScriptedClock suites included,
		// §11.10). Without it the session owns audio time only while it is in lockstep. Ignored without an AudioSystem.
		bool OwnsAudioTime = false;
		// M13: borrowed frozen scripting registry. Null creates a session-owned registry through RegisterBindings.
		// Simulate ignores scripting services. A schema snapshot pins every FieldInfo used by this scene/VM.
		ScriptApiRegistry* ScriptApi = nullptr;
		Ref<const ScriptFieldSchemaSource> ScriptSchemas{};
		VariantValue Parameters{}; // absent normalizes to an empty JSON object
		// M20 (Docs/Decisions/0031-m20-contract.md decision 5): the scene asset the scene document came from (the editor's
		// open scene or play.start's scene, a suite's or replay's scene, the Runtime's start scene); the invalid handle for a
		// scene without an asset (an editor scene never saved). Scene.GetCurrent returns it until a Scene.Load. The document
		// itself is kept as the session's start document, which Scene.Reload reloads while the current scene is the first.
		AssetHandle SceneAsset{};
		ScriptEnvironment Environment{};
		RunModes ScriptRunMode = RunModes::Editor;
		bool TestMode = false;
		IPlaySessionTestHook* TestHook = nullptr;
		IScriptTestHost* TestHost = nullptr; // test-mode reporting from setup until teardown; outlives the session
		IPlaySessionHost* Host = nullptr;
		// M19 (Docs/Decisions/0030-m19-contract.md decisions 27 and 28): the game's native module, or null for a game without
		// one: the editor's loaded module (NativeModuleHost::GetLoadedTypes), the Runtime's linked one (GetLinkedNativeModule).
		// Prepare canonicalizes the scene's native components against it (CanonicalizeNativeComponents; an error fails the
		// session's start like any strict-load error, with the context "while starting the play session"), and a Play session
		// owns a NativeWorld over it (GetNative) whose systems run in the NativeStep phase and whose resources the state hash's
		// native block holds. Simulate ignores it, as it ignores scripts. Null: no NativeStep, no native block, every "Native."
		// component kept as written.
		Ref<const NativeModuleTypes> Native{};
	};

	// One play session. Not copyable or movable; main thread only (§4.11).
	//
	// Creation (Create, CreateFromScene): the scene document is loaded strictly (LoadOptions Strict: any error fails, §6) into a
	// new runtime scene (SceneSpecification::Runtime) with the session's UUIDGenerator, so the session's entities are
	// created in canonical order with the document's ids; TransformSystem::Update gives every entity its world matrix. No
	// step runs: GetTick() is 0. The edit scene a session was copied from is never touched (§5.6: "play then stop leaves the
	// edit scene byte-identical").
	//
	// Stepping. FixedStep() runs one tick of the §5.7 fixed step (PlaySessionPhase InterpolationSnapshot to
	// PostStepTransformUpdate) as tick GetTick() with SimStep::FromTick(tick, 1 / FixedHz), then GetTick() grows by one.
	// FrameUpdate(frame) runs the frame phase (LatchFrame to RenderExtraction) with frame.DeltaTime and frame.Alpha (the
	// scene's interpolation alpha, Scene::SetInterpolationAlpha, set before and reset to 1 after). Tick() is FixedStep() then
	// FrameUpdate with Alpha 1 and DeltaTime 1 / FixedHz: one lockstep tick (§13.6), and a ManualClock frame. The host's loop
	// drives a running session through AdvanceLoopStep and AdvanceLoopFrame, which do nothing while it is paused or in
	// lockstep; play.step drives ticks itself (Automation/Methods/PlayMethods.h).
	//
	// Render interpolation (§5.2). The interpolation snapshot (phase 0) copies every entity's WorldTransform into its
	// PreviousWorldTransform and clears InterpolationResetTag, after a TransformSystem::Update when the scene's revision
	// changed since the session's last update (a write at the safe point would otherwise be snapshotted stale and then
	// interpolated). An entity renders without interpolation (InterpolationResetTag, which render extraction also honours on
	// descendants) when, since the last snapshot:
	//   - its world matrix changed outside the fixed steps (written in the frame phase by scripts or the observer, by
	//     automation at the safe point, or by the editor): FrameTransformUpdate compares every entity's new world matrix
	//     with its value at the end of the last fixed step (or at the last frame phase), which also covers descendants;
	//   - it was created (CreateEntity, scripts in M13; it also has no PreviousWorldTransform yet) or became effectively
	//     enabled;
	//   - it was teleported (MarkTeleported: RigidBody:Teleport and Transform:Teleport in M11/M13).
	// Motion made inside the fixed steps (scripts in FixedUpdate, physics, kinematic platforms) interpolates.
	//
	// Entity cap (§5.7). A session holds at most Simulation.MaxEntities entities. CreateEntity and the hosts' creation paths
	// (CheckEntityCapacity: automation's entity.create and entity.duplicate with target "play"; scripts in M13) fail with
	// InvalidState "entity limit <N> reached" instead of creating more; nothing crashes. Entities destroyed in the current
	// tick stay in the registry until the end-of-tick flush (§5.7), so live, new and pending-destroy entities together stay
	// within twice the limit as well, however many a script creates and destroys in one tick. prefab.instantiate only
	// creates in the edit scene.
	//
	// Run state (§5.6, §13.6): Running or Paused; lockstep (owned by one automation client, or by none for in-process
	// drivers such as the FeatureTest runner); a time scale the host applies to its frame loop; a stepping flag while a
	// play.step runs (the host's loop is then unthrottled, §4.2); and the modified flag (a hot reload during ordinary play,
	// §7.5 race rule 4).
	//
	// Audio (§5.6, §5.7, §10.1, §10.2; M12). A Play session with PlaySessionSpecification::Audio owns an AudioSystem over its
	// scene (GetAudioSystem; none in Simulate mode and none without an engine). Scripts reach the AudioSource methods,
	// Audio.PlayOneShot and Audio.SetGroupVolume/GetGroupVolume through it (M13), so a game's mix ends with its session.
	//   - Start and hold. Activate builds the AudioSystem with its voices held (paused), initializes its mix before scripts,
	//     and ends with AudioSystem::Start (§5.6:
	//     "PlayOnStart audio starts"), so PlayOnStart voices exist at tick 0 with their cursors at 0. The first AudioUpdate
	//     phase releases the hold: whatever paused and lockstep state the host applies after Create (EditorPlayController,
	//     the Runtime's --paused) is in force before a voice can play on a device.
	//   - Pause. The voices are paused while the session is paused and not in lockstep (§10.2: play-mode pause; a paused
	//     session's play.step then advances only the simulation). A lockstep session's voices follow its ticks whatever its
	//     paused flag, since play.step is its only clock (§10.1: "voices advance with simulation time").
	//   - Time ownership (§10.1). The session owns the engine's simulation time (AudioEngine::BeginSimulationTime with
	//     Simulation.FixedHz) while it is in lockstep (SetLockstep(true); lockstep owned by a client or by an in-process
	//     driver) and from activation to destruction when PlaySessionSpecification::OwnsAudioTime is set (a test run). Then the device
	//     reads nothing, and each AudioUpdate phase pulls, after AudioSystem::Update, one tick of frames
	//     (AudioEngine::AdvanceSimulationTick) for every tick run since the last pull: exactly one per Tick, and 0 to
	//     MaxStepsPerFrame per frame of a ScriptedClock test run. Ticks run before the session took time are never pulled.
	//     Leaving lockstep (without OwnsAudioTime) or destroying the session gives time back (EndSimulationTime).
	//   - Order. Voices pause before time goes back to the device and resume only after the session has taken it; hosts
	//     that pause a lockstep session and leave lockstep (play.pause, a lockstep owner's disconnect) call SetPaused(true)
	//     first. Destroying the session is Stop: the AudioSystem releases every voice and restores the group volumes, and
	//     only then does time go back.
	// Audio is not simulated state: it never reaches the state hash.
	//
	// World pause (M35, Docs/Decisions/0050-m35-pause-entity-state.md decisions 1 to 4). Distinct from the run state's
	// pause (SetPaused, play.pause, play.state's Paused), which stops every tick: the session keeps ticking while its world
	// is paused. Scripts (Time.PauseWorld, Time.ResumeWorld) and automation (play.setWorldPaused) request it
	// (RequestWorldPause), and each fixed step applies the latest request before anything of it runs, so a tick runs wholly
	// paused or wholly not. While it holds:
	//   - the world's phases do not run: NativeStep, SpriteAnimation, AnimationSystem, PhysicsPreStep to Physics2DPostStep
	//     (so bodies keep their velocities for the resume) and the frame's Particles phase;
	//   - FixedUpdate, Tasks, Update and LateUpdate reach only the behaviours whose class sets RunsWhileWorldPaused (and a
	//     test case and the tasks without an instance owner): ScriptFrameState::WorldPaused;
	//   - the world's clock (GetWorldTick; Time.GetTick and Time.GetTime) holds, while GetTick counts on, and a world
	//     behaviour's tasks count the world's clock (ScriptFrameState::WorldHeldTicks);
	//   - the AudioSystem holds the voices outside the Ui group when the pause's sounds choice is Pause (SetWorldPaused);
	// and every other phase runs as before: the input, text, UI input, start flush, UI events, transform updates, destroy
	// flushes and layouts, so a menu works, deterministically from latched input. The world's systems step on the world's
	// clock (SimStep::FromTick(GetWorldTick(), ...)), which is the session's in every session that never paused. The pause,
	// its pending request and the world's clock are session state, kept across Scene.Load and Scene.Reload, and from the
	// first request to pause they form a block of the state hash (ComputeStateHash).
	//
	// Determinism (§1.3, §4.12): the scene, the state hash, the UUIDs it creates and its Random stream depend only on the
	// document, the seed, the FixedHz and the input applied per tick, in every configuration.
	class PlaySession
	{
	public:
		// The largest time scale play.setTimeScale and play.start accept.
		static constexpr double MaxTimeScale = 100.0;

		// Restricts construction to the factories; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class PlaySession;
		};

		// Use Create or Prepare followed by Activate.
		explicit PlaySession(ConstructionKey key);
		~PlaySession();

		PlaySession(const PlaySession&) = delete;
		PlaySession& operator=(const PlaySession&) = delete;

		// Project Seed ^ Scene Seed (§4.12).
		[[nodiscard]] static constexpr uint64_t ComputeSessionSeed(uint32_t projectSeed, uint32_t sceneSeed)
		{
			return static_cast<uint64_t>(projectSeed ^ sceneSeed);
		}

		// A session of the scene document `sceneDocument` (a canonical scene document: SceneSerializer::ToJson's output, or a
		// cooked scene's SceneData). Errors: InvalidArgument for a specification without a registry, a Project.Simulation.FixedHz
		// outside [1, FrameLoopConfig::MaxFixedHz], a Project.Simulation.MaxEntities of 0 or a zero view size (each naming the
		// member); the strict load's errors (Validation located at
		// their JSON pointer, UnsupportedVersion) with the context "while starting the play session"; InvalidState
		// "entity limit <N> reached" when the document holds more entities than MaxEntities; Validation from PlayInput::Create.
		// The session keeps the document as its start document, which Scene.Reload restarts from while the scene it started
		// with is current (M20, Docs/Decisions/0031-m20-contract.md decision 5): the overloads taking `const Json&` copy it,
		// those taking shared immutable data keep it without a copy (InvalidArgument for a null document).
		[[nodiscard]] static Result<Scope<PlaySession>> Create(const PlaySessionSpecification& specification, const Json& sceneDocument);
		[[nodiscard]] static Result<Scope<PlaySession>> Create(const PlaySessionSpecification& specification, Ref<const Json> sceneDocument);
		// The fallible part of Create: validates/loads the scene, input, physics and empty script VM, without gameplay
		// callbacks, voices or mixer/time ownership. A rejected or discarded candidate leaves an existing session intact.
		// Only configure run state or inspect the candidate until Activate; do not advance or invoke its script VM.
		[[nodiscard]] static Result<Scope<PlaySession>> Prepare(const PlaySessionSpecification& specification, const Json& sceneDocument);
		[[nodiscard]] static Result<Scope<PlaySession>> Prepare(const PlaySessionSpecification& specification, Ref<const Json> startDocument);
		// Once, after retiring the previous session: acquire audio, lay the scene's UI out (GetUi, M22), initialize instances
		// and run startup callbacks, then start PlayOnStart voices and lay the UI out again. Script faults use the normal error
		// stream/pause/fatal-stop path, not an outer preparation error. Create is Prepare followed by Activate. Activate itself
		// does not return a fallible result.
		void Activate();

		// The serializer copy of §5.6: SceneSerializer::ToJson(editScene) into an in-memory document, then Create, which keeps
		// the document as the session's start document (the play copy that Scene.Reload restarts from, with the edit scene's
		// unsaved edits). `editScene` is only read. Errors: those of ToJson and Create.
		[[nodiscard]] static Result<Scope<PlaySession>> CreateFromScene(const PlaySessionSpecification& specification, const Scene& editScene);

		// One fixed step (see the class comment).
		void FixedStep();
		// The frame phase (see the class comment). frame.Alpha in [0, 1] and frame.DeltaTime finite and >= 0 (asserted).
		void FrameUpdate(const FrameTime& frame);
		// FixedStep, then FrameUpdate with Alpha 1 and DeltaTime 1 / FixedHz: one lockstep tick.
		void Tick();
		// ADR 0038 decision 15 (§5.6 Scene.Load, §5.7): true when the frame must run no further fixed step,
		// since a scene transition requested in a completed tick waits for the frame's end, as lockstep's next frame phase
		// applies it after every tick. A transition requested in a fixed step belongs to that tick; one requested by a scene's
		// startup callbacks to the next tick, so it ends the frame after that tick (a frame of no step leaves it pending); one
		// requested in the frame phase or between frames applies at that frame's end and ends no steps. Every loop that runs a
		// frame's fixed steps checks it after each step and gives the steps it did not run back to its scheduler
		// (FixedStepScheduler::EndFrameAfter); a FixedStep called anyway runs nothing, as after a quit request.
		[[nodiscard]] bool EndsFrameSteps() const;

		// The host loop's drive: AdvanceLoopStep runs FixedStep and AdvanceLoopFrame runs FrameUpdate, each only while the
		// session is running and not in lockstep (paused and lockstep sessions advance only through Tick, from play.step).
		void AdvanceLoopStep();
		void AdvanceLoopFrame(const FrameTime& frame);

		// The runtime scene. Automation and the editor may change it (target "play", §13.4); runtime systems write it directly.
		[[nodiscard]] Scene& GetScene();
		[[nodiscard]] const Scene& GetScene() const;
		[[nodiscard]] PlayMode GetMode() const;
		// The tick the next FixedStep runs: the number of ticks run so far.
		[[nodiscard]] uint64_t GetTick() const;
		// 1 / Project.Simulation.FixedHz.
		[[nodiscard]] double GetFixedDelta() const;
		// The specification's copy of the project's settings.
		[[nodiscard]] const ProjectSettings& GetProjectSettings() const;
		// PlaySessionSpecification::Assets (null for a session without assets): the manager the replay start fingerprint
		// reads cooked hashes through (ComputeReplayFingerprint; Docs/Decisions/0030-m19-contract.md decision 2). A
		// documented back-reference that outlives the session.
		[[nodiscard]] AssetManager* GetAssets() const;
		// PlaySessionSpecification::ScriptSchemas (null for a session without them): the script field schemas the replay
		// start fingerprint types a Script component's asset fields with (ComputeReplayFingerprint, decision 2). Valid for
		// the session's lifetime.
		[[nodiscard]] const ScriptFieldSchemaSource* GetScriptSchemas() const;
		[[nodiscard]] uint64_t GetSeed() const;
		// PlaySessionSpecification::Serial.
		[[nodiscard]] uint64_t GetSerial() const;
		[[nodiscard]] Random& GetRandom();
		[[nodiscard]] UUIDGenerator& GetIdGenerator();
		[[nodiscard]] PlayInput& GetInput();
		[[nodiscard]] const PlayInput& GetInput() const;
		// M13: null in Simulate, otherwise the session's VM. Replaced on Scene.Load at the end of a frame. No caller may
		// retain a pointer across Tick/FrameUpdate; use session serial plus scene generation for pending operations.
		[[nodiscard]] ScriptEngine* GetScripts();
		[[nodiscard]] const ScriptEngine* GetScripts() const;
		[[nodiscard]] uint64_t GetSceneGeneration() const;
		[[nodiscard]] const Json& GetLoadParameters() const;
		// M20 (Docs/Decisions/0031-m20-contract.md decision 5): the scene asset the current scene came from, what
		// Scene.GetCurrent returns: PlaySessionSpecification::SceneAsset until a Scene.Load, then the asset it named (a
		// Scene.Reload keeps it); the invalid handle for a scene without an asset.
		[[nodiscard]] AssetHandle GetCurrentSceneAsset() const;
		// Session-owned history, including faults of scenes whose VM has been replaced. Cursors never reset on Scene.Load.
		[[nodiscard]] ScriptErrorStream& GetScriptErrors();
		[[nodiscard]] const ScriptErrorStream& GetScriptErrors() const;
		// The first script error the session published to GetScriptErrors (its scripts' faults and its session errors, OnStart's
		// included), as it was then, with the tick it occurred at; nullopt until one is. The stream moves a repeated error to
		// its back with a new ID, so its first entry is not always the first occurrence (input.record stop's verification names
		// this one, Docs/Decisions/0027-m17-contract.md A1). Like the cursors, it is never reset on Scene.Load.
		[[nodiscard]] const std::optional<ScriptError>& GetFirstScriptError() const;
		// Application.Quit records the first request. The editor stops play and the Runtime exits only after returning
		// from the current frame; a test hook also receives it for ExpectQuit. It is not a recursive destruction call.
		[[nodiscard]] std::optional<int32_t> GetQuitRequest() const;
		// The structured error of a fatal stop (§11.1: a second soft memory breach or the hard limit, or a script VM that
		// could not start), as published to GetScriptErrors; nullopt while the session runs. Once it is set, FixedStep and
		// Tick do nothing and the session stays paused until the host ends it (play.state reports it).
		[[nodiscard]] const std::optional<ScriptError>& GetFatalError() const;
		// One recorder for this session's absolute input timeline, owned by the session. Null in Simulate. Session
		// feeds it every ApplyInput result before scripts run, and the state-hash checkpoint of the boundary each fixed step
		// starts from (ReplayRecorder::CaptureCheckpoint) when a frame phase ran since the previous step, so the boundary is
		// a completed tick's; MarkModified invalidates it. It survives Scene.Load.
		[[nodiscard]] ReplayRecorder* GetRecorder();
		[[nodiscard]] const ReplayRecorder* GetRecorder() const;
		// M12: the session's audio (see "Audio" above): null in Simulate mode and without PlaySessionSpecification::Audio.
		[[nodiscard]] AudioSystem* GetAudioSystem();
		[[nodiscard]] const AudioSystem* GetAudioSystem() const;
		// M12: true while the session holds the audio engine's simulation time (see "Audio" above).
		[[nodiscard]] bool IsAudioTimeOwned() const;

		// --- Physics (M11; added by the M11 contract, Docs/Decisions/0014-m11-decisions.md decision 14) ------------------
		// The session's PhysicsSystem (§9.2 to §9.6). Create makes it right after the scene loaded, from Project.Physics
		// (gravity, layers, collisions), Project.Simulation.FixedHz and Assets, and it creates every body in canonical order
		// (§5.6 "Session setup"); its errors fail Create with the context "while starting the play session". The fixed step
		// calls its PreStep, Step and PostStep in their phases, and both destroy flushes (DestroyFlush, FrameDestroyFlush)
		// call its FlushDestroyed before Scene::FlushPendingDestroys, in Play and Simulate alike (Simulate is Play without
		// scripts and audio, §5.6). ComputeStateHash appends PhysicsSystem::AppendStateHash after the M7 values. Valid for the
		// session's lifetime.
		[[nodiscard]] PhysicsSystem& GetPhysics();
		[[nodiscard]] const PhysicsSystem& GetPhysics() const;
		// --- End of the M11 additions ------------------------------------------------------------------------------------

		// M19 (Docs/Decisions/0030-m19-contract.md decision 28): the session's native runtime (NativeWorld.h), null in
		// Simulate mode and without PlaySessionSpecification::Native. Valid for the session's lifetime.
		[[nodiscard]] NativeWorld* GetNative();
		[[nodiscard]] const NativeWorld* GetNative() const;

		// M21 (Docs/Decisions/0032-m21-contract.md decision 10): the session's 2D physics (Scene/Physics2DSystem.h), created
		// right after the PhysicsSystem (and again with it at a scene transition), from Project.Physics2D's gravity,
		// Project.Physics's layers and collisions and Project.Simulation.FixedHz (its errors fail Create like the 3D
		// system's), in Play and Simulate alike; the fixed step calls its PreStep, Step and PostStep in the Physics2D phases,
		// and both destroy flushes call its FlushDestroyed after the 3D system's. The system creates its Physics2DWorld only
		// at the first 2D body, so a session without one holds one PhysicsWorld, as before M21. Never null; valid for the
		// session's lifetime.
		[[nodiscard]] Physics2DSystem* GetPhysics2D();
		[[nodiscard]] const Physics2DSystem* GetPhysics2D() const;
		// M22 (C22.1, Docs/Decisions/0033-m22-contract.md decision 3): the session's UI (Scene/UiSystem.h): the layouts of the
		// UiLayout and FrameUiLayout phases at the session's scene resolution (the game view's, ComputeGameViewOutputMapping
		// of GetViewSize: the view's size, or a primary camera's PixelPerfectCamera's reference resolution, since M22's
		// landing), in Play and Simulate alike; ui.tree reads the current one for the play scene, and W22.2's UiInput
		// hit-tests it. Valid for the session's lifetime. Activate, and a Scene.Load or Scene.Reload's replacement of the scene
		// at the end of a frame, run UiSystem::LayoutStep of the current scene both before the scene's startup callbacks and
		// after them (not a phase: no observer reports it), the second once the world transforms the callbacks changed are up
		// to date (the milestone review), so the current layout always describes the current scene, in its OnCreate and
		// OnStart and before its first step included (the review of W22.2). C22.2 (decisions 24, 27 and 29): its interaction state, which the UiInput phase updates from the
		// step view and the project's Ui settings (and keeps empty while the scene has no canvas), the UiEvents phase's queue,
		// and the ui block of ComputeStateHash; a scene replacement resets the interaction (UiSystem::ResetInteraction) before
		// its layouts. The session's script host
		// returns it (IScriptHost::GetUi) to the Ui script module, and its game view and ExtractView pass its interaction
		// state to extraction for the widgets' tints (RenderExtractionRequest::UiInteraction).
		[[nodiscard]] UiSystem& GetUi();
		[[nodiscard]] const UiSystem& GetUi() const;

		// M23 (Docs/Decisions/0034-m23-contract.md decision 29): the session's animation (Scene/AnimationSystem.h), created
		// after the physics systems (and again with them at a scene transition, for the new scene) with the session's asset
		// manager, in Play and Simulate alike; the fixed step calls its Step in the AnimationSystem phase. Its event listener
		// is the session's ScriptEngine in Play, set wherever the physics system's is (PrepareScripts, after it creates the
		// engine at the start, a Scene.Load or a Scene.Reload, AnimationSystem::SetEventListener beside
		// PhysicsSystem::SetEventListener) and cleared wherever that one is cleared (the transition and the destructor, before
		// the engine is destroyed), so events reach scripts from the session's first tick and after every transition, and
		// never a destroyed engine; none in Simulate. Never null (a session without an asset manager gets one too, which
		// poses nothing), and valid until the next scene transition replaces it. A scene without an Animator or a SkinnedMesh
		// costs it two empty views per tick and changes nothing (decision 29).
		[[nodiscard]] AnimationSystem* GetAnimation();
		[[nodiscard]] const AnimationSystem* GetAnimation() const;

		// M26 (C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 6): the session's particles (Scene/ParticleSystem.h),
		// which the Particles frame phase steps, in Play and Simulate alike. Created with the session and kept across a scene
		// transition (its state lives in the scene's runtime components); never null once the session is created.
		[[nodiscard]] ParticleSystem* GetParticles();
		[[nodiscard]] const ParticleSystem* GetParticles() const;

		// The state hash (§13.7, §5.1): XXH64 (seed 0) over the scene's minified canonical serialization
		// (SceneSerializer::SaveToString, JsonStyle::Minified), followed by the tick, the Random state and the UUIDGenerator's
		// draw count as little-endian 64-bit values; M11 appends the physics state (§5.1 "+ physics velocities at runtime").
		// It covers the simulated world only: every entity with its id, name, parent, active flag, tags and serialized
		// components (a Script component's authored Fields, never its Luau instance state), the tick, Random, the id draws and
		// each body's position, rotation, velocities and sleep flag. Script tables (self.State, module locals), audio voices,
		// input state and rendering are outside it, so two sessions with equal hashes have equal worlds but may hold different
		// script state that has not reached the world yet (ReplayData.h, Architecture §5.1).
		// Equal for equal sessions in every configuration (§1.3); play.state, play.step and input.replay report it as 16
		// lowercase hex digits. Returns 0 only when the scene cannot be serialized (logged). M19 (decision 28): the native
		// block follows the physics block, NativeWorld::AppendStateHash (the module's resources; native components are in the
		// scene's serialization), and is empty without a native module, so a session without one hashes as before. M21
		// (Docs/Decisions/0032-m21-contract.md decision 11): the physics2d block, Physics2DSystem::AppendStateHash, follows
		// the native block and is empty without a 2D body. M22 (C22.2, Docs/Decisions/0033-m22-contract.md decision 29): the
		// ui block follows the physics2d block (ADR 0028's phase-2 ledger), UiSystem::AppendStateHash, the UI interaction
		// state (focus, hover, press, drag, the stick, the pointer over the UI) and the UI events still queued; it is empty
		// while both are, which the UiInput phase keeps so in every scene without a canvas, so such a scene hashes as before,
		// whatever its input. What interaction writes (a toggle's IsOn, a slider's Value, a scroll view's ScrollPosition) is
		// in the scene's serialization.
		// M23 adds no block (Docs/Decisions/0034-m23-contract.md decision 29): an Animator's playback state is component data,
		// which the scene's serialization covers, and its sampled pose is a function of that data and the assets.
		// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 4; phase 5's ledger: after M28's save-data block): the
		// world-pause block, empty until the session's first request to pause (RequestWorldPause), then the pause in force,
		// its sounds choice, the pending request and the world's clock (GetWorldTick).
		[[nodiscard]] uint64_t ComputeStateHash() const;

		// The entity cap (see the class comment).
		[[nodiscard]] uint32_t GetMaxEntities() const;
		// OK when `additional` more entities fit under MaxEntities, and live, new and pending-destroy entities together under
		// twice that; InvalidState "entity limit <N> reached" (with a hint) otherwise.
		[[nodiscard]] Status CheckEntityCapacity(size_t additional) const;
		// A runtime spawn (§4.8, §5.7): CheckEntityCapacity(1), then Scene::CreateEntity with the next deterministic id, as the
		// last root or the last child of `parent` (valid and in this session's scene, asserted). The entity renders without
		// interpolation until the next snapshot. Errors: InvalidState "entity limit <N> reached".
		[[nodiscard]] Result<Entity> CreateEntity(std::string_view name, Entity parent);
		[[nodiscard]] Result<Entity> CreateEntity(std::string_view name);
		// Marks `entity` (valid, in this session's scene, asserted) as teleported: it and its descendants render at their
		// current world pose until the next interpolation snapshot (§5.2).
		void MarkTeleported(Entity entity);

		// Run state (see the class comment).
		[[nodiscard]] bool IsPaused() const;
		// Also pauses or resumes the session's voices unless the session is in lockstep (see "Audio" above, M12).
		void SetPaused(bool paused);
		[[nodiscard]] double GetTimeScale() const;
		// Errors: InvalidArgument for a scale that is not finite or outside [0, MaxTimeScale].
		[[nodiscard]] Status SetTimeScale(double timeScale);

		// --- The world pause (M35, Docs/Decisions/0050-m35-pause-entity-state.md decisions 1 to 4; see "World pause") ---
		// Records a request to pause (`paused`) or resume the world, which the next fixed step applies before anything of it
		// runs; a later request before then replaces it, and one asking for what is already in force cancels the pending
		// one. `sounds` is the paused world's choice for its voices (ignored for a resume). Time.PauseWorld,
		// Time.ResumeWorld and play.setWorldPaused call it. Errors: InvalidArgument for a `sounds` value outside the enum.
		[[nodiscard]] Status RequestWorldPause(bool paused, WorldPauseSounds sounds = WorldPauseSounds::Pause);
		// The world pause the next fixed step runs with: the pending request's, else the one in force.
		[[nodiscard]] bool IsWorldPaused() const;
		// The sounds choice the next fixed step runs with: the pending pause's, else the one in force (the last pause's after
		// a resume; Pause before any).
		[[nodiscard]] WorldPauseSounds GetWorldPauseSounds() const;
		// The world's clock: the fixed steps the world ran (GetTick less the ticks the world pause held).
		[[nodiscard]] uint64_t GetWorldTick() const;
		[[nodiscard]] bool IsLockstep() const;
		// The client that owns lockstep; NoClient when the session is not in lockstep or an in-process driver owns it.
		[[nodiscard]] ClientId GetLockstepOwner() const;
		// Enters (true) or leaves (false) lockstep; `owner` is the owning client (NoClient for in-process drivers). Leaving
		// lockstep does not resume: the session keeps its paused state. With an AudioSystem, entering takes the engine's
		// simulation time and lets the voices follow the ticks, and leaving pauses them when the session is paused and gives
		// time back unless the session is a test run (see "Audio" above, M12).
		void SetLockstep(bool lockstep, ClientId owner = NoClient);
		// True while a play.step runs (the host does not throttle its frames then, §4.2).
		[[nodiscard]] bool IsStepping() const;
		void SetStepping(bool stepping);
		// §7.5 race rule 4: an asset or script reload during ordinary play marks the session modified (play.state "modified"),
		// which invalidates a recording in progress (M13). Never cleared.
		[[nodiscard]] bool IsModified() const;
		void MarkModified();

		// The game view (§5.7 frame phase step 4). RenderExtraction extracts the scene through its primary camera at the view
		// size and the frame's alpha into the session's last snapshot, unless extraction is disabled (play.step's render
		// "last" and "none" disable it for the ticks they do not render). The host renders the last snapshot. The view size
		// is also the extent of the scripts' view coordinates (Application.GetWindowSize, Camera:ScreenToWorldRay and
		// WorldToScreen; see MapToViewCoordinates). Hosts resize only ordinary live play; a deterministic session keeps
		// the view it was created with.
		void SetViewSize(uint32_t width, uint32_t height);
		[[nodiscard]] glm::uvec2 GetViewSize() const;
		void SetExtractionEnabled(bool enabled);
		[[nodiscard]] bool IsExtractionEnabled() const;
		// The snapshot of the last RenderExtraction phase (HasCamera false before the first one, and when the scene has no
		// primary camera; extraction errors are logged once per session and leave the previous snapshot).
		[[nodiscard]] const RenderSnapshot& GetLastExtraction() const;
		// How many RenderExtraction phases extracted since the session started (play.step's "rendered" counts their growth).
		[[nodiscard]] uint64_t GetExtractionCount() const;

		// A view of the session's scene as it is now, between ticks: what viewport.screenshot renders for the play scene and
		// the Runtime's --screenshot-at (§8.13). It first brings the scene's render state up to date the way the next frame
		// phase would: TransformSystem::Update when the scene's revision changed since the session's last update (the step-0
		// refresh), and InterpolationResetTag on every entity whose world matrix changed outside the fixed steps (the
		// comparison of FrameTransformUpdate), so a write made since the last step (entity.update {target: "play"} at the safe
		// point, the editor) shows at once and is never interpolated from its stale pose. Then ExtractRenderSnapshot with
		// `request` and GetViewAlpha() in place of request.Alpha (M31: and the session's rendered time at that alpha in place
		// of request.SimulationTime, as its game view's extraction passes it). The simulation state (the state hash) is
		// unchanged. Errors: those of ExtractRenderSnapshot.
		[[nodiscard]] Result<RenderSnapshot> ExtractView(const RenderExtractionRequest& request);
		// The interpolation alpha of a view between ticks (ExtractView): 1 while the session is paused or in lockstep and when
		// its last frame phase ran through Tick() (a ManualClock frame, play.step); otherwise the Alpha of the last FrameUpdate
		// (1 before the first).
		[[nodiscard]] float GetViewAlpha() const;
	private:
		// The scene, the UUIDGenerator, the Random stream, the input, the run state, the last snapshot and the bookkeeping of
		// the interpolation rules (PlaySession.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
