#pragma once

#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Renderer/RenderSnapshot.h"
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
// Frozen by the M7 contract (Docs/Decisions/0012-m7-decisions.md decisions 2 to 5).

namespace Engine {

	class AssetManager;
	class Entity;
	class Scene;
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
		PhysicsPreStep,          // 5: create, rebuild or remove bodies; teleports; MoveKinematic (M11; empty)
		PhysicsStep,             // 6: PhysicsWorld::Step(fixedDt) (M11; empty)
		PhysicsPostStep,         // 7: dynamic poses written back; sorted collision and trigger events (M11; empty)
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
		AudioUpdate,          // 3: AudioSystem::Update (M12; empty)
		RenderExtraction      // 4: the game view's snapshot (ExtractRenderSnapshot), unless extraction is disabled
	};

	// The phase's enumerator name ("InterpolationSnapshot" ...).
	[[nodiscard]] std::string_view PlaySessionPhaseToString(PlaySessionPhase phase);

	class PlaySession;

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
		double TimeScale = 1.0; // [0, PlaySession::MaxTimeScale]
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
		[[nodiscard]] Random& GetRandom();
		[[nodiscard]] UUIDGenerator& GetIdGenerator();
		[[nodiscard]] PlayInput& GetInput();
		[[nodiscard]] const PlayInput& GetInput() const;

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
		void SetPaused(bool paused);
		[[nodiscard]] double GetTimeScale() const;
		// Errors: InvalidArgument for a scale that is not finite or outside [0, MaxTimeScale].
		[[nodiscard]] Status SetTimeScale(double timeScale);
		[[nodiscard]] bool IsLockstep() const;
		// The client that owns lockstep; NoClient when the session is not in lockstep or an in-process driver owns it.
		[[nodiscard]] ClientId GetLockstepOwner() const;
		// Enters (true) or leaves (false) lockstep; `owner` is the owning client (NoClient for in-process drivers). Leaving
		// lockstep does not resume: the session keeps its paused state.
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
