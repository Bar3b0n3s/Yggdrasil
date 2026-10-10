#pragma once

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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The play session (Architecture §5.6, §5.7, §13.6): a runtime copy of a scene and the simulation that runs it in the
// documented step order. The editor creates one per Play or Simulate from the open edit scene (serializer copy, §5.6), the
// Runtime one from the start scene of its Game.pak (§14.3): the same SceneSerializer::FromJson path, strict, so "works in
// the editor, broken in export" cannot happen. The session owns its runtime scene, its seeded UUIDGenerator and Random
// (§4.8, §4.12), its game input (PlayInput) and the game view's last render snapshot; physics (M11), scripts (M13) and audio
// (M12) join it in their milestones, in the hook phases below, which are empty in M7.
//
// Frozen by the M7 contract (Docs/Decisions/0012-m7-decisions.md decisions 2 to 5); the M12 contract added the audio hook
// (PlaySessionSpecification::Audio and OwnsAudioTime, GetAudioSystem, IsAudioTimeOwned and the "Audio" paragraph below;
// Docs/Decisions/0015-m12-decisions.md).

namespace Engine {

	class AssetManager;
	class AudioEngine;
	class AudioSystem;
	class Entity;
	class PhysicsSystem;
	class ReplayRecorder;
	class Scene;
	class ScriptApiRegistry;
	class ScriptEngine;
	class ScriptFieldSchemaSource;
	class IScriptTestHost;
	class TypeRegistry;
	struct RenderExtractionRequest;

	// Play runs scripts and audio; Simulate is Play without them (§5.6), viewed through the editor camera.
	enum class PlayMode : uint8_t
	{
		Play,
		Simulate
	};

	// The documented step order (§5.7), one value per phase in the order a tick runs them. A tick is one fixed step; the
	// frame phase runs once per frame, and once per tick in lockstep and with a ManualClock (Tick()).
	enum class PlaySessionPhase : uint8_t
	{
		// Fixed step.
		InterpolationSnapshot,   // 0: TransformSystem::Update when a transform changed outside the session's phases since its
								 //    last update (automation at the safe point); then PreviousWorldTransform <- WorldTransform
								 //    for every entity and InterpolationResetTag cleared (§5.2)
		ApplyInput,              // 1: the events queued for this tick, then the step view latched (PlayInput::ApplyTick)
		StartFlush,              // 2: OnStart of instances created since the last flush, canonical order (M13; empty)
		FixedUpdate,             // 3: script OnFixedUpdate, (ExecutionOrder, canonical) order (M13; empty)
		Tasks,                   // 4: Task.Wait*/Task.Delay threads due at this tick, then the test case thread (M13; empty)
		PreStepTransformUpdate,  // 5: TransformSystem::Update, before PhysicsSystem::PreStep
		PhysicsPreStep,          // 5: PhysicsSystem::PreStep: create, rebuild or remove bodies; teleports; MoveKinematic (M11)
		PhysicsStep,             // 6: PhysicsSystem::Step: PhysicsWorld::Step(fixedDt) (M11)
		PhysicsPostStep,         // 7: PhysicsSystem::PostStep: dynamic poses written back; sorted collision and trigger events (M11)
		DestroyFlush,            // 8: OnDestroy, synthesized exits, body/voice/instance removal (M11-M13), then entity destruction,
								 //    children first (Scene::FlushPendingDestroys)
		PostStepTransformUpdate, // 9: TransformSystem::Update
		// Frame phase.
		LatchFrame,           // 1: PlayInput::LatchFrame
		FrameStartFlush,      // 1: start flush (M13; empty)
		Update,               // 2: script OnUpdate(frameDt) (M13; empty)
		LateUpdate,           // 2: script OnLateUpdate(frameDt) (M13; empty)
		FrameDestroyFlush,    // 2: destroy flush, as DestroyFlush
		FrameTransformUpdate, // 2: TransformSystem::Update; entities whose world matrix changed outside the fixed steps get
							  //    InterpolationResetTag (see "Render interpolation" below)
		AudioUpdate,          // 3: AudioSystem::Update, then, while the session owns audio time, the simulation-time pulls of
							  //    the ticks run since the last pull (M12; see "Audio" below)
		RenderExtraction      // 4: the game view's snapshot (ExtractRenderSnapshot), unless extraction is disabled
	};

	// The phase's enumerator name ("InterpolationSnapshot" ...).
	[[nodiscard]] std::string_view PlaySessionPhaseToString(PlaySessionPhase phase);

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
	// through GetQuitRequest. Error publication is still retained in ScriptEngine's owned error stream.
	class IPlaySessionHost
	{
	public:
		virtual ~IPlaySessionHost() = default;
		[[nodiscard]] virtual ScriptEnvironment GetScriptEnvironment() const = 0;
		[[nodiscard]] virtual Status SetScriptCursorMode(CursorMode mode) = 0;
		[[nodiscard]] virtual CursorMode GetScriptCursorMode() const = 0;
		virtual void OnScriptError(const ScriptError& error, bool fatal) = 0;
	};

	// Instrumentation of the step order (§5.7 "documented and tested"; "PlaySession: step order matches the documented
	// sequence"): the session calls OnPhase at the start of every phase it runs, the empty hook phases of M7 included. The
	// observer may change the session's scene during FixedUpdate, Update, LateUpdate and PhysicsStep, standing in for the
	// scripts (M13) and physics (M11) that will act there ("Interpolation: script-moved entities interpolate; frame-phase
	// writes, teleports and new entities do not"). Production code installs none. Simulate mode skips the script and audio
	// phases (StartFlush, FixedUpdate, Tasks, FrameStartFlush, Update, LateUpdate, AudioUpdate) entirely, so they are not
	// reported there.
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
	// again (a settings change during play takes effect at the next Play) and the systems of later milestones find theirs
	// here (Physics in M11, Scripting in M13) without a change to this struct.
	struct PlaySessionSpecification
	{
		// Required, frozen, outliving the session (documented back-reference, §4.7): the context's registry.
		const TypeRegistry* Registry = nullptr;
		// The context's asset manager, outliving the session; may be null in M7 (prefab instantiation and Scene.Load from
		// scripts need it in M13).
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
		// The game view's size in pixels for its extraction (the window's framebuffer; the editor's game viewport from M10;
		// SetViewSize changes it). Both >= 1.
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
		// M12: the session owns the engine's simulation time from its creation to its end, in lockstep or not: a test run
		// (§10.1 "or a test run is active"; M13's FeatureTest runner sets it for every suite, ScriptedClock suites included,
		// §11.10). Without it the session owns audio time only while it is in lockstep. Ignored without an AudioSystem.
		bool OwnsAudioTime = false;
		// M13: borrowed frozen scripting registry. Null creates a session-owned registry through RegisterBindings.
		// Simulate ignores scripting services. A schema snapshot pins every FieldInfo used by this scene/VM.
		ScriptApiRegistry* ScriptApi = nullptr;
		Ref<const ScriptFieldSchemaSource> ScriptSchemas{};
		VariantValue Parameters{}; // absent normalizes to an empty JSON object
		ScriptEnvironment Environment{};
		RunModes ScriptRunMode = RunModes::Editor;
		bool TestMode = false;
		IPlaySessionTestHook* TestHook = nullptr;
		IScriptTestHost* TestHost = nullptr; // test-mode reporting from setup until teardown; outlives the session
		IPlaySessionHost* Host = nullptr;
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
	// InvalidState "entity limit <N> reached" instead of creating more; nothing crashes. prefab.instantiate only creates in
	// the edit scene.
	//
	// Run state (§5.6, §13.6): Running or Paused; lockstep (owned by one automation client, or by none for in-process
	// drivers such as the FeatureTest runner); a time scale the host applies to its frame loop; a stepping flag while a
	// play.step runs (the host's loop is then unthrottled, §4.2); and the modified flag (a hot reload during ordinary play,
	// §7.5 race rule 4).
	//
	// Audio (§5.6, §5.7, §10.1, §10.2; M12). A Play session with PlaySessionSpecification::Audio owns an AudioSystem over its
	// scene (GetAudioSystem; none in Simulate mode and none without an engine). Scripts reach the AudioSource methods,
	// Audio.PlayOneShot and Audio.SetGroupVolume/GetGroupVolume through it (M13), so a game's mix ends with its session.
	//   - Start and hold. Create builds the AudioSystem with its voices held (paused) and ends with AudioSystem::Start (§5.6:
	//     "PlayOnStart audio starts"), so PlayOnStart voices exist at tick 0 with their cursors at 0. The first AudioUpdate
	//     phase releases the hold: whatever paused and lockstep state the host applies after Create (EditorPlayController,
	//     the Runtime's --paused) is in force before a voice can play on a device.
	//   - Pause. The voices are paused while the session is paused and not in lockstep (§10.2: play-mode pause; a paused
	//     session's play.step then advances only the simulation). A lockstep session's voices follow its ticks whatever its
	//     paused flag, since play.step is its only clock (§10.1: "voices advance with simulation time").
	//   - Time ownership (§10.1). The session owns the engine's simulation time (AudioEngine::BeginSimulationTime with
	//     Simulation.FixedHz) while it is in lockstep (SetLockstep(true); lockstep owned by a client or by an in-process
	//     driver) and for its whole life when PlaySessionSpecification::OwnsAudioTime is set (a test run). Then the device
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
	// Determinism (§1.3, §4.12): the scene, the state hash, the UUIDs it creates and its Random stream depend only on the
	// document, the seed, the FixedHz and the input applied per tick, in every configuration.
	class PlaySession
	{
	public:
		// The largest time scale play.setTimeScale and play.start accept.
		static constexpr double MaxTimeScale = 100.0;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class PlaySession;
		};

		// Use Create.
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
		[[nodiscard]] static Result<Scope<PlaySession>> Create(const PlaySessionSpecification& specification, const Json& sceneDocument);

		// The serializer copy of §5.6: SceneSerializer::ToJson(editScene) into an in-memory document, then Create. `editScene`
		// is only read. Errors: those of ToJson and Create.
		[[nodiscard]] static Result<Scope<PlaySession>> CreateFromScene(const PlaySessionSpecification& specification, const Scene& editScene);

		// One fixed step (see the class comment).
		void FixedStep();
		// The frame phase (see the class comment). frame.Alpha in [0, 1] and frame.DeltaTime finite and >= 0 (asserted).
		void FrameUpdate(const FrameTime& frame);
		// FixedStep, then FrameUpdate with Alpha 1 and DeltaTime 1 / FixedHz: one lockstep tick.
		void Tick();

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
		// Application.Quit records the first request. The editor stops play and the Runtime exits only after returning
		// from the current frame; a test hook also receives it for ExpectQuit. It is not a recursive destruction call.
		[[nodiscard]] std::optional<int32_t> GetQuitRequest() const;
		// One recorder for this session's absolute input timeline, owned by the session. Null in Simulate. Session
		// feeds it every ApplyInput result before scripts run, and MarkModified invalidates it. It survives Scene.Load.
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

		// The state hash (§13.7, §5.1): XXH64 (seed 0) over the scene's minified canonical serialization
		// (SceneSerializer::SaveToString, JsonStyle::Minified), followed by the tick, the Random state and the UUIDGenerator's
		// draw count as little-endian 64-bit values; M11 appends the physics state (§5.1 "+ physics velocities at runtime").
		// Equal for equal sessions in every configuration (§1.3); play.state, play.step and input.replay report it as 16
		// lowercase hex digits. Returns 0 only when the scene cannot be serialized (logged).
		[[nodiscard]] uint64_t ComputeStateHash() const;

		// The entity cap (see the class comment).
		[[nodiscard]] uint32_t GetMaxEntities() const;
		// OK when `additional` more entities fit under MaxEntities; InvalidState "entity limit <N> reached" (with the hint to
		// raise Simulation.MaxEntities or destroy entities) otherwise.
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
		// "last" and "none" disable it for the ticks they do not render). The host renders the last snapshot.
		void SetViewSize(uint32_t width, uint32_t height);
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
		// `request` and GetViewAlpha() in place of request.Alpha. The simulation state (the state hash) is unchanged. Errors:
		// those of ExtractRenderSnapshot.
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
