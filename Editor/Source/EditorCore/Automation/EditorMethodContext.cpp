#include "EditorPCH.h"
#include "EditorCore/Automation/EditorMethodContext.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

namespace Engine {

	namespace Utils {

		static bool IsHexDigit(char character)
		{
			return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
		}

		static bool IsHexText(std::string_view text)
		{
			return !text.empty() && std::all_of(text.begin(), text.end(), &IsHexDigit);
		}

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
		if (reference.empty())
		{
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "an entity reference must not be empty",
				"give 16 hex digits, a unique id prefix of at least 6 hex digits, or a path such as \"/Game/Board\""));
		}

		if (reference.front() == '/')
		{
			Result<Entity> entity = scene.ResolveEntityPath(reference);
			if (!entity)
				return std::unexpected(Utils::LocateAtParam(entity.error(), pointer));
			return *entity;
		}

		if (Utils::IsHexText(reference) && reference.size() == UUID::TextLength)
		{
			const std::optional<UUID> id = UUID::FromString(reference);
			const Entity entity = id.has_value() ? scene.FindEntityByID(*id) : Entity();
			if (!entity.IsValid())
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, pointer, std::format("no entity has the id {}", reference)));
			return entity;
		}

		if (UUID::IsValidPrefix(reference))
		{
			std::vector<Entity> matches;
			scene.ForEachCanonical([&matches, reference](Entity entity)
			{
				if (entity.GetUUID().MatchesPrefix(reference))
					matches.push_back(entity);
			});
			if (matches.empty())
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, pointer, std::format("no entity id starts with '{}'", reference)));
			if (matches.size() > 1)
			{
				std::vector<ErrorIssue> candidates;
				for (const Entity match : matches)
				{
					ErrorIssue issue;
					issue.JsonPointer = std::string(pointer);
					issue.Message = std::format("candidate {} '{}'", match.GetUUID().ToString(), scene.GetEntityPath(match));
					issue.Suggestions = { match.GetUUID().ToString() };
					candidates.push_back(std::move(issue));
				}
				ErrorLocation location;
				location.JsonPointer = std::string(pointer);
				return std::unexpected(
					Error(ErrorCode::InvalidArgument, std::format("the id prefix '{}' is ambiguous: {} entities match", reference, matches.size()))
						.WithHint("give more hex digits, or the full 16-digit id")
						.WithLocation(std::move(location))
						.WithIssues(std::move(candidates)));
			}
			return matches.front();
		}

		return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is not an entity reference", reference),
			"give 16 hex digits, a unique id prefix of at least 6 hex digits, or a path starting with '/' such as \"/Game/Board\""));
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
		ENGINE_ASSERT(entity.IsValid(), "MakeEntitySummary needs a valid entity");
		EntitySummary summary;
		summary.Id = entity.GetUUID().ToString();
		summary.Name = entity.GetName();
		summary.Path = entity.GetScene()->GetEntityPath(entity);
		return summary;
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

}
