#include "EnginePCH.h"
#include "Engine/Automation/Methods/SessionMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/PlayMethods.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Platform/Process.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		// session.hello's client.name: 1 to 64 printable ASCII characters (the rule Handshake.h's ParseHelloRequest applies
		// on the I/O thread; in-process callers reach the handler without it).
		constexpr size_t MaxClientNameLength = 64;

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

		Result<SessionHelloResult> SessionHello(AutomationMethodContext& context, const SessionHelloParams& params)
		{
			// The I/O thread already checked the token and the version of TCP clients (Handshake.h); in-process callers have no
			// token, and get the same param checks here.
			ENGINE_TRY(Utils::ValidateHello(params));

			SessionHostDescription host = context.DescribeSession();
			SessionHelloResult result;
			result.Protocol = CurrentProtocolVersion.ToString();
			result.EngineVersion = std::string(EngineVersionString);
			result.Client = context.GetRequest().Client;
			result.Capabilities = std::move(host.Capabilities);
			result.Project = std::move(host.Project);
			return result;
		}

		Result<SessionInfoResult> SessionInfo(AutomationMethodContext& context, const NoParams& /*params*/)
		{
			SessionHostDescription host = context.DescribeSession();
			SessionInfoResult result;
			result.Protocol = CurrentProtocolVersion.ToString();
			result.EngineVersion = std::string(EngineVersionString);
			result.ProcessId = Process::GetCurrentId();
			result.Capabilities = std::move(host.Capabilities);
			result.Project = std::move(host.Project);
			const PlaySession* session = context.GetPlaySession();
			result.PlayState = std::string(PlayRunStateToString(GetPlayRunState(session)));
			result.Renderer = std::move(host.Renderer);
			if (session != nullptr && session->IsLockstep())
				result.LockstepOwner = context.GetClientName(session->GetLockstepOwner());
			result.ReadOnly = host.ReadOnly;
			result.Headless = host.Headless;
			result.Clients = std::move(host.Clients);
			std::sort(result.Clients.begin(), result.Clients.end(), [](const SessionClientSummary& left, const SessionClientSummary& right)
			{
				return left.Id < right.Id;
			});
			return result;
		}

		Result<SessionShutdownResult> SessionShutdown(AutomationMethodContext& context, const SessionShutdownParams& params)
		{
			return context.Shutdown(params);
		}

	}

	void RegisterSessionMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<SessionClientInfo>("SessionClientInfo", "Who connects: session.hello's client.")
			.Field("name", &SessionClientInfo::Name, "The client's name, 1 to 64 printable ASCII characters, such as \"engine-mcp\".")
			.Field("version", &SessionClientInfo::Version, "The client's version; free text, may be empty.");

		registry.Struct<SessionHelloParams>("SessionHelloParams", "The params of session.hello, the first request of every connection.")
			.Field("token", &SessionHelloParams::Token, "The session token from the server's session file; never logged or echoed.")
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

		registry.Struct<SessionInfoResult>("SessionInfoResult", "The host's session (the editor's or the exported game's): versions, project, play state, renderer and clients.")
			.Field("protocolVersion", &SessionInfoResult::Protocol, "The protocol version the server speaks.")
			.Field("engineVersion", &SessionInfoResult::EngineVersion, "The engine's version.")
			.Field("pid", &SessionInfoResult::ProcessId, "The host's process id.")
			.Field("capabilities", &SessionInfoResult::Capabilities, "The optional protocol features the server offers.")
			.Field("project", &SessionInfoResult::Project, "The open project.")
			.Field("playState", &SessionInfoResult::PlayState, "\"Edit\", \"Play\", \"Simulate\" or \"Paused\".")
			.Field("renderer", &SessionInfoResult::Renderer, "\"vulkan\" or \"none\" (--renderer).")
			.Field("lockstepOwner", &SessionInfoResult::LockstepOwner, "The name of the client that owns lockstep; empty when none.")
			.Field("readOnly", &SessionInfoResult::ReadOnly, "Whether the host denies mutations: the editor with --read-only, the Runtime always.")
			.Field("headless", &SessionInfoResult::Headless, "Whether the host runs without a visible window (--headless).")
			.Field("clients", &SessionInfoResult::Clients, "The connected clients, by id.");

		registry.Struct<SessionShutdownParams>("SessionShutdownParams", "The params of session.shutdown.")
			.Field("save", &SessionShutdownParams::Save, "The editor saves the open scene first when it has unsaved changes (the Runtime has none to save).")
			.Field("force", &SessionShutdownParams::Force, "The editor exits even when the open scene has unsaved changes, discarding them.");

		registry.Struct<SessionShutdownResult>("SessionShutdownResult", "What session.shutdown saved before the host exits (always nothing in the Runtime).")
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
				.Description = "Opens the session: the first request of every connection, with the token from the server's session file. Reports "
							   "the protocol and engine versions, the client id, the capabilities and the open project (the game in the Runtime).",
				.RequiredParams = { "token", "protocolVersion", "client" },
				.AvailableInRuntime = true,
				.AvailableInLauncher = true,
				.Examples = { { .Description = "Open a session as the MCP bridge.", .Params = helloExample } },
			},
			&Automation::SessionHello);

		methods.Add(
			{
				.Name = "session.info",
				.Description = "Reports the host's session (the editor's or the exported game's): versions, capabilities, the open project, play "
							   "state, renderer, lockstep owner, the read-only flag and the connected clients.",
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
				.Description = "Exits the host after the response. The editor with unsaved changes in the open scene needs save (write them "
							   "first) or force (discard them); the Runtime saves nothing and needs neither.",
				.AvailableInRuntime = true,
				.AvailableInLauncher = true,
				.Examples = { { .Description = "Save the open scene, then exit.", .Params = saveExample } },
			},
			&Automation::SessionShutdown);
	}

}
