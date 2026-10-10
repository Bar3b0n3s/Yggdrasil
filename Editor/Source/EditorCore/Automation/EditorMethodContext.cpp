#include "EditorPCH.h"
#include "EditorCore/Automation/EditorMethodContext.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Automation/Methods/SceneMethods.h"
#include "Engine/Automation/Methods/SessionMethods.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"

#include <map>

namespace Engine {

	namespace {

		class EditScriptHost final : public IScriptHost
		{
		public:
			explicit EditScriptHost(EditorContext& editor)
				: m_Editor(editor)
			{
			}
			Status Initialize()
			{
				std::map<AssetHandle, AssetRef<ScriptData>> scripts;
				for (UUID id : GetScene().GetCanonicalOrder())
				{
					const auto* component = GetScene().FindEntityByID(id).TryGetComponent<ScriptComponent>();
					if (component && component->Script.IsValid())
					{
						ENGINE_TRY_ASSIGN(const AssetRef<Asset> asset, m_Editor.GetAssets().Load(component->Script.GetHandle()));
						const auto script = AssetCast<ScriptData>(asset);
						if (!script)
							return MakeError(ErrorCode::Validation, "a behaviour refers to a non-script asset");
						scripts.emplace(component->Script.GetHandle(), script);
					}
				}
				ENGINE_TRY_ASSIGN(m_Schemas, ScriptFieldSchemaSource::Create(std::move(scripts)));
				return {};
			}
			Scene& GetScene() override { return m_Editor.GetScene(); }
			const TypeRegistry& GetTypes() const override { return m_Editor.GetTypeRegistry(); }
			AssetManager* GetAssets() override { return &m_Editor.GetAssets(); }
			const IFieldSchemaSource* GetFieldSchemas() const override { return m_Schemas.get(); }
			PhysicsSystem* GetPhysics() override { return nullptr; }
			AudioSystem* GetAudio() override { return nullptr; }
			DebugDrawList* GetDebugDraw() override { return nullptr; }
			Random& GetRandom() override { return m_Random; }
			const InputState& GetInput() const override { return m_Input; }
			ScriptFrameState GetFrameState() const override { return {}; }
			ScriptEnvironment GetEnvironment() const override { return m_Editor.GetPlay().GetScriptEnvironment(); }
			uint64_t GetSceneGeneration() const override { return 1; }
			const Json& GetLoadParameters() const override { return m_Parameters; }
			Result<ScriptActionState> GetAction(std::string_view name) const override
			{
				if (!m_Editor.GetProject().GetSettings().Input.Actions.contains(std::string(name)))
					return MakeError(ErrorCode::InvalidArgument, "INPUT_UNKNOWN_ACTION: '{}'", name);
				return ScriptActionState{};
			}
			Result<UUID> CreateEntity(std::string_view, UUID) override { return ReadOnly(); }
			Result<UUID> Instantiate(AssetHandle, const std::optional<glm::vec3>&, const std::optional<glm::quat>&, UUID) override { return ReadOnly(); }
			void MarkTeleported(UUID) override {}
			Status SetTimeScale(double) override { return ReadOnly(); }
			Status SetCursorMode(CursorMode) override { return ReadOnly(); }
			CursorMode GetCursorMode() const override { return CursorMode::Normal; }
			Status RequestSceneLoad(AssetHandle, Json) override { return ReadOnly(); }
			void RequestQuit(int32_t) override {}
			void RequestPause() override {}
			void OnScriptError(const ScriptError& error, bool /*fatal*/) override
			{
				static_cast<void>(m_Editor.GetPlay().GetScriptErrors().Add(error));
				m_Editor.AppendEvent({ .Tick = error.Tick, .Type = EngineEventType::ScriptErrorRaised, .Id = error.Entity, .Path = error.Script, .Name = {}, .Message = error.Message });
			}
			void OnExternalMutation(std::string_view) override {}
			bool IsReloadDeferred() const override { return true; }
		private:
			static std::unexpected<Error> ReadOnly() { return MakeError(ErrorCode::InvalidState, "Edit evaluation is read-only"); }
			EditorContext& m_Editor; // borrowed during one synchronous edit evaluation
			Ref<const ScriptFieldSchemaSource> m_Schemas{};
			Random m_Random{ 0 };
			InputState m_Input{};
			Json m_Parameters = Json::object();
		};

	}

	EditorMethodContext::EditorMethodContext(EditorContext& editor, AutomationServer& server, MethodRequest request)
		: AutomationMethodContext(TypeKeyOf<EditorMethodContext>(), std::move(request)), m_Editor(&editor), m_Server(&server)
	{
	}

	Scope<MethodContext> EditorMethodContext::CreateNested(MethodRequest request) const
	{
		// The batch's attribution and dry-run sandbox live on the EditorContext for the whole batch (the server set them for
		// the parent request), so an op's context needs only the same editor and server.
		return CreateScope<EditorMethodContext>(*m_Editor, *m_Server, std::move(request));
	}

	const ProjectSettings* EditorMethodContext::GetProjectSettings() const
	{
		return m_Editor->HasProject() ? &m_Editor->GetProject().GetSettings() : nullptr;
	}

	std::vector<UUID> EditorMethodContext::GetSelectedEntities() const
	{
		const auto selection = m_Editor->GetSelection();
		return { selection.begin(), selection.end() };
	}

	Result<StatsGetResult> EditorMethodContext::GetHostStatistics() const
	{
		if (!m_Editor->HasProject())
			return MakeError(ErrorCode::InvalidState, "statistics require an open project");
		const auto& read = m_Server->GetSpecification().ReadHostStatistics;
		StatsGetResult result = read ? read() : StatsGetResult{};
		if (result.Views.empty())
			result.Views = { StatsViewSummary{ .Name = "scene" }, StatsViewSummary{ .Name = "game" } };
		if (const PlaySession* session = GetPlaySession())
		{
			result.Entities = static_cast<uint32_t>(session->GetScene().GetEntityCount());
			result.Bodies = session->GetPhysics().GetStats().BodyCount;
		}
		else
			result.Entities = m_Editor->HasScene() ? static_cast<uint32_t>(m_Editor->GetScene().GetEntityCount()) : 0;
		if (const AudioEngine* audio = GetAudioEngine())
			result.Voices = audio->GetStats().LiveVoices;
		if (const GraphicsDevice* device = m_Editor->GetEngine().GetGraphicsDevice())
		{
			result.MemoryAllocationCount = device->GetMemoryAllocationCount();
			result.MaxMemoryAllocationCount = device->GetInfo().MaxMemoryAllocationCount;
		}
		return result;
	}

	IAssetReferenceResolver* EditorMethodContext::GetAssetReferenceResolver() const
	{
		return &m_Server->GetAssetReferenceResolver();
	}

	Result<Ref<const IFieldSchemaSource>> EditorMethodContext::GetFieldSchemaSnapshot()
	{
		if (!m_FieldSchemas)
		{
			if (!m_Editor->HasProject())
				return MakeError(ErrorCode::InvalidState, "script field schemas require an open project");
			ENGINE_TRY_ASSIGN(m_FieldSchemas, m_Editor->GetScriptSchemaSnapshot());
		}
		return m_FieldSchemas;
	}

	Status EditorMethodContext::CompleteParameterOwners(Json& params)
	{
		if (GetRequest().Method != "entity.update")
			return {};
		const auto components = params.find("components");
		if (components == params.end() || !components->is_object())
			return {};
		const auto script = components->find("Script");
		if (script == components->end() || !script->is_object() || script->contains("Script"))
			return {};
		const auto fields = script->find("Fields");
		if (fields == script->end() || !fields->is_object() || fields->empty())
			return {};
		if (const auto removed = params.find("removeComponents"); removed != params.end() && removed->is_array())
		{
			for (const auto& name : *removed)
				if (name == Json("Script"))
					return {}; // The handler removes it before applying the new component's fields.
		}

		// Malformed structural params remain for the registry's normal located, aggregate validation.
		const JsonReader reader(params);
		const auto entity = reader.FindMember("entity");
		if (!entity)
			return {};
		const auto reference = entity->ReadString();
		if (!reference)
			return {};
		SceneTarget target = SceneTarget::Edit;
		const auto targetJson = reader.FindMember("target");
		if (targetJson)
		{
			const auto text = targetJson->ReadString();
			if (!text)
				return {};
			const EnumInfo* type = m_Editor->GetTypeRegistry().FindEnumByKey(TypeKeyOf<SceneTarget>());
			const EnumEntry* entry = type != nullptr ? type->FindByName(*text) : nullptr;
			if (entry == nullptr)
				return {};
			target = static_cast<SceneTarget>(entry->Value);
		}
		ENGINE_TRY_ASSIGN(Scene * scene, ResolveTargetScene(target, targetJson.has_value(), true));
		ENGINE_TRY_ASSIGN(const Entity owner, ResolveEntity(*scene, *reference, "/entity"));
		if (const auto* existing = owner.TryGetComponent<ScriptComponent>(); existing != nullptr && existing->Script.IsValid())
			(*script)["Script"] = existing->Script.GetHandle().ToString();
		return {};
	}

	Result<Scene*> EditorMethodContext::ResolveTargetScene(SceneTarget target, bool given, bool mutation) const
	{
		// Reads default to the play scene while playing and mutations to the edit scene (Â§13.4); an explicit "play" needs a
		// running session.
		PlaySession* session = m_Editor->GetPlay().GetSession();
		if (given && target == SceneTarget::Play)
		{
			if (session == nullptr)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidState, "/target", "not playing: there is no play scene",
					"start play mode first, or omit target to use the edit scene"));
			}
			return &session->GetScene();
		}
		if (!given && !mutation && session != nullptr)
			return &session->GetScene();
		if (!m_Editor->HasScene())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "no scene open").WithHint("open one with scene.open {path} or create one with scene.new {path}"));
		}
		return &m_Editor->GetScene();
	}

	Result<Entity> EditorMethodContext::ResolveEntity(Scene& scene, std::string_view reference, std::string_view pointer) const
	{
		return Utils::ResolveEntityReference(scene, reference, pointer);
	}

	Result<VfsPath> EditorMethodContext::ResolveProjectPath(std::string_view path, std::string_view pointer, std::string_view extension) const
	{
		if (!m_Editor->HasProject())
			return MakeError(ErrorCode::InvalidState, "no project open; call project.create or project.open");
		if (path.empty())
		{
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "a path must not be empty",
				"give a project-relative path such as \"Assets/Scenes/Main.scene\""));
		}

		constexpr std::string_view SchemeSeparator = "://";
		Result<VfsPath> parsed = path.find(SchemeSeparator) != std::string_view::npos ? VfsPath::Parse(path) : VfsPath::Create("project", path);
		if (!parsed)
		{
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
				std::format("'{}' is not a valid project path: {}", path, parsed.error().GetMessageText()),
				"give a project-relative path with '/' separators, such as \"Assets/Scenes/Main.scene\""));
		}
		if (parsed->GetScheme() != "project")
		{
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
				std::format("'{}' is outside the project: only project:// paths are accepted", path),
				"give a project-relative path such as \"Assets/Scenes/Main.scene\""));
		}
		if (parsed->IsRoot())
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "the path names the project root, not a file"));
		if (!extension.empty() && parsed->GetExtension() != extension)
		{
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' does not end with '{}'", path, extension),
				std::format("name a '{}' file", extension)));
		}
		return std::move(*parsed);
	}

	EntitySummary EditorMethodContext::MakeEntitySummary(ConstEntity entity) const
	{
		return Utils::SummarizeEntity(entity);
	}

	PlaySession* EditorMethodContext::GetPlaySession() const
	{
		return m_Editor->GetPlay().GetSession();
	}

	Status EditorMethodContext::StartPlay(const PlayStartOptions& options)
	{
		return m_Editor->GetPlay().Start(options);
	}

	Status EditorMethodContext::StopPlay()
	{
		return m_Editor->GetPlay().Stop();
	}

	Status EditorMethodContext::StartRecordingSession(const PlayStartOptions& options, bool restart)
	{
		ENGINE_TRY(Utils::RefreshAssets(*m_Editor));
		return m_Editor->GetPlay().StartRecording(options, restart);
	}

	Status EditorMethodContext::RestartForReplay(const ReplayHeader& header)
	{
		return m_Editor->GetPlay().StartReplay(header, GetRequest().Client);
	}
	void EditorMethodContext::ReleaseReplayInput(uint64_t serial)
	{
		m_Editor->GetPlay().ReleaseReplayInput(serial);
	}
	Result<ReplayHeader> EditorMethodContext::DescribeReplayHeader() const
	{
		return m_Editor->GetPlay().DescribeReplayHeader();
	}
	ScriptErrorStream* EditorMethodContext::GetScriptErrors() const
	{
		return &m_Editor->GetPlay().GetScriptErrors();
	}

	Result<AssetRef<ReplayData>> EditorMethodContext::LoadReplay(std::string_view path)
	{
		ENGINE_TRY_ASSIGN(const VfsPath canonical, Utils::ResolveAssetsPath(*this, path, "/path", ".replay"));
		ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(*this, canonical.GetPath(), "/path"));
		ENGINE_TRY_ASSIGN(const AssetRef<Asset> loaded, m_Editor->GetAssets().Load(handle));
		const auto replay = AssetCast<ReplayData>(loaded);
		if (!replay)
			return MakeError(ErrorCode::Validation, "the requested asset is not a cooked replay");
		return replay;
	}

	Result<std::string> EditorMethodContext::ValidateReplayOutput(std::string_view path) const
	{
		ENGINE_TRY_ASSIGN(const VfsPath canonical, Utils::ResolveAssetsPath(*this, path, "/path", ".replay"));
		if (m_Editor->IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "the project is read-only");
		if (m_Editor->AreAgentMutationsDenied() && m_Editor->GetCommandOrigin() == CommandOrigin::Agent)
			return MakeError(ErrorCode::PermissionDenied, "agent mutations are disabled by the editor automation policy");
		return canonical.ToString();
	}

	Result<std::string> EditorMethodContext::WriteReplay(std::string_view path, const ReplayDocument& document)
	{
		ENGINE_TRY_ASSIGN(const std::string canonical, ValidateReplayOutput(path));
		ENGINE_TRY_ASSIGN(const VfsPath output, VfsPath::Parse(canonical));
		ENGINE_TRY_ASSIGN(const std::string text, ReplayToText(document));
		ENGINE_TRY(m_Editor->WriteProjectFile(output, AsBytes(text)));
		ENGINE_TRY(Utils::RefreshAssets(*m_Editor));
		return std::string(output.GetPath());
	}

	Result<ScriptEvaluation> EditorMethodContext::EvalInEdit(std::string_view code, std::string_view reference)
	{
		if (!m_Editor->HasScene())
			return MakeError(ErrorCode::InvalidState, "no edit scene is open");
		std::optional<UUID> entity;
		if (!reference.empty())
		{
			ENGINE_TRY_ASSIGN(const Entity resolved, ResolveEntity(m_Editor->GetScene(), reference, "/entity"));
			entity = resolved.GetUUID();
		}
		EditScriptHost host(*m_Editor);
		ENGINE_TRY(host.Initialize());
		ENGINE_TRY_ASSIGN(ScriptApiRegistry * api, m_Editor->GetPlay().GetScriptApi());
		ENGINE_TRY_ASSIGN(auto engine, ScriptEngine::Create({ .Host = &host, .Api = api, .Settings = m_Editor->GetProject().GetSettings().Scripting, .Mode = RunModes::Editor, .ReadOnly = true }));
		ENGINE_TRY(engine->InitializeInstances());
		return engine->Evaluate(code, VfsPath{}, entity);
	}

	std::string EditorMethodContext::GetClientName(ClientId client) const
	{
		for (const AutomationClientInfo& info : m_Server->GetClients())
		{
			if (info.Id == client)
				return info.Name;
		}
		return {};
	}

	EventLog& EditorMethodContext::GetEventLog() const
	{
		return m_Editor->GetEngine().GetEventLog();
	}

	Result<Image> EditorMethodContext::CaptureView(const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request)
	{
		const ScreenshotCaptures& captures = m_Server->GetSpecification().Screenshots;
		if (!captures.View)
			return MakeError(ErrorCode::Unsupported, "the editor renders no views with --renderer none: start the editor without --renderer none");
		return captures.View(snapshot, request);
	}

	std::optional<ExplicitRenderCamera> EditorMethodContext::GetSceneViewCamera() const
	{
		return m_Editor->GetSceneViewCamera();
	}

	Result<std::string> EditorMethodContext::WriteOutputFile(std::string_view extension, std::span<const std::byte> bytes)
	{
		return m_Server->WriteOutputFile(extension, bytes);
	}

	SessionHostDescription EditorMethodContext::DescribeSession() const
	{
		const AutomationServerSpecification& specification = m_Server->GetSpecification();
		SessionHostDescription host;
		host.Capabilities = { "dryRun", "ifRevision", "batch", "pendingOperations", "offload" };
		if (specification.TestHooks)
			host.Capabilities.emplace_back("testHooks");
		if (m_Editor->HasProject())
		{
			host.Project.Open = true;
			host.Project.Name = m_Editor->GetProject().GetSettings().Name;
			host.Project.ProjectFile = FileSystem::PathToUtf8(m_Editor->GetProject().GetProjectFile());
			host.Project.ReadOnly = m_Editor->IsReadOnly();
		}
		host.Renderer = specification.RendererName;
		host.ReadOnly = m_Editor->HasProject() && m_Editor->IsReadOnly();
		host.Headless = specification.Headless;
		for (const AutomationClientInfo& client : m_Server->GetClients())
		{
			host.Clients.push_back(
				SessionClientSummary{ .Id = client.Id, .Name = client.Name, .Version = client.Version, .InProcess = client.InProcess });
		}
		return host;
	}

	Result<SessionShutdownResult> EditorMethodContext::Shutdown(const SessionShutdownParams& params)
	{
		EditorContext& editor = *m_Editor;
		SessionShutdownResult result;
		const bool mustSave = params.Save && editor.HasScene() && editor.IsSceneDirty();
		ENGINE_TRY(Utils::CheckDirtyScene(editor, params.Save, params.Force && !params.Save, "force"));
		if (mustSave)
		{
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::GetOwnScenePath(editor));
			ENGINE_TRY(Utils::SaveOpenScene(editor, path));
			result.Saved = true;
			result.SavedFiles.push_back(Utils::ToProjectRelative(path));
		}
		editor.RequestShutdown(0);
		return result;
	}

	SceneSummary EditorMethodContext::MakeSceneSummary(const Scene& scene) const
	{
		if (m_Editor->HasScene() && &m_Editor->GetScene() == &scene)
			return Utils::MakeSceneSummary(*m_Editor);
		// The play scene, a copy of the edit scene: its file and name, the editor's revision, and never dirty, because it is
		// never saved.
		SceneSummary summary;
		summary.Path = m_Editor->GetScenePath().has_value() ? Utils::ToProjectRelative(*m_Editor->GetScenePath()) : std::string();
		summary.Name = scene.GetName();
		summary.Revision = ToAutomationCounter(m_Editor->GetRevision());
		summary.Dirty = false;
		summary.EntityCount = ToAutomationCounter(scene.GetEntityCount());
		return summary;
	}

	AssetManager* EditorMethodContext::GetAssets() const
	{
		return &m_Editor->GetAssets();
	}

	std::chrono::steady_clock::time_point EditorMethodContext::GetWallClockTime() const
	{
		const AutomationServerSpecification& specification = m_Server->GetSpecification();
		return specification.WallClock ? specification.WallClock() : std::chrono::steady_clock::now();
	}

	AudioEngine* EditorMethodContext::GetAudioEngine() const
	{
		return m_Editor->GetEngine().GetAudioEngine();
	}

}
