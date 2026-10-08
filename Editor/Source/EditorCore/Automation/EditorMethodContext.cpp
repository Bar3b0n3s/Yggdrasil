#include "EditorPCH.h"
#include "EditorCore/Automation/EditorMethodContext.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Automation/Methods/SceneMethods.h"
#include "Engine/Automation/Methods/SessionMethods.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

namespace Engine {

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

	IAssetReferenceResolver* EditorMethodContext::GetAssetReferenceResolver() const
	{
		return &m_Server->GetAssetReferenceResolver();
	}

	Result<Scene*> EditorMethodContext::ResolveTargetScene(SceneTarget target, bool given, bool mutation) const
	{
		// Reads default to the play scene while playing and mutations to the edit scene (§13.4); an explicit "play" needs a
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
