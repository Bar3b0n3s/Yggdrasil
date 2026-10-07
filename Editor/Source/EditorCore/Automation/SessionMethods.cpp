#include "EditorPCH.h"
#include "EditorCore/Automation/SessionMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Platform/Process.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		// session.hello's client.name: 1 to 64 printable ASCII characters (the rule Handshake.h's ParseHelloRequest applies
		// on the I/O thread; in-process callers reach the handler without it).
		constexpr size_t MaxClientNameLength = 64;

		static std::vector<std::string> GetCapabilities(const AutomationServer& server)
		{
			std::vector<std::string> capabilities = { "dryRun", "ifRevision", "batch", "pendingOperations", "offload" };
			if (server.GetSpecification().TestHooks)
				capabilities.emplace_back("testHooks");
			return capabilities;
		}

		static SessionProjectSummary MakeSessionProjectSummary(const EditorContext& editor)
		{
			SessionProjectSummary summary;
			if (!editor.HasProject())
				return summary;
			summary.Open = true;
			summary.Name = editor.GetProject().GetSettings().Name;
			summary.ProjectFile = FileSystem::PathToUtf8(editor.GetProject().GetProjectFile());
			summary.ReadOnly = editor.IsReadOnly();
			return summary;
		}

		static Status ValidateHello(const SessionHelloParams& params)
		{
			const std::string& name = params.Client.Name;
			const bool printable = std::all_of(name.begin(), name.end(), [](char character)
			{
				return character >= 0x20 && character <= 0x7e;
			});
			if (name.empty() || name.size() > MaxClientNameLength || !printable)
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/client/name",
					std::format("client.name must be 1 to {} printable ASCII characters", MaxClientNameLength), "use a name such as \"engine-mcp\""));
			}

			const std::optional<ProtocolVersion> version = ProtocolVersion::Parse(params.Protocol);
			if (!version.has_value())
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/protocolVersion",
					std::format("'{}' is not a protocol version", params.Protocol), "use \"<major>.<minor>\", such as \"1.0\""));
			}
			if (version->Major != CurrentProtocolVersion.Major)
			{
				return MakeError(ErrorCode::Unsupported, "protocol version {} is not compatible with {}", version->ToString(),
					CurrentProtocolVersion.ToString());
			}
			return {};
		}

	}

	namespace Automation {

		Result<SessionHelloResult> SessionHello(EditorMethodContext& context, const SessionHelloParams& params)
		{
			// The I/O thread already checked the token and the version of TCP clients (Handshake.h); in-process callers have no
			// token, and get the same param checks here.
			ENGINE_TRY(Utils::ValidateHello(params));

			SessionHelloResult result;
			result.Protocol = CurrentProtocolVersion.ToString();
			result.EngineVersion = std::string(EngineVersionString);
			result.Client = context.GetRequest().Client;
			result.Capabilities = Utils::GetCapabilities(context.GetServer());
			result.Project = Utils::MakeSessionProjectSummary(context.GetEditor());
			return result;
		}

		Result<SessionInfoResult> SessionInfo(EditorMethodContext& context, const NoParams& /*params*/)
		{
			const EditorContext& editor = context.GetEditor();
			const AutomationServer& server = context.GetServer();
			SessionInfoResult result;
			result.Protocol = CurrentProtocolVersion.ToString();
			result.EngineVersion = std::string(EngineVersionString);
			result.ProcessId = Process::GetCurrentId();
			result.Capabilities = Utils::GetCapabilities(server);
			result.Project = Utils::MakeSessionProjectSummary(editor);
			result.Renderer = server.GetSpecification().RendererName;
			result.ReadOnly = editor.HasProject() && editor.IsReadOnly();
			result.Headless = server.GetSpecification().Headless;

			std::vector<AutomationClientInfo> clients = server.GetClients();
			std::sort(clients.begin(), clients.end(), [](const AutomationClientInfo& left, const AutomationClientInfo& right)
			{
				return left.Id < right.Id;
			});
			for (const AutomationClientInfo& client : clients)
			{
				SessionClientSummary summary;
				summary.Id = client.Id;
				summary.Name = client.Name;
				summary.Version = client.Version;
				summary.InProcess = client.InProcess;
				result.Clients.push_back(std::move(summary));
			}
			return result;
		}

		Result<SessionShutdownResult> SessionShutdown(EditorMethodContext& context, const SessionShutdownParams& params)
		{
			EditorContext& editor = context.GetEditor();
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

	}

	void RegisterSessionMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<SessionClientInfo>("SessionClientInfo", "Who connects: session.hello's client.")
			.Field("name", &SessionClientInfo::Name, "The client's name, 1 to 64 printable ASCII characters, such as \"engine-mcp\".")
			.Field("version", &SessionClientInfo::Version, "The client's version; free text, may be empty.");

		registry.Struct<SessionHelloParams>("SessionHelloParams", "The params of session.hello, the first request of every connection.")
			.Field("token", &SessionHelloParams::Token, "The session token from the editor's session file; never logged or echoed.")
			.Field("protocolVersion", &SessionHelloParams::Protocol, "The protocol version the client speaks, \"<major>.<minor>\".")
			.Field("client", &SessionHelloParams::Client, "Who connects.");

		registry.Struct<SessionProjectSummary>("SessionProjectSummary", "The open project, or open: false in the launcher state.")
			.Field("open", &SessionProjectSummary::Open, "Whether a project is open.")
			.Field("name", &SessionProjectSummary::Name, "The project's name (ProjectSettings.Name).")
			.Field("projectFile", &SessionProjectSummary::ProjectFile, "The absolute path of the .eproj, with '/' separators.")
			.Field("readOnly", &SessionProjectSummary::ReadOnly, "Whether the project was opened read-only (--read-only).");

		registry.Struct<SessionHelloResult>("SessionHelloResult", "The session session.hello opened.")
			.Field("protocolVersion", &SessionHelloResult::Protocol, "The protocol version the server speaks.")
			.Field("engineVersion", &SessionHelloResult::EngineVersion, "The engine's version.")
			.Field("clientId", &SessionHelloResult::Client, "This connection's client id, unique for the server's lifetime.")
			.Field("capabilities", &SessionHelloResult::Capabilities,
				"The optional protocol features the server offers: dryRun, ifRevision, batch, pendingOperations, offload, and testHooks.")
			.Field("project", &SessionHelloResult::Project, "The open project.");

		registry.Struct<SessionClientSummary>("SessionClientSummary", "One connected automation client.")
			.Field("id", &SessionClientSummary::Id, "The client's id.")
			.Field("name", &SessionClientSummary::Name, "The client's name.")
			.Field("version", &SessionClientSummary::Version, "The client's version.")
			.Field("inProcess", &SessionClientSummary::InProcess, "A batch, command-line or test caller rather than a TCP connection.");

		registry.Struct<SessionInfoResult>("SessionInfoResult", "The editor session: versions, project, play state, renderer and clients.")
			.Field("protocolVersion", &SessionInfoResult::Protocol, "The protocol version the server speaks.")
			.Field("engineVersion", &SessionInfoResult::EngineVersion, "The engine's version.")
			.Field("pid", &SessionInfoResult::ProcessId, "The editor's process id.")
			.Field("capabilities", &SessionInfoResult::Capabilities, "The optional protocol features the server offers.")
			.Field("project", &SessionInfoResult::Project, "The open project.")
			.Field("playState", &SessionInfoResult::PlayState, "\"Edit\", \"Play\", \"Simulate\" or \"Paused\".")
			.Field("renderer", &SessionInfoResult::Renderer, "\"vulkan\" or \"none\" (--renderer).")
			.Field("lockstepOwner", &SessionInfoResult::LockstepOwner, "The name of the client that owns lockstep; empty when none.")
			.Field("readOnly", &SessionInfoResult::ReadOnly, "Whether the editor denies mutations (--read-only).")
			.Field("headless", &SessionInfoResult::Headless, "Whether the editor runs without a visible window (--headless).")
			.Field("clients", &SessionInfoResult::Clients, "The connected clients, by id.");

		registry.Struct<SessionShutdownParams>("SessionShutdownParams", "The params of session.shutdown.")
			.Field("save", &SessionShutdownParams::Save, "Save the open scene first when it has unsaved changes.")
			.Field("force", &SessionShutdownParams::Force, "Exit even when the open scene has unsaved changes, discarding them.");

		registry.Struct<SessionShutdownResult>("SessionShutdownResult", "What session.shutdown saved before the editor exits.")
			.Field("saved", &SessionShutdownResult::Saved, "Whether the open scene was written.")
			.Field("savedFiles", &SessionShutdownResult::SavedFiles, "The project-relative files written.");
	}

	void RegisterSessionMethods(MethodRegistry& methods)
	{
		Json helloExample = Json::object();
		helloExample["token"] = std::string(64, '0');
		helloExample["protocolVersion"] = CurrentProtocolVersion.ToString();
		helloExample["client"] = Json::object();
		helloExample["client"]["name"] = "engine-mcp";
		helloExample["client"]["version"] = "1.0";
		methods.Add(
			{
				.Name = "session.hello",
				.Description = "Opens the session: the first request of every connection, with the token from the editor's session file. Reports "
							   "the protocol and engine versions, the client id, the capabilities and the open project.",
				.RequiredParams = { "token", "protocolVersion", "client" },
				.AvailableInRuntime = true,
				.AvailableInLauncher = true,
				.Examples = { { .Description = "Open a session as the MCP bridge.", .Params = helloExample } },
			},
			&Automation::SessionHello);

		methods.Add(
			{
				.Name = "session.info",
				.Description = "Reports the editor session: versions, capabilities, the open project, play state, renderer, lockstep owner, the "
							   "read-only flag and the connected clients.",
				.AvailableInRuntime = true,
				.AvailableInLauncher = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the session.", .Params = Json::object() } },
			},
			&Automation::SessionInfo);

		Json saveExample = Json::object();
		saveExample["save"] = true;
		methods.Add(
			{
				.Name = "session.shutdown",
				.Description = "Exits the editor after the response. With unsaved changes in the open scene it needs save (write them first) or "
							   "force (discard them).",
				.AvailableInRuntime = true,
				.AvailableInLauncher = true,
				.Examples = { { .Description = "Save the open scene, then exit.", .Params = saveExample } },
			},
			&Automation::SessionShutdown);
	}

}
