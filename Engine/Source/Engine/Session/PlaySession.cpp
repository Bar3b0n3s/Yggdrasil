#include "EnginePCH.h"
#include "Engine/Session/PlaySession.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Native/NativeComponents.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptProxy.h"
#include "Engine/Session/NativeWorld.h"
#include "Engine/Session/ReplayRecorder.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
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
			if (!specification.Parameters.IsNull() && !specification.Parameters.Get().is_object())
				return MakeSpecificationError("Parameters", "scene parameters must be a JSON object");
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

		// M19 (Docs/Decisions/0030-m19-contract.md decision 27): the start of a Play session with a native module makes the
		// scene's native components canonical against it. Its errors (a value its type rejects) fail the start like a strict
		// load's, each one an issue naming its entity; its warnings (an unregistered component, a migrated field) are logged.
		static Status CanonicalizeNativeScene(Scene& scene, const NativeModuleTypes& types)
		{
			Result<NativeSceneReport> report = CanonicalizeNativeComponents(scene, &types, false);
			if (!report)
				return std::unexpected(std::move(report).error().WithContext("while starting the play session"));
			std::vector<ErrorIssue> issues;
			for (const LoadDiagnostic& diagnostic : report->Diagnostics)
			{
				if (diagnostic.Severity != DiagnosticSeverity::Error)
				{
					ENGINE_CORE_WARN("Native component of the entity {} at {} ({}): {}", diagnostic.Entity, diagnostic.JsonPointer, diagnostic.Code, diagnostic.Message);
					continue;
				}
				issues.push_back({ .JsonPointer = diagnostic.JsonPointer,
					.Message = std::format("entity {}: {} ({})", diagnostic.Entity, diagnostic.Message, diagnostic.Code),
					.Hint = {},
					.Suggestions = {} });
			}
			if (issues.empty())
				return {};
			return std::unexpected(Error(ErrorCode::Validation, std::format("{} native component value(s) of the scene are invalid", issues.size()))
					.WithIssues(std::move(issues))
					.WithHint("fix the values (native.setComponents) or the module's types, then play again")
					.WithContext("while starting the play session"));
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

	struct PlaySession::State final : IScriptHost
	{
		explicit State(PlaySession& session)
			: Session(session)
		{
		}

		PlaySession& Session; // owning session, outlives every hosted VM and subsystem
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
		// FrameIndex during the previous fixed step (none before the first): still FrameIndex at the next step when no frame
		// phase ran in between, as when a grouping clock runs several steps in one frame (FeatureTestRunner).
		std::optional<uint64_t> PreviousStepFrame{};
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
		bool Activated = false;
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
		// M12 audio (see "Audio" in PlaySession.h). Audio is the context's engine (a documented back-reference; null in
		// Simulate mode and without an engine) and SceneAudio the session's AudioSystem, declared after the scene so it is
		// destroyed first and releases its voices while the scene exists. TestRunAudioTime is
		// PlaySessionSpecification::OwnsAudioTime; AudioTimeOwned whether the session holds the engine's simulation time, and
		// AudioTick the tick up to which the engine has pulled frames since. AudioHeld keeps the voices paused from Create to
		// the first AudioUpdate phase, so nothing plays before the host has applied its paused and lockstep state.
		AudioEngine* Audio = nullptr;
		Scope<AudioSystem> SceneAudio;
		bool TestRunAudioTime = false;
		bool AudioTimeOwned = false;
		uint64_t AudioTick = 0;
		bool AudioHeld = false;
		// M19: the native module's runtime (NativeWorld.h), in Play with PlaySessionSpecification::Native; null otherwise.
		Scope<NativeWorld> Native{};
		Scope<ScriptApiRegistry> OwnedApi{};
		ScriptApiRegistry* Api = nullptr;
		Ref<const ScriptFieldSchemaSource> Schemas{};
		Scope<ReplayRecorder> Recorder{};
		Scope<ScriptEngine> Scripts{};
		ScriptErrorStream Errors{};
		ScriptFrameState ScriptFrame{};
		ScriptEnvironment Environment{};
		RunModes ScriptRunMode = RunModes::Editor;
		bool TestMode = false;
		IPlaySessionTestHook* TestHook = nullptr;
		IScriptTestHost* TestHost = nullptr;
		IPlaySessionHost* Host = nullptr;
		uint64_t SceneGeneration = 1;
		Json Parameters = Json::object();
		std::optional<int32_t> QuitRequest{};
		bool FatalStop = false;
		std::optional<ScriptError> FatalError{}; // set with FatalStop (GetFatalError)
		std::optional<ScriptError> FirstError{}; // the session's first published script error (GetFirstScriptError)
		bool ReplacingScene = false;
		DebugDrawList DebugDraw{};
		Scope<Scene> PendingScene{};
		Json PendingParameters = Json::object();
		// M20 (Scene.Reload, Scene.GetCurrent; Docs/Decisions/0031-m20-contract.md decision 5): the document the current
		// scene started from (the session's start document until a Scene.Load, then the loaded asset's document as it was
		// then; shared and immutable), the scene asset it came from (PlaySessionSpecification::SceneAsset, then the asset
		// Scene.Load named) and its path for load diagnostics (empty for the start document, as at the session's start);
		// then the same of the pending transition, which Scene.Reload takes from the current scene.
		Ref<const Json> CurrentDocument{};
		AssetHandle CurrentSceneAsset{};
		std::string CurrentSourcePath{};
		Ref<const Json> PendingDocument{};
		AssetHandle PendingSceneAsset{};
		std::string PendingSourcePath{};

		[[nodiscard]] Scene& GetScene() override { return *RuntimeScene; }
		[[nodiscard]] const TypeRegistry& GetTypes() const override { return *Registry; }
		[[nodiscard]] AssetManager* GetAssets() override { return Assets; }
		[[nodiscard]] const IFieldSchemaSource* GetFieldSchemas() const override { return Schemas.get(); }
		[[nodiscard]] PhysicsSystem* GetPhysics() override { return Physics.get(); }
		[[nodiscard]] AudioSystem* GetAudio() override { return SceneAudio.get(); }
		[[nodiscard]] DebugDrawList* GetDebugDraw() override { return &DebugDraw; }
		[[nodiscard]] Random& GetRandom() override { return Stream; }
		[[nodiscard]] const InputState& GetInput() const override { return Input.GetDevices(); }
		[[nodiscard]] ScriptFrameState GetFrameState() const override
		{
			ScriptFrameState frame = ScriptFrame;
			frame.TimeScale = TimeScale;
			return frame;
		}
		// The host supplies the application facts; the session owns the view its scripts' screen coordinates refer to.
		[[nodiscard]] ScriptEnvironment GetEnvironment() const override
		{
			ScriptEnvironment environment = Host != nullptr ? Host->GetScriptEnvironment() : Environment;
			environment.WindowSize = glm::vec2(static_cast<float>(ViewWidth), static_cast<float>(ViewHeight));
			return environment;
		}
		[[nodiscard]] uint64_t GetSceneGeneration() const override { return SceneGeneration; }
		[[nodiscard]] const Json& GetLoadParameters() const override { return Parameters; }
		[[nodiscard]] Result<ScriptActionState> GetAction(std::string_view name) const override;
		[[nodiscard]] Result<UUID> CreateEntity(std::string_view name, UUID parent) override;
		[[nodiscard]] Result<UUID> Instantiate(AssetHandle prefab, const std::optional<glm::vec3>& position,
			const std::optional<glm::quat>& rotation, UUID parent) override;
		void MarkTeleported(UUID entity) override;
		[[nodiscard]] Status SetTimeScale(double scale) override { return Session.SetTimeScale(scale); }
		[[nodiscard]] Status SetCursorMode(CursorMode mode) override;
		[[nodiscard]] CursorMode GetCursorMode() const override { return Host != nullptr ? Host->GetScriptCursorMode() : CursorMode::Normal; }
		[[nodiscard]] Status RequestSceneLoad(AssetHandle scene, Json parameters) override;
		[[nodiscard]] Status RequestSceneReload(std::optional<Json> parameters) override;
		[[nodiscard]] AssetHandle GetCurrentScene() const override;
		void RequestQuit(int32_t exitCode) override;
		void RequestPause() override { Session.SetPaused(true); }
		void OnScriptError(const ScriptError& error, bool fatal) override;
		void OnExternalMutation(std::string_view reason) override;
		[[nodiscard]] bool IsReloadDeferred() const override { return Lockstep || TestMode || (Recorder != nullptr && Recorder->IsRecording()); }
		void OnScriptPhase(std::string_view script, std::string_view callback) override
		{
			if (Host != nullptr)
				Host->OnScriptPhase(script, callback);
		}
		// FatalStop with the session error that caused it, already published (ReportSessionError).
		void StopAfterSessionError()
		{
			FatalStop = true;
			if (!Errors.GetErrors().empty())
				FatalError = Errors.GetErrors().back();
		}
		[[nodiscard]] Status PrepareScripts();
		void StartScripts();
		void FlushDestroyed();
		// OK while the frame may still request a scene transition: one per frame, none during a replacement or after a stop
		// or quit request.
		[[nodiscard]] Status CheckSceneTransition() const;
		// Scene.Load's and Scene.Reload's request: `document` loaded strictly into a new runtime scene before the request is
		// accepted, so the caller still receives its located error, then kept with the parameters, the asset and the path as
		// the pending transition that ConsumeRequests commits at the end of the frame.
		[[nodiscard]] Status PrepareSceneTransition(Ref<const Json> document, AssetHandle asset, std::string sourcePath, Json parameters);
		// Drops the pending transition.
		void ClearPendingTransition();
		void ConsumeRequests();
		void ReportSessionError(const Error& error);

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
		// M12: the AudioUpdate phase's work (see "Audio" in PlaySession.h).
		void UpdateAudio(double deltaSeconds);
		// M12: applies the run state to the audio: the voices are paused while held, or while the session is paused and not in
		// lockstep; the session owns the engine's simulation time while it is in lockstep or a test run. Voices pause before
		// time goes back to the device and resume only after the session has taken it, so the device never plays a voice
		// the session holds.
		void ApplyAudioRunState();
		// M12: takes (true) or gives back (false) the audio engine's simulation time, when the session has audio.
		void SetAudioTimeOwned(bool owned);
	};

	Result<ScriptActionState> PlaySession::State::GetAction(std::string_view name) const
	{
		const auto action = Input.GetActions().FindAction(name);
		if (!action.has_value())
		{
			std::vector<std::string_view> names;
			for (uint32_t index = 0; index < Input.GetActions().GetActionCount(); ++index)
				names.push_back(Input.GetActions().GetDefinition(index).Name);
			return std::unexpected(Error(ErrorCode::NotFound, std::format("INPUT_UNKNOWN_ACTION: no action '{}'", name))
					.WithHint(MakeDidYouMeanHint(FuzzySuggest(name, names))));
		}
		return ScriptActionState{ .Down = Input.IsActionDown(ScriptFrame.Phase, *action),
			.Pressed = Input.WasActionPressed(ScriptFrame.Phase, *action),
			.Released = Input.WasActionReleased(ScriptFrame.Phase, *action),
			.Axis = Input.GetActionAxis(ScriptFrame.Phase, *action) };
	}

	Result<UUID> PlaySession::State::CreateEntity(std::string_view name, UUID parent)
	{
		const Entity parentEntity = RuntimeScene->FindEntityByID(parent);
		if (parent.IsValid() && !parentEntity.IsValid())
			return MakeError(ErrorCode::NotFound, "No parent entity {}", parent);
		ENGINE_TRY_ASSIGN(const Entity entity, parentEntity ? Session.CreateEntity(name, parentEntity) : Session.CreateEntity(name));
		return entity.GetUUID();
	}

	Result<UUID> PlaySession::State::Instantiate(AssetHandle prefab, const std::optional<glm::vec3>& position,
		const std::optional<glm::quat>& rotation, UUID parent)
	{
		if (Assets == nullptr)
			return MakeError(ErrorCode::InvalidState, "Prefab instantiation requires an asset manager");
		const Entity parentEntity = RuntimeScene->FindEntityByID(parent);
		if (parent.IsValid() && !parentEntity.IsValid())
			return MakeError(ErrorCode::NotFound, "No parent entity {}", parent);
		// SetWorldPosition below asserts that the position is representable under the parent; script input never reaches
		// that assertion. Validate before the prefab subtree exists, since the position cannot be checked after it.
		if (position.has_value())
			ENGINE_TRY(ScriptProxy::ValidateWorldPosition(parentEntity, *position));
		LoadReport report;
		ENGINE_TRY_ASSIGN(const Prefab asset, LoadPrefabAsset(*Assets, prefab, *Registry, Schemas.get(), report));
		ENGINE_TRY(Session.CheckEntityCapacity(asset.GetEntityIDs().size()));
		PrefabInstantiateOptions options{};
		options.PrefabHandle = prefab;
		options.RootID = IdGenerator.Next();
		options.Parent = parentEntity;
		ENGINE_TRY_ASSIGN(const Entity root, PrefabInstantiator::Instantiate(*RuntimeScene, asset, options, { .Schemas = Schemas.get() }, report));
		if (position.has_value())
			TransformSystem::SetWorldPosition(root, *position);
		if (rotation.has_value())
			TransformSystem::SetWorldRotation(root, *rotation);
		Session.MarkTeleported(root);
		return root.GetUUID();
	}

	void PlaySession::State::MarkTeleported(UUID entity)
	{
		if (const Entity target = RuntimeScene->FindEntityByID(entity))
			Session.MarkTeleported(target);
	}

	Status PlaySession::State::SetCursorMode(CursorMode mode)
	{
		if (mode != CursorMode::Normal && mode != CursorMode::Hidden && mode != CursorMode::Locked)
			return MakeError(ErrorCode::InvalidArgument, "Invalid cursor mode");
		if (Host == nullptr)
			return MakeError(ErrorCode::Unsupported, "This session has no cursor host");
		return Host->SetScriptCursorMode(mode);
	}

	Status PlaySession::State::CheckSceneTransition() const
	{
		if (ReplacingScene || PendingScene != nullptr || FatalStop || QuitRequest.has_value())
			return MakeError(ErrorCode::InvalidState, "The session already has a pending scene transition or stop");
		return {};
	}

	Status PlaySession::State::PrepareSceneTransition(Ref<const Json> document, AssetHandle asset, std::string sourcePath, Json parameters)
	{
		// Validate before accepting the request, while the caller can still receive its located error. No gameplay or
		// teardown runs here; the active protected call continues in its original scene and VM until frame end.
		Scope<Scene> candidate = Scene::Create({ .Name = "Play", .Seed = 0, .Registry = Registry, .IdGenerator = &IdGenerator, .Runtime = true });
		LoadOptions options;
		options.Schemas = Schemas.get();
		options.SourcePath = sourcePath;
		LoadReport report;
		ENGINE_TRY(SceneSerializer::FromJson(*candidate, *document, options, report));
		if (candidate->GetEntityCount() > Project.Simulation.MaxEntities)
			return Utils::MakeSceneOverLimitError(Project.Simulation.MaxEntities, candidate->GetEntityCount());
		PendingScene = std::move(candidate);
		PendingParameters = std::move(parameters);
		PendingDocument = std::move(document);
		PendingSceneAsset = asset;
		PendingSourcePath = std::move(sourcePath);
		return {};
	}

	void PlaySession::State::ClearPendingTransition()
	{
		PendingScene.reset();
		PendingParameters = Json::object();
		PendingDocument.reset();
		PendingSceneAsset = AssetHandle();
		PendingSourcePath.clear();
	}

	Status PlaySession::State::RequestSceneLoad(AssetHandle scene, Json parameters)
	{
		ENGINE_TRY(CheckSceneTransition());
		if (!parameters.is_object())
			return MakeError(ErrorCode::Validation, "Scene.Load parameters must be an object");
		if (Assets == nullptr)
			return MakeError(ErrorCode::InvalidState, "Scene.Load requires an asset manager");
		ENGINE_TRY_ASSIGN(const AssetRef<Asset> loaded, Assets->Load(scene));
		const AssetRef<SceneData> data = AssetCast<SceneData>(loaded);
		if (data == nullptr || data->Document == nullptr)
			return MakeError(ErrorCode::Validation, "Scene.Load requires a valid Scene asset");
		// The asset's document as it is now: a later Scene.Reload restarts from it even if the asset changes meanwhile.
		return PrepareSceneTransition(data->Document, scene, Assets->GetReferencePath(scene), std::move(parameters));
	}

	Status PlaySession::State::RequestSceneReload(std::optional<Json> parameters)
	{
		ENGINE_TRY(CheckSceneTransition());
		if (parameters.has_value() && !parameters->is_object())
			return MakeError(ErrorCode::Validation, "Scene.Reload parameters must be an object");
		ENGINE_CORE_ASSERT(CurrentDocument != nullptr, "A prepared play session keeps the document its scene started from");
		// The document the current scene started from, never its asset's file now: in editor play the play copy, so the
		// edit scene's unsaved edits are kept (§5.6), and what play changed since is not.
		Json kept = parameters.has_value() ? std::move(*parameters) : Parameters;
		return PrepareSceneTransition(CurrentDocument, CurrentSceneAsset, CurrentSourcePath, std::move(kept));
	}

	AssetHandle PlaySession::State::GetCurrentScene() const
	{
		return CurrentSceneAsset;
	}

	void PlaySession::State::RequestQuit(int32_t exitCode)
	{
		if (QuitRequest.has_value())
			return;
		QuitRequest = exitCode;
		if (TestHook != nullptr)
			TestHook->OnQuit(exitCode);
	}

	void PlaySession::State::OnScriptError(const ScriptError& error, bool fatal)
	{
		const ScriptError published = Errors.Add(error);
		if (!FirstError.has_value())
			FirstError = published;
		if (Host != nullptr)
			Host->OnScriptError(published, fatal);
		if (fatal)
		{
			FatalStop = true;
			if (!FatalError.has_value())
				FatalError = published;
			Session.SetPaused(true);
			if (Recorder != nullptr)
				Recorder->Invalidate("fatal script error");
		}
	}

	void PlaySession::State::OnExternalMutation(std::string_view reason)
	{
		Modified = true;
		if (Recorder != nullptr)
			Recorder->Invalidate(reason);
	}

	Status PlaySession::State::PrepareScripts()
	{
		if (Mode != PlayMode::Play)
			return {};
		if (Api == nullptr)
		{
			OwnedApi = CreateScope<ScriptApiRegistry>();
			ENGINE_TRY(RegisterBindings(*OwnedApi, *Registry));
			Api = OwnedApi.get();
		}
		ENGINE_TRY_ASSIGN(auto engine, ScriptEngine::Create({ .Host = this, .Api = Api, .Settings = Project.Scripting, .Mode = ScriptRunMode, .TestMode = TestMode, .TestHost = TestHost, .ReadOnly = false, .ClockSeconds = {} }));
		Scripts = std::move(engine);
		Physics->SetEventListener(Scripts.get());
		return {};
	}

	void PlaySession::State::StartScripts()
	{
		if (Scripts == nullptr)
			return;
		const Status initialized = Scripts->InitializeInstances();
		if (!initialized)
		{
			ReportSessionError(initialized.error());
			StopAfterSessionError();
			return;
		}
		for (const PhysicsDiagnostic& diagnostic : Physics->GetDiagnostics())
			Scripts->OnPhysicsDiagnostic(diagnostic);
		Scripts->StartPending();
	}

	void PlaySession::State::FlushDestroyed()
	{
		// OnDestroy may destroy peers; synthesized exits can do the same. Each script entry is marked before invocation,
		// so alternating these phases reaches a fixed point without rerunning a callback or using an arbitrary spin cap.
		if (Scripts != nullptr)
		{
			while (Scripts->PrepareDestroyFlush() != 0)
				Physics->FlushDestroyed(Tick);
		}
		Physics->FlushDestroyed(Tick);
		if (Scripts != nullptr)
		{
			while (Scripts->PrepareDestroyFlush() != 0)
				Physics->FlushDestroyed(Tick);
			Scripts->FinishDestroyFlush();
		}
		RuntimeScene->FlushPendingDestroys();
	}

	void PlaySession::State::ReportSessionError(const Error& error)
	{
		ScriptError report{};
		report.Script = error.GetLocation().File;
		report.Line = error.GetLocation().Line;
		report.Column = error.GetLocation().Column;
		report.JsonPointer = error.GetLocation().JsonPointer.value_or("");
		report.Message = error.ToString();
		report.Callback = "Scene.Load";
		report.Tick = Tick;
		ENGINE_CORE_ERROR("Script scene transition failed: {}", error);
		OnScriptError(report, false);
		if (TestHost != nullptr)
			TestHost->OnScriptError(report, false);
		Session.SetPaused(true);
	}

	void PlaySession::State::ConsumeRequests()
	{
		if (QuitRequest.has_value() || FatalStop)
		{
			ClearPendingTransition();
			if (FatalStop && Scripts != nullptr)
				Scripts->Stop();
			return;
		}
		if (PendingScene == nullptr)
			return;
		// Create the new physics world before discarding the old scene. Failed setup leaves the current session intact.
		TransformSystem::Update(*PendingScene);
		PlaySessionSpecification specification{};
		specification.Project = Project;
		specification.Assets = Assets;
		auto physics = PhysicsSystem::Create(Utils::MakePhysicsSpecification(specification, *PendingScene));
		if (!physics)
		{
			ReportSessionError(physics.error());
			ClearPendingTransition();
			return;
		}
		ReplacingScene = true;
		if (Scripts != nullptr)
			Scripts->Stop();
		// OnDestroy may request quit or hit a fatal budget error. Do not enter another scene after either request.
		if (QuitRequest.has_value() || FatalStop)
		{
			// The candidate physics system owns signal connections into PendingScene. Disconnect them while that
			// registry still exists, just as the committed session destroys physics before its scene.
			physics->reset();
			ClearPendingTransition();
			ReplacingScene = false;
			return;
		}
		Physics->SetEventListener(nullptr);
		Scripts.reset();
		SceneAudio.reset();
		Physics.reset();
		RuntimeScene = std::move(PendingScene);
		Physics = std::move(*physics);
		Parameters = std::move(PendingParameters);
		CurrentDocument = std::move(PendingDocument);
		CurrentSceneAsset = PendingSceneAsset;
		CurrentSourcePath = std::move(PendingSourcePath);
		ClearPendingTransition();
		ENGINE_CORE_VERIFY(SceneGeneration != std::numeric_limits<uint64_t>::max(), "Session scene generation exhausted");
		++SceneGeneration;
		DebugDraw.Clear();
		LastExtraction = {};
		ExtractionErrorLogged = false;
		UpdateTransforms();
		RecordReferenceTransforms();
		if (Audio != nullptr)
		{
			SceneAudio = CreateScope<AudioSystem>(*RuntimeScene, AudioSystemSpecification{ .Audio = Audio, .Assets = Assets });
			SceneAudio->InitializeMix();
			AudioHeld = true;
			ApplyAudioRunState();
		}
		ReplacingScene = false;
		const Status prepared = PrepareScripts();
		if (!prepared)
		{
			ReportSessionError(prepared.error());
			StopAfterSessionError();
		}
		else
			StartScripts();
		if (SceneAudio != nullptr)
			SceneAudio->Start();
	}

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
		if (FatalStop)
		{
			ConsumeRequests();
			return;
		}
		const uint64_t tick = Tick;
		const auto alpha = static_cast<float>(frame.Alpha);
		Scene& scene = *RuntimeScene;
		const bool play = Mode == PlayMode::Play;
		scene.SetInterpolationAlpha(alpha);
		ScriptFrame = { .Phase = InputPhase::Frame, .Tick = tick, .Frame = FrameIndex, .DeltaTime = frame.DeltaTime, .FixedDeltaTime = FixedDelta, .TimeScale = TimeScale, .InterpolationAlpha = alpha };

		EnterPhase(session, PlaySessionPhase::LatchFrame, tick);
		Input.LatchFrame();
		if (play)
		{
			EnterPhase(session, PlaySessionPhase::FrameStartFlush, tick);
			Scripts->StartPending();
			EnterPhase(session, PlaySessionPhase::Update, tick);
			Scripts->Update();
			EnterPhase(session, PlaySessionPhase::LateUpdate, tick);
			Scripts->LateUpdate();
		}
		EnterPhase(session, PlaySessionPhase::FrameDestroyFlush, tick);
		FlushDestroyed();
		EnterPhase(session, PlaySessionPhase::FrameTransformUpdate, tick);
		UpdateTransforms();
		TagWritesOutsideSteps(true);
		if (play)
		{
			EnterPhase(session, PlaySessionPhase::AudioUpdate, tick);
			UpdateAudio(frame.DeltaTime);
		}
		EnterPhase(session, PlaySessionPhase::RenderExtraction, tick);
		if (ExtractionEnabled)
			ExtractGameView(alpha);

		scene.SetInterpolationAlpha(1.0f);
		LastFrameAlpha = alpha;
		++FrameIndex;
		ConsumeRequests();
	}

	void PlaySession::State::ExtractGameView(float alpha)
	{
		++ExtractionCount;
		const RenderExtractionRequest request{ .Camera = RenderCameraSource::Primary,
			.CameraEntity = UUID(),
			.ExplicitCamera = ExplicitRenderCamera{},
			.Width = ViewWidth,
			.Height = ViewHeight,
			.Alpha = alpha,
			.Quality = { .ShadowMapSize = Project.Rendering.ShadowMapSize, .SsaoHalfResolution = Project.Rendering.SsaoHalfResolution } };
		Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(*RuntimeScene, request);
		if (snapshot)
		{
			snapshot->DebugDraw.Append(DebugDraw);
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

	void PlaySession::State::UpdateAudio(double deltaSeconds)
	{
		// M12 audio hook (§5.7 frame phase step 3, §10.1): the first phase releases the voices held since Create; then the
		// listener and sources; then, while the session owns audio time, one tick of frames for every tick run since the last
		// pull (one per Tick, 0 to MaxStepsPerFrame per frame of a ScriptedClock test run).
		if (SceneAudio == nullptr)
			return;
		if (AudioHeld)
		{
			AudioHeld = false;
			ApplyAudioRunState();
		}
		SceneAudio->Update(deltaSeconds);
		if (!AudioTimeOwned)
			return;
		for (; AudioTick < Tick; ++AudioTick)
			Audio->AdvanceSimulationTick();
	}

	void PlaySession::State::ApplyAudioRunState()
	{
		if (SceneAudio == nullptr)
			return;
		const bool voicesPaused = AudioHeld || (Paused && !Lockstep);
		if (voicesPaused)
			SceneAudio->SetPaused(true);
		SetAudioTimeOwned(Lockstep || TestRunAudioTime);
		if (!voicesPaused)
			SceneAudio->SetPaused(false);
	}

	void PlaySession::State::SetAudioTimeOwned(bool owned)
	{
		if (Audio == nullptr || owned == AudioTimeOwned)
			return;
		if (owned)
		{
			Audio->BeginSimulationTime(Project.Simulation.FixedHz);
			AudioTick = Tick;
		}
		else
		{
			Audio->EndSimulationTime();
		}
		AudioTimeOwned = owned;
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
			case PlaySessionPhase::NativeStep:              return "NativeStep";
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

	std::optional<glm::vec2> MapToViewCoordinates(const glm::vec2& position, const glm::vec2& displayMin, const glm::vec2& displaySize,
		const glm::uvec2& viewSize)
	{
		if (viewSize.x == 0 || viewSize.y == 0)
			return std::nullopt;
		glm::dvec2 mapped(0.0);
		for (int axis = 0; axis < 2; ++axis)
		{
			if (!std::isfinite(position[axis]) || !std::isfinite(displayMin[axis]) || !std::isfinite(displaySize[axis]) || !(displaySize[axis] > 0.0f))
				return std::nullopt;
			mapped[axis] = (static_cast<double>(position[axis]) - displayMin[axis]) / displaySize[axis] * viewSize[axis];
			if (!std::isfinite(mapped[axis]) || std::abs(mapped[axis]) > static_cast<double>(std::numeric_limits<float>::max()))
				return std::nullopt;
		}
		return glm::vec2(mapped);
	}

	glm::uvec2 GetDeterministicViewSize(const WindowSettings& window)
	{
		return { std::max(window.Width, 1u), std::max(window.Height, 1u) };
	}

	PlaySession::PlaySession(ConstructionKey /*key*/)
		: m_State(CreateScope<State>(*this))
	{
	}

	PlaySession::~PlaySession()
	{
		m_State->ReplacingScene = true;
		if (m_State->Scripts != nullptr)
			m_State->Scripts->Stop();
		if (m_State->Physics != nullptr)
			m_State->Physics->SetEventListener(nullptr);
		m_State->Scripts.reset();
		// M12 (§10.2 Stop): the AudioSystem releases its voices and restores the group volumes while the scene exists, and
		// only then does the engine's time go back to the device, so the device never plays a voice of the ended session.
		m_State->SceneAudio.reset();
		m_State->SetAudioTimeOwned(false);
	}

	Result<Scope<PlaySession>> PlaySession::Create(const PlaySessionSpecification& specification, const Json& sceneDocument)
	{
		return Create(specification, CreateRef<const Json>(sceneDocument));
	}

	Result<Scope<PlaySession>> PlaySession::Create(const PlaySessionSpecification& specification, Ref<const Json> sceneDocument)
	{
		ENGINE_TRY_ASSIGN(auto session, Prepare(specification, std::move(sceneDocument)));
		session->Activate();
		return session;
	}

	Result<Scope<PlaySession>> PlaySession::Prepare(const PlaySessionSpecification& specification, const Json& sceneDocument)
	{
		return Prepare(specification, CreateRef<const Json>(sceneDocument));
	}

	Result<Scope<PlaySession>> PlaySession::Prepare(const PlaySessionSpecification& specification, Ref<const Json> startDocument)
	{
		if (startDocument == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "a play session needs a scene document");
		const Json& sceneDocument = *startDocument;
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
		state.Api = specification.ScriptApi;
		state.Schemas = specification.ScriptSchemas;
		state.Parameters = specification.Parameters.IsNull() ? Json::object() : specification.Parameters.Get();
		state.Environment = specification.Environment;
		state.ScriptRunMode = specification.ScriptRunMode;
		state.TestMode = specification.TestMode;
		state.TestHook = specification.TestHook;
		state.TestHost = specification.TestHost;
		state.Host = specification.Host;
		state.ScriptFrame.FixedDeltaTime = state.FixedDelta;
		state.ScriptFrame.DeltaTime = state.FixedDelta;
		if (state.Mode == PlayMode::Play)
			state.Recorder = CreateScope<ReplayRecorder>();

		// §5.6: the serializer path the exported Runtime uses, strict, into a runtime scene with the session's generator.
		state.RuntimeScene = Scene::Create(
			SceneSpecification{ .Name = "Play", .Seed = 0, .Registry = specification.Registry, .IdGenerator = &state.IdGenerator, .Runtime = true });
		LoadOptions options;
		options.Mode = LoadMode::Strict;
		options.Schemas = state.Schemas.get();
		LoadReport report;
		Status loaded = SceneSerializer::FromJson(*state.RuntimeScene, sceneDocument, options, report);
		if (!loaded)
			return std::unexpected(std::move(loaded).error().WithContext("while starting the play session"));
		const size_t loadedCount = state.RuntimeScene->GetEntityCount();
		if (loadedCount > maxEntities)
			return Utils::MakeSceneOverLimitError(maxEntities, loadedCount);
		// M19 (decisions 27 and 28): Play runs the module; Simulate ignores it, as it ignores scripts.
		if (specification.Native != nullptr && specification.Mode == PlayMode::Play)
		{
			ENGINE_TRY(Utils::CanonicalizeNativeScene(*state.RuntimeScene, *specification.Native));
			ENGINE_TRY_ASSIGN(state.Native, NativeWorld::Create(specification.Native));
		}
		// M20: the start document Scene.Reload restarts from while this scene is current, and the asset it came from.
		state.CurrentDocument = std::move(startDocument);
		state.CurrentSceneAsset = specification.SceneAsset;

		state.UpdateTransforms();
		// M11: the bodies, in canonical order (§5.6 "Session setup").
		Result<Scope<PhysicsSystem>> physics = PhysicsSystem::Create(Utils::MakePhysicsSpecification(specification, *state.RuntimeScene));
		if (!physics)
			return std::unexpected(std::move(physics).error().WithContext("while starting the play session"));
		state.Physics = std::move(*physics);
		state.RecordReferenceTransforms();

		if (specification.Audio != nullptr && specification.Mode == PlayMode::Play)
		{
			state.Audio = specification.Audio;
			state.TestRunAudioTime = specification.OwnsAudioTime;
		}
		ENGINE_TRY(state.PrepareScripts());
		return session;
	}

	void PlaySession::Activate()
	{
		State& state = *m_State;
		ENGINE_CORE_VERIFY(!state.Activated, "PlaySession::Activate runs once per prepared session");
		state.Activated = true;
		// Retire the prior session before acquiring its shared engine's mixer/time. Voices remain held until AudioUpdate.
		if (state.Audio != nullptr)
		{
			state.SceneAudio = CreateScope<AudioSystem>(*state.RuntimeScene,
				AudioSystemSpecification{ .Audio = state.Audio, .Assets = state.Assets });
			state.SceneAudio->InitializeMix();
			state.AudioHeld = true;
			state.ApplyAudioRunState();
		}
		state.StartScripts();
		if (state.SceneAudio != nullptr)
			state.SceneAudio->Start();
	}

	Result<Scope<PlaySession>> PlaySession::CreateFromScene(const PlaySessionSpecification& specification, const Scene& editScene)
	{
		Result<Json> document = SceneSerializer::ToJson(editScene);
		if (!document)
			return std::unexpected(std::move(document).error().WithContext("while copying the edit scene for play"));
		// The copy's document becomes the session's start document without another copy (Scene.Reload, M20).
		return Create(specification, CreateRef<const Json>(std::move(*document)));
	}

	void PlaySession::FixedStep()
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(state.Activated, "Activate the prepared play session before stepping");
		if (state.FatalStop || state.QuitRequest.has_value())
			return;
		const uint64_t tick = state.Tick;
		const SimStep step = SimStep::FromTick(tick, state.FixedDelta);
		PhysicsSystem& physics = *state.Physics;
		const bool play = state.Mode == PlayMode::Play;
		// The checkpoint of the completed-tick boundary this step starts from, before anything of the step runs
		// (ReplayRecorder.h). A step that follows the previous one in the same frame (a grouping scripted clock) starts from
		// no such boundary, since the previous tick's frame phase has not run, so it captures none.
		const bool completedBoundary = state.PreviousStepFrame != state.FrameIndex;
		state.PreviousStepFrame = state.FrameIndex;
		if (completedBoundary && state.Recorder != nullptr && state.Recorder->IsRecording())
			state.Recorder->CaptureCheckpoint(*this);
		state.ScriptFrame = { .Phase = InputPhase::Step, .Tick = tick, .Frame = state.FrameIndex, .DeltaTime = state.FixedDelta, .FixedDeltaTime = state.FixedDelta, .TimeScale = state.TimeScale, .InterpolationAlpha = 1.0f };
		state.DebugDraw.Advance(static_cast<float>(state.FixedDelta));

		state.EnterPhase(*this, PlaySessionPhase::InterpolationSnapshot, tick);
		state.TakeInterpolationSnapshot();
		state.EnterPhase(*this, PlaySessionPhase::ApplyInput, tick);
		state.Input.ApplyTick(tick);
		if (state.Recorder != nullptr && state.Recorder->IsRecording())
		{
			const Status captured = state.Recorder->CaptureAppliedInput(tick, state.Input.GetLastAppliedEvents());
			if (!captured)
				state.Recorder->Invalidate(captured.error().ToString());
		}
		if (play)
		{
			state.EnterPhase(*this, PlaySessionPhase::StartFlush, tick);
			state.Scripts->StartPending();
			state.EnterPhase(*this, PlaySessionPhase::FixedUpdate, tick);
			state.Scripts->FixedUpdate();
			state.EnterPhase(*this, PlaySessionPhase::Tasks, tick);
			state.Scripts->ResumeTasks();
			if (state.TestHook != nullptr)
				state.TestHook->AfterTasks(*this, step);
		}
		// M19 (decision 28): the native module's systems, whose Transform writes the transform update below propagates
		// before physics. A fault in native code stops the session at once: no later phase or tick runs (NativeWorld.h).
		if (state.Native != nullptr && !state.FatalStop)
		{
			state.EnterPhase(*this, PlaySessionPhase::NativeStep, tick);
			state.Native->Step(*this, step);
			if (state.FatalStop)
				return;
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
		state.FlushDestroyed();
		state.EnterPhase(*this, PlaySessionPhase::PostStepTransformUpdate, tick);
		state.UpdateTransforms();
		// Motion made inside the fixed step interpolates: the frame phase compares with the matrices as the step left them.
		state.RecordReferenceTransforms();
		state.Tick = tick + 1;
	}

	void PlaySession::FrameUpdate(const FrameTime& frame)
	{
		ENGINE_CORE_ASSERT(m_State->Activated, "Activate the prepared play session before updating");
		ENGINE_CORE_ASSERT(std::isfinite(frame.Alpha) && frame.Alpha >= 0.0 && frame.Alpha <= 1.0, "PlaySession::FrameUpdate: alpha {} is outside [0, 1]",
			frame.Alpha);
		ENGINE_CORE_ASSERT(std::isfinite(frame.DeltaTime) && frame.DeltaTime >= 0.0, "PlaySession::FrameUpdate: delta {} is not a duration",
			frame.DeltaTime);
		m_State->RunFramePhase(*this, frame);
		m_State->LastFrameFromTick = false;
	}

	void PlaySession::Tick()
	{
		if (m_State->FatalStop || m_State->QuitRequest.has_value())
			return;
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

	AssetManager* PlaySession::GetAssets() const
	{
		return m_State->Assets;
	}

	const ScriptFieldSchemaSource* PlaySession::GetScriptSchemas() const
	{
		return m_State->Schemas.get();
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

	ScriptEngine* PlaySession::GetScripts()
	{
		return m_State->Scripts.get();
	}

	const ScriptEngine* PlaySession::GetScripts() const
	{
		return m_State->Scripts.get();
	}

	uint64_t PlaySession::GetSceneGeneration() const
	{
		return m_State->SceneGeneration;
	}

	const Json& PlaySession::GetLoadParameters() const
	{
		return m_State->Parameters;
	}

	AssetHandle PlaySession::GetCurrentSceneAsset() const
	{
		return m_State->CurrentSceneAsset;
	}

	ScriptErrorStream& PlaySession::GetScriptErrors()
	{
		return m_State->Errors;
	}

	const ScriptErrorStream& PlaySession::GetScriptErrors() const
	{
		return m_State->Errors;
	}

	const std::optional<ScriptError>& PlaySession::GetFirstScriptError() const
	{
		return m_State->FirstError;
	}

	std::optional<int32_t> PlaySession::GetQuitRequest() const
	{
		return m_State->QuitRequest;
	}

	const std::optional<ScriptError>& PlaySession::GetFatalError() const
	{
		return m_State->FatalError;
	}

	ReplayRecorder* PlaySession::GetRecorder()
	{
		return m_State->Recorder.get();
	}

	const ReplayRecorder* PlaySession::GetRecorder() const
	{
		return m_State->Recorder.get();
	}

	PhysicsSystem& PlaySession::GetPhysics()
	{
		return *m_State->Physics;
	}

	const PhysicsSystem& PlaySession::GetPhysics() const
	{
		return *m_State->Physics;
	}

	NativeWorld* PlaySession::GetNative()
	{
		return m_State->Native.get();
	}

	const NativeWorld* PlaySession::GetNative() const
	{
		return m_State->Native.get();
	}

	AudioSystem* PlaySession::GetAudioSystem()
	{
		return m_State->SceneAudio.get();
	}

	const AudioSystem* PlaySession::GetAudioSystem() const
	{
		return m_State->SceneAudio.get();
	}

	bool PlaySession::IsAudioTimeOwned() const
	{
		return m_State->AudioTimeOwned;
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
		// M19 (decision 28): the native block, the module's resources; nothing without a module.
		if (state.Native != nullptr)
			state.Native->AppendStateHash(hasher);
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
		// Destroyed entities stay in the registry until the end-of-tick flush, so creating and destroying in a loop would
		// otherwise grow it without bound within one tick: live, new and pending entities together stay within twice the
		// limit. count + additional is at most the limit here, so the subtraction cannot wrap.
		const size_t pending = m_State->RuntimeScene->GetPendingDestroyCount();
		if (pending > 2 * static_cast<size_t>(limit) - (count + additional))
		{
			return std::unexpected(Error(ErrorCode::InvalidState,
				std::format("entity limit {} reached: {} entities destroyed this tick still wait for the end-of-tick flush beside {} live ones, and {} more were asked for",
					limit, pending, count, additional))
					.WithHint("destroy and create fewer entities in one tick, or raise the project setting Simulation.MaxEntities"));
		}
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
		m_State->ApplyAudioRunState();
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
		// M12: lockstep owns the audio engine's time (§10.1), and a lockstep session's voices follow its ticks.
		m_State->ApplyAudioRunState();
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
		if (m_State->Recorder != nullptr)
			m_State->Recorder->Invalidate("play scene modified externally");
	}

	void PlaySession::SetViewSize(uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(width >= 1 && height >= 1, "PlaySession::SetViewSize needs at least 1 x 1 pixels (got {} x {})", width, height);
		m_State->ViewWidth = width;
		m_State->ViewHeight = height;
	}

	glm::uvec2 PlaySession::GetViewSize() const
	{
		return { m_State->ViewWidth, m_State->ViewHeight };
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
		ENGINE_TRY_ASSIGN(auto snapshot, ExtractRenderSnapshot(*m_State->RuntimeScene, view));
		snapshot.DebugDraw.Append(m_State->DebugDraw);
		return snapshot;
	}

	float PlaySession::GetViewAlpha() const
	{
		const State& state = *m_State;
		if (state.Paused || state.Lockstep || state.LastFrameFromTick)
			return 1.0f;
		return state.LastFrameAlpha;
	}

}
