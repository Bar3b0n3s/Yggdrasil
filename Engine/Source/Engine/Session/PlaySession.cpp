#include "EnginePCH.h"
#include "Engine/Session/PlaySession.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/TransformSystem.h"

#include <cmath>
#include <format>
#include <utility>

namespace Engine {

	namespace {

		// The world matrix an entity had at the end of the last fixed step or frame phase: what the frame phase and
		// ExtractView compare the current one with to find transforms written outside the fixed steps (§5.2). Runtime-only
		// and private to the session: never registered, so the serializer, the state hash and the change tracker never see
		// it.
		struct SessionReferenceTransform
		{
			glm::mat4 Matrix = glm::mat4(1.0f);
		};

		// The entity was effectively disabled (HierarchyDisabledTag) at the last interpolation snapshot, so becoming enabled
		// before the next one tags it (§5.2: "entities created or enabled since the last step" are not interpolated).
		struct SessionDisabledAtSnapshotTag
		{
		};

	}

	namespace Utils {

		// InvalidArgument naming the specification member `member` that fails its rule.
		static std::unexpected<Error> MakeSpecificationError(std::string_view member, std::string message)
		{
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("{}: {}", member, message))
					.WithHint("check the project's settings (project.getSettings)"));
		}

		// The checks of PlaySession::Create on the specification (see there).
		static Status ValidateSpecification(const PlaySessionSpecification& specification)
		{
			if (specification.Registry == nullptr)
				return MakeSpecificationError("Registry", "a play session needs the context's type registry");
			const SimulationSettings& simulation = specification.Project.Simulation;
			if (simulation.FixedHz < 1 || simulation.FixedHz > FrameLoopConfig::MaxFixedHz)
			{
				return MakeSpecificationError("Project.Simulation.FixedHz",
					std::format("the fixed rate must be 1 to {} Hz (got {})", FrameLoopConfig::MaxFixedHz, simulation.FixedHz));
			}
			if (simulation.MaxStepsPerFrame < 1)
				return MakeSpecificationError("Project.Simulation.MaxStepsPerFrame", "a frame must be allowed at least 1 step (got 0)");
			if (simulation.MaxEntities < 1)
				return MakeSpecificationError("Project.Simulation.MaxEntities", "a play session must hold at least 1 entity (got 0)");
			if (specification.ViewWidth < 1 || specification.ViewHeight < 1)
			{
				return MakeSpecificationError("ViewWidth, ViewHeight",
					std::format("the game view must be at least 1 x 1 pixels (got {} x {})", specification.ViewWidth, specification.ViewHeight));
			}
			return {};
		}

		// The entity cap's error (§5.7).
		static std::unexpected<Error> MakeEntityLimitError(uint32_t limit, size_t count, size_t additional)
		{
			return std::unexpected(Error(ErrorCode::InvalidState,
				std::format("entity limit {} reached: the play session holds {} entities and {} more were asked for", limit, count, additional))
					.WithHint("raise the project setting Simulation.MaxEntities, or destroy entities first"));
		}

		// The session's physics (M11, PlaySession::GetPhysics): the project's PhysicsSettings, its FixedHz and the session's
		// asset manager, over the session's runtime scene.
		static PhysicsSystemSpecification MakePhysicsSpecification(const PlaySessionSpecification& specification, Scene& scene)
		{
			const PhysicsSettings& physics = specification.Project.Physics;
			PhysicsSystemSpecification result;
			result.RuntimeScene = &scene;
			result.Gravity = physics.Gravity;
			result.Layers = physics.Layers;
			result.Collisions = physics.Collisions;
			result.FixedHz = specification.Project.Simulation.FixedHz;
			result.Assets = specification.Assets;
			return result;
		}

		// The entity cap's error for a scene that holds more entities than a session may (§5.7), with the start's context.
		static std::unexpected<Error> MakeSceneOverLimitError(uint32_t limit, size_t count)
		{
			return std::unexpected(Error(ErrorCode::InvalidState, std::format("entity limit {} reached: the scene to play holds {} entities", limit, count))
					.WithHint("raise the project setting Simulation.MaxEntities, or play a smaller scene")
					.WithContext("while starting the play session"));
		}

	}

	struct PlaySession::State
	{
		// The specification's back-references and settings.
		const TypeRegistry* Registry = nullptr;
		AssetManager* Assets = nullptr;
		IPlaySessionObserver* Observer = nullptr;
		PlayMode Mode = PlayMode::Play;
		uint64_t Seed = 0;
		uint64_t Serial = 0;
		ProjectSettings Project;
		double FixedDelta = 1.0 / 60.0;
		// The session's deterministic UUIDs and random stream (§4.8, §4.12), declared before the scene that refers to the
		// generator, so the scene is destroyed first.
		UUIDGenerator IdGenerator = UUIDGenerator::CreateDeterministic(0);
		Random Stream{ 0 };
		Scope<Scene> RuntimeScene;
		// The scene's physics (M11), declared after the scene it refers to, so it is destroyed first.
		Scope<PhysicsSystem> Physics;
		PlayInput Input;
		uint64_t Tick = 0;       // the next tick
		uint64_t FrameIndex = 0; // frame phases run so far
		// The scene's revision at the session's last TransformSystem::Update: a different revision means a transform may
		// have been written since (automation at the safe point), so the world matrices are refreshed before they are read.
		uint64_t TransformRevision = 0;
		// Run state.
		bool Paused = false;
		bool Lockstep = false;
		ClientId LockstepOwner = NoClient;
		double TimeScale = 1.0;
		bool Stepping = false;
		bool Modified = false;
		// The game view.
		uint32_t ViewWidth = 1;
		uint32_t ViewHeight = 1;
		bool ExtractionEnabled = true;
		RenderSnapshot LastExtraction{};
		uint64_t ExtractionCount = 0;
		bool ExtractionErrorLogged = false;
		// GetViewAlpha: the last frame phase's alpha, and whether it ran through Tick().
		float LastFrameAlpha = 1.0f;
		bool LastFrameFromTick = false;

		// The observer's notification at the start of `phase` (IPlaySessionObserver).
		void EnterPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) const;
		// TransformSystem::Update, remembering the revision it saw.
		void UpdateTransforms();
		// §5.7 step 0 (see PlaySessionPhase::InterpolationSnapshot).
		void TakeInterpolationSnapshot();
		// Records every entity's world matrix as the reference of TagWritesOutsideSteps.
		void RecordReferenceTransforms();
		// Tags (InterpolationResetTag) every entity whose world matrix differs from its reference, has none (created since),
		// or became effectively enabled since the last snapshot; with `record`, the current matrices become the references.
		void TagWritesOutsideSteps(bool record);
		// The step-0 refresh of a view between ticks: world matrices brought up to date and outside writes tagged.
		void RefreshViewState();
		// The frame phase (§5.7) with `frame`.
		void RunFramePhase(PlaySession& session, const FrameTime& frame);
		// The RenderExtraction phase's work: the game view through the primary camera at `alpha`.
		void ExtractGameView(float alpha);
	};

	void PlaySession::State::EnterPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) const
	{
		if (Observer != nullptr)
			Observer->OnPhase(session, phase, tick);
	}

	void PlaySession::State::UpdateTransforms()
	{
		TransformSystem::Update(*RuntimeScene);
		TransformRevision = RuntimeScene->GetRevision();
	}

	void PlaySession::State::TakeInterpolationSnapshot()
	{
		if (RuntimeScene->GetRevision() != TransformRevision)
			UpdateTransforms();
		entt::registry& registry = RuntimeScene->GetRegistry();
		for (const auto [entity, world] : registry.view<WorldTransformComponent>().each())
			registry.emplace_or_replace<PreviousWorldTransformComponent>(entity, world.Matrix);
		registry.clear<InterpolationResetTag>();
		registry.clear<SessionDisabledAtSnapshotTag>();
		for (const entt::entity entity : registry.view<HierarchyDisabledTag>())
			registry.emplace<SessionDisabledAtSnapshotTag>(entity);
	}

	void PlaySession::State::RecordReferenceTransforms()
	{
		entt::registry& registry = RuntimeScene->GetRegistry();
		for (const auto [entity, world] : registry.view<WorldTransformComponent>(entt::exclude<PendingDestroyTag>).each())
			registry.emplace_or_replace<SessionReferenceTransform>(entity, world.Matrix);
	}

	void PlaySession::State::TagWritesOutsideSteps(bool record)
	{
		entt::registry& registry = RuntimeScene->GetRegistry();
		for (const auto [entity, world] : registry.view<WorldTransformComponent>(entt::exclude<PendingDestroyTag>).each())
		{
			const SessionReferenceTransform* reference = registry.try_get<SessionReferenceTransform>(entity);
			if (reference == nullptr || reference->Matrix != world.Matrix)
				registry.emplace_or_replace<InterpolationResetTag>(entity);
			if (record)
				registry.emplace_or_replace<SessionReferenceTransform>(entity, world.Matrix);
		}
		for (const entt::entity entity : registry.view<SessionDisabledAtSnapshotTag>(entt::exclude<HierarchyDisabledTag, PendingDestroyTag>))
			registry.emplace_or_replace<InterpolationResetTag>(entity);
	}

	void PlaySession::State::RefreshViewState()
	{
		if (RuntimeScene->GetRevision() != TransformRevision)
			UpdateTransforms();
		TagWritesOutsideSteps(false);
	}

	void PlaySession::State::RunFramePhase(PlaySession& session, const FrameTime& frame)
	{
		const uint64_t tick = Tick;
		const auto alpha = static_cast<float>(frame.Alpha);
		Scene& scene = *RuntimeScene;
		const bool play = Mode == PlayMode::Play;
		scene.SetInterpolationAlpha(alpha);

		EnterPhase(session, PlaySessionPhase::LatchFrame, tick);
		Input.LatchFrame();
		if (play)
		{
			EnterPhase(session, PlaySessionPhase::FrameStartFlush, tick);
			EnterPhase(session, PlaySessionPhase::Update, tick);
			EnterPhase(session, PlaySessionPhase::LateUpdate, tick);
		}
		EnterPhase(session, PlaySessionPhase::FrameDestroyFlush, tick);
		Physics->FlushDestroyed(tick);
		scene.FlushPendingDestroys();
		EnterPhase(session, PlaySessionPhase::FrameTransformUpdate, tick);
		UpdateTransforms();
		TagWritesOutsideSteps(true);
		if (play)
			EnterPhase(session, PlaySessionPhase::AudioUpdate, tick);
		EnterPhase(session, PlaySessionPhase::RenderExtraction, tick);
		if (ExtractionEnabled)
			ExtractGameView(alpha);

		scene.SetInterpolationAlpha(1.0f);
		LastFrameAlpha = alpha;
		++FrameIndex;
	}

	void PlaySession::State::ExtractGameView(float alpha)
	{
		++ExtractionCount;
		const RenderExtractionRequest request{ .Camera = RenderCameraSource::Primary,
			.CameraEntity = UUID(),
			.ExplicitCamera = ExplicitRenderCamera{},
			.Width = ViewWidth,
			.Height = ViewHeight,
			.Alpha = alpha };
		Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(*RuntimeScene, request);
		if (snapshot)
		{
			LastExtraction = std::move(*snapshot);
			return;
		}
		// The game view keeps its previous snapshot; the session keeps running. Reported once, not every frame.
		if (!ExtractionErrorLogged)
		{
			ENGINE_CORE_WARN("The game view of the play session could not be extracted (it keeps showing its previous view): {}", snapshot.error());
			ExtractionErrorLogged = true;
		}
	}

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

	Result<Scope<PlaySession>> PlaySession::Create(const PlaySessionSpecification& specification, const Json& sceneDocument)
	{
		ENGINE_TRY(Utils::ValidateSpecification(specification));
		const uint32_t maxEntities = specification.Project.Simulation.MaxEntities;

		// A document over the cap does not start (§5.7); counted before anything is loaded.
		if (const std::optional<JsonReader> entities = JsonReader(sceneDocument).FindMember("Entities"); entities.has_value() && entities->IsArray())
		{
			const size_t count = entities->GetArraySize().value_or(0);
			if (count > maxEntities)
				return Utils::MakeSceneOverLimitError(maxEntities, count);
		}

		Result<PlayInput> input = PlayInput::Create(specification.Project.Input);
		if (!input)
			return std::unexpected(std::move(input).error().WithContext("while starting the play session"));

		Scope<PlaySession> session = CreateScope<PlaySession>(ConstructionKey());
		State& state = *session->m_State;
		state.Registry = specification.Registry;
		state.Assets = specification.Assets;
		state.Observer = specification.Observer;
		state.Mode = specification.Mode;
		state.Seed = specification.Seed;
		state.Serial = specification.Serial;
		state.Project = specification.Project;
		state.FixedDelta = 1.0 / static_cast<double>(specification.Project.Simulation.FixedHz);
		state.IdGenerator = UUIDGenerator::CreateDeterministic(specification.Seed);
		state.Stream.Seed(specification.Seed);
		state.Input = std::move(*input);
		state.ViewWidth = specification.ViewWidth;
		state.ViewHeight = specification.ViewHeight;

		// §5.6: the serializer path the exported Runtime uses, strict, into a runtime scene with the session's generator.
		state.RuntimeScene = Scene::Create(
			SceneSpecification{ .Name = "Play", .Seed = 0, .Registry = specification.Registry, .IdGenerator = &state.IdGenerator, .Runtime = true });
		LoadOptions options;
		options.Mode = LoadMode::Strict;
		LoadReport report;
		Status loaded = SceneSerializer::FromJson(*state.RuntimeScene, sceneDocument, options, report);
		if (!loaded)
			return std::unexpected(std::move(loaded).error().WithContext("while starting the play session"));
		const size_t loadedCount = state.RuntimeScene->GetEntityCount();
		if (loadedCount > maxEntities)
			return Utils::MakeSceneOverLimitError(maxEntities, loadedCount);

		state.UpdateTransforms();
		// M11: the bodies, in canonical order (§5.6 "Session setup").
		Result<Scope<PhysicsSystem>> physics = PhysicsSystem::Create(Utils::MakePhysicsSpecification(specification, *state.RuntimeScene));
		if (!physics)
			return std::unexpected(std::move(physics).error().WithContext("while starting the play session"));
		state.Physics = std::move(*physics);
		state.RecordReferenceTransforms();
		return session;
	}

	Result<Scope<PlaySession>> PlaySession::CreateFromScene(const PlaySessionSpecification& specification, const Scene& editScene)
	{
		Result<Json> document = SceneSerializer::ToJson(editScene);
		if (!document)
			return std::unexpected(std::move(document).error().WithContext("while copying the edit scene for play"));
		return Create(specification, *document);
	}

	void PlaySession::FixedStep()
	{
		State& state = *m_State;
		const uint64_t tick = state.Tick;
		const SimStep step = SimStep::FromTick(tick, state.FixedDelta);
		Scene& scene = *state.RuntimeScene;
		PhysicsSystem& physics = *state.Physics;
		const bool play = state.Mode == PlayMode::Play;

		state.EnterPhase(*this, PlaySessionPhase::InterpolationSnapshot, tick);
		state.TakeInterpolationSnapshot();
		state.EnterPhase(*this, PlaySessionPhase::ApplyInput, tick);
		state.Input.ApplyTick(tick);
		if (play)
		{
			state.EnterPhase(*this, PlaySessionPhase::StartFlush, tick);
			state.EnterPhase(*this, PlaySessionPhase::FixedUpdate, tick);
			state.EnterPhase(*this, PlaySessionPhase::Tasks, tick);
		}
		state.EnterPhase(*this, PlaySessionPhase::PreStepTransformUpdate, tick);
		state.UpdateTransforms();
		state.EnterPhase(*this, PlaySessionPhase::PhysicsPreStep, tick);
		physics.PreStep(step);
		state.EnterPhase(*this, PlaySessionPhase::PhysicsStep, tick);
		physics.Step(step);
		state.EnterPhase(*this, PlaySessionPhase::PhysicsPostStep, tick);
		physics.PostStep(step);
		state.EnterPhase(*this, PlaySessionPhase::DestroyFlush, tick);
		physics.FlushDestroyed(tick);
		scene.FlushPendingDestroys();
		state.EnterPhase(*this, PlaySessionPhase::PostStepTransformUpdate, tick);
		state.UpdateTransforms();
		// Motion made inside the fixed step interpolates: the frame phase compares with the matrices as the step left them.
		state.RecordReferenceTransforms();
		state.Tick = tick + 1;
	}

	void PlaySession::FrameUpdate(const FrameTime& frame)
	{
		ENGINE_CORE_ASSERT(std::isfinite(frame.Alpha) && frame.Alpha >= 0.0 && frame.Alpha <= 1.0, "PlaySession::FrameUpdate: alpha {} is outside [0, 1]",
			frame.Alpha);
		ENGINE_CORE_ASSERT(std::isfinite(frame.DeltaTime) && frame.DeltaTime >= 0.0, "PlaySession::FrameUpdate: delta {} is not a duration",
			frame.DeltaTime);
		m_State->RunFramePhase(*this, frame);
		m_State->LastFrameFromTick = false;
	}

	void PlaySession::Tick()
	{
		FixedStep();
		State& state = *m_State;
		state.RunFramePhase(*this,
			FrameTime{ .DeltaTime = state.FixedDelta, .UnscaledDeltaTime = state.FixedDelta, .Alpha = 1.0, .FrameIndex = state.FrameIndex });
		state.LastFrameFromTick = true;
	}

	void PlaySession::AdvanceLoopStep()
	{
		if (!m_State->Paused && !m_State->Lockstep)
			FixedStep();
	}

	void PlaySession::AdvanceLoopFrame(const FrameTime& frame)
	{
		if (!m_State->Paused && !m_State->Lockstep)
			FrameUpdate(frame);
	}

	Scene& PlaySession::GetScene()
	{
		return *m_State->RuntimeScene;
	}

	const Scene& PlaySession::GetScene() const
	{
		return *m_State->RuntimeScene;
	}

	PlayMode PlaySession::GetMode() const
	{
		return m_State->Mode;
	}

	uint64_t PlaySession::GetTick() const
	{
		return m_State->Tick;
	}

	double PlaySession::GetFixedDelta() const
	{
		return m_State->FixedDelta;
	}

	const ProjectSettings& PlaySession::GetProjectSettings() const
	{
		return m_State->Project;
	}

	uint64_t PlaySession::GetSeed() const
	{
		return m_State->Seed;
	}

	uint64_t PlaySession::GetSerial() const
	{
		return m_State->Serial;
	}

	Random& PlaySession::GetRandom()
	{
		return m_State->Stream;
	}

	UUIDGenerator& PlaySession::GetIdGenerator()
	{
		return m_State->IdGenerator;
	}

	PlayInput& PlaySession::GetInput()
	{
		return m_State->Input;
	}

	const PlayInput& PlaySession::GetInput() const
	{
		return m_State->Input;
	}

	PhysicsSystem& PlaySession::GetPhysics()
	{
		return *m_State->Physics;
	}

	const PhysicsSystem& PlaySession::GetPhysics() const
	{
		return *m_State->Physics;
	}

	uint64_t PlaySession::ComputeStateHash() const
	{
		const State& state = *m_State;
		const Result<std::string> canonical = SceneSerializer::SaveToString(*state.RuntimeScene, JsonStyle::Minified);
		if (!canonical)
		{
			ENGINE_CORE_ERROR("Cannot compute the state hash of the play session at tick {}: {}", state.Tick, canonical.error());
			return 0;
		}
		XXH64Hasher hasher(0);
		hasher.Update(*canonical);
		hasher.UpdateU64(state.Tick);
		for (const uint64_t word : state.Stream.GetState())
			hasher.UpdateU64(word);
		hasher.UpdateU64(state.IdGenerator.GetDrawCount());
		state.Physics->AppendStateHash(hasher);
		return hasher.Digest();
	}

	uint32_t PlaySession::GetMaxEntities() const
	{
		return m_State->Project.Simulation.MaxEntities;
	}

	Status PlaySession::CheckEntityCapacity(size_t additional) const
	{
		const uint32_t limit = GetMaxEntities();
		const size_t count = m_State->RuntimeScene->GetEntityCount();
		if (count > limit || additional > limit - count)
			return Utils::MakeEntityLimitError(limit, count, additional);
		return {};
	}

	Result<Entity> PlaySession::CreateEntity(std::string_view name, Entity parent)
	{
		ENGINE_CORE_ASSERT(parent.IsValid() && parent.GetScene() == m_State->RuntimeScene.get(),
			"PlaySession::CreateEntity: the parent of '{}' is not an entity of the session's scene", name);
		ENGINE_TRY(CheckEntityCapacity(1));
		const Entity entity = m_State->RuntimeScene->CreateEntity(name, parent);
		m_State->RuntimeScene->GetRegistry().emplace_or_replace<InterpolationResetTag>(entity.GetHandle());
		return entity;
	}

	Result<Entity> PlaySession::CreateEntity(std::string_view name)
	{
		ENGINE_TRY(CheckEntityCapacity(1));
		const Entity entity = m_State->RuntimeScene->CreateEntity(name);
		m_State->RuntimeScene->GetRegistry().emplace_or_replace<InterpolationResetTag>(entity.GetHandle());
		return entity;
	}

	void PlaySession::MarkTeleported(Entity entity)
	{
		ENGINE_CORE_ASSERT(entity.IsValid() && entity.GetScene() == m_State->RuntimeScene.get(),
			"PlaySession::MarkTeleported needs an entity of the session's scene");
		m_State->RuntimeScene->GetRegistry().emplace_or_replace<InterpolationResetTag>(entity.GetHandle());
	}

	bool PlaySession::IsPaused() const
	{
		return m_State->Paused;
	}

	void PlaySession::SetPaused(bool paused)
	{
		m_State->Paused = paused;
	}

	double PlaySession::GetTimeScale() const
	{
		return m_State->TimeScale;
	}

	Status PlaySession::SetTimeScale(double timeScale)
	{
		if (!std::isfinite(timeScale) || timeScale < 0.0 || timeScale > MaxTimeScale)
		{
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("the time scale must be 0 to {} (got {})", MaxTimeScale, timeScale))
					.WithHint("1 is real time, 0.5 half speed, 0 stops time while frames keep running"));
		}
		m_State->TimeScale = timeScale;
		return {};
	}

	bool PlaySession::IsLockstep() const
	{
		return m_State->Lockstep;
	}

	ClientId PlaySession::GetLockstepOwner() const
	{
		return m_State->LockstepOwner;
	}

	void PlaySession::SetLockstep(bool lockstep, ClientId owner)
	{
		m_State->Lockstep = lockstep;
		m_State->LockstepOwner = lockstep ? owner : NoClient;
	}

	bool PlaySession::IsStepping() const
	{
		return m_State->Stepping;
	}

	void PlaySession::SetStepping(bool stepping)
	{
		m_State->Stepping = stepping;
	}

	bool PlaySession::IsModified() const
	{
		return m_State->Modified;
	}

	void PlaySession::MarkModified()
	{
		m_State->Modified = true;
	}

	void PlaySession::SetViewSize(uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(width >= 1 && height >= 1, "PlaySession::SetViewSize needs at least 1 x 1 pixels (got {} x {})", width, height);
		m_State->ViewWidth = width;
		m_State->ViewHeight = height;
	}

	void PlaySession::SetExtractionEnabled(bool enabled)
	{
		m_State->ExtractionEnabled = enabled;
	}

	bool PlaySession::IsExtractionEnabled() const
	{
		return m_State->ExtractionEnabled;
	}

	const RenderSnapshot& PlaySession::GetLastExtraction() const
	{
		return m_State->LastExtraction;
	}

	uint64_t PlaySession::GetExtractionCount() const
	{
		return m_State->ExtractionCount;
	}

	Result<RenderSnapshot> PlaySession::ExtractView(const RenderExtractionRequest& request)
	{
		m_State->RefreshViewState();
		RenderExtractionRequest view = request;
		view.Alpha = GetViewAlpha();
		return ExtractRenderSnapshot(*m_State->RuntimeScene, view);
	}

	float PlaySession::GetViewAlpha() const
	{
		const State& state = *m_State;
		if (state.Paused || state.Lockstep || state.LastFrameFromTick)
			return 1.0f;
		return state.LastFrameAlpha;
	}

}
