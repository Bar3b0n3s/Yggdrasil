#include "EditorPCH.h"
#include "EditorCore/Automation/AutomationServer.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/Handshake.h"
#include "Engine/Automation/Protocol/ProtocolServer.h"
#include "Engine/Automation/Protocol/ResultOffload.h"
#include "Engine/Automation/Protocol/SessionFile.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/Process.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <format>
#include <map>

// Client ids. The Dispatcher keys its clients by id, and TCP connections (numbered by the ProtocolServer) and in-process
// callers (numbered here) must never collide, so the server gives every client its own id and maps TCP connections onto
// them.
//
// Offloaded results. A writable project keeps them in project://Library/Automation/Out/ (gitignored, §2.1); without a
// project, and for read-only editors, which write nothing under the project, they go to user://Automation/Out/. The
// response names the file's absolute native path (ADR 0008 decision 22): the project root is known, and user://Automation is
// the parent of the sessions directory (<UserData>/<AppName>/Automation, §13.2). A server without a sessions directory (an
// in-process server of the Tests) names the user:// path instead, the only one it knows.
//
// Offloaded results are transient: an agent reads one right after its response. So that the directories do not grow
// without bound, the first write to a directory removes the files of servers whose process no longer runs (crashed
// editors; the process id is the first part of the server tag), and the destructor removes this server's own files from
// every directory it wrote to.

namespace Engine {

	namespace Utils {

		// The process id at the start of an offload file name ("<processId>-<startSeconds>-<sequence>.json",
		// MakeOffloadServerTag and MakeOffloadFileName), or nullopt for a name of another form.
		[[nodiscard]] static std::optional<uint32_t> ParseOffloadProcessId(std::string_view fileName)
		{
			if (!fileName.ends_with(".json"))
				return std::nullopt;
			uint32_t processId = 0;
			const std::from_chars_result parsed = std::from_chars(fileName.data(), fileName.data() + fileName.size(), processId);
			if (parsed.ec != std::errc() || parsed.ptr == fileName.data() || *parsed.ptr != '-')
				return std::nullopt;
			return processId;
		}

		// Removes the files of `directory` whose names `selects` accepts, logging failures (a file another process removed
		// meanwhile is not one).
		template<typename Predicate>
		static void RemoveOffloadFiles(VirtualFileSystem& vfs, const VfsPath& directory, Predicate&& selects)
		{
			const Result<std::vector<VfsEntry>> entries = vfs.List(directory, false);
			if (!entries)
			{
				if (entries.error().GetCode() != ErrorCode::NotFound)
					ENGINE_WARN("Could not list the offloaded automation results in '{}': {}", directory.ToString(), entries.error().ToString());
				return;
			}
			for (const VfsEntry& entry : *entries)
			{
				if (entry.Info.IsDirectory || !selects(entry.Path.GetFileName()))
					continue;
				const Status removed = vfs.Remove(entry.Path);
				if (!removed && removed.error().GetCode() != ErrorCode::NotFound)
					ENGINE_WARN("Could not remove the offloaded automation result '{}': {}", entry.Path.ToString(), removed.error().ToString());
			}
		}

	}

	struct AutomationServer::State
	{
		struct ClientRecord
		{
			std::string Name{};
			std::string Version{};
			bool InProcess = false;
			ClientId Connection = NoClient; // the ProtocolServer's id of a TCP client
		};

		Scope<Dispatcher> Calls;
		Scope<ProtocolServer> Transport;
		std::string Token{};
		std::string ServerTag{}; // MakeOffloadServerTag of the process id and start time
		std::string StartedAt{}; // SessionFile::FormatUtcTimestamp of the start time
		std::map<ClientId, ClientRecord> Clients{};
		std::map<ClientId, ClientId> Connections{}; // ProtocolServer id -> client id
		std::map<ClientId, std::vector<Json>> InProcessResponses{};
		ClientId NextClientId = 1;
		bool SessionFileWritten = false;
		std::optional<std::string> SessionProject{};       // the projectPath the session file names
		std::optional<std::string> FailedSessionProject{}; // the projectPath of the last failed rewrite, reported once
		std::chrono::steady_clock::time_point NextSessionFileAttempt{};
		// The offload directories this server wrote to, each pruned of the files of servers that are gone when first used.
		std::vector<VfsPath> OffloadDirectories{};
		// The dry run of the request being served: dry runs are never pending, so at most one is open, from AdmitRequest to
		// FinishRequest of the request that opened it.
		Scope<EditorDryRunScope> DryRun;
		const MethodContext* DryRunRequest = nullptr;

		// Removes `client` like a disconnect: its pending operations are cancelled and one AutomationClientDisconnected
		// event is appended (§13.2 "Disconnect").
		void RemoveClient(ClientId client, EditorContext& editor);
		// Writes the session file when the project it names changed since the last successful write (or it was never
		// written). A failed write is tried again at most once per specification.SessionFileRetryInterval and reported once per project:
		// the error is returned for the first failure only.
		[[nodiscard]] Status WriteSessionFileIfChanged(const EditorContext& editor, const AutomationServerSpecification& specification,
			std::chrono::steady_clock::time_point now);
		// Removes this server's offloaded results from every directory it wrote to.
		void RemoveOwnOffloadFiles(VirtualFileSystem& vfs) const;
	};

	AutomationServer::AutomationServer(ConstructionKey /*key*/, EditorContext& editor, const AutomationServerSpecification& specification)
		: m_Editor(&editor), m_Specification(specification), m_Watchdog(specification.WatchdogStallThreshold, std::chrono::steady_clock::now()), m_Methods(editor.GetTypeRegistry()), m_State(CreateScope<State>())
	{
	}

	AutomationServer::~AutomationServer()
	{
		// No new requests, then every pending operation is cancelled while the editor still exists, then the session file goes.
		if (m_State->Transport != nullptr)
			m_State->Transport->Stop();
		m_State->Calls.reset();
		m_State->Transport.reset();
		m_State->RemoveOwnOffloadFiles(m_Editor->GetVfs());
		if (m_State->SessionFileWritten)
		{
			const Status removed = SessionFile::Remove(m_Specification.SessionsDirectory, Process::GetCurrentId());
			if (!removed)
				ENGINE_WARN("Could not remove the automation session file: {}", removed.error().ToString());
		}
	}

	Result<Scope<AutomationServer>> AutomationServer::Create(EditorContext& editor, const AutomationServerSpecification& specification)
	{
		Scope<AutomationServer> server = CreateScope<AutomationServer>(ConstructionKey(), editor, specification);
		RegisterEditorMethods(server->m_Methods, EditorMethodOptions{ .TestHooks = specification.TestHooks });
		server->m_Methods.Freeze();

		const std::chrono::system_clock::time_point started = std::chrono::system_clock::now();
		const int64_t startSeconds = std::chrono::floor<std::chrono::seconds>(started.time_since_epoch()).count();
		State& state = *server->m_State;
		state.ServerTag = MakeOffloadServerTag(Process::GetCurrentId(), startSeconds);
		state.StartedAt = SessionFile::FormatUtcTimestamp(started);
		IMethodHost& host = *server;
		state.Calls = CreateScope<Dispatcher>(server->m_Methods, host, Log::GetRingBuffer(), &server->m_Watchdog);

		if (specification.Listen)
		{
			if (specification.SessionsDirectory.empty())
				return MakeError(ErrorCode::InvalidArgument, "a listening automation server needs a sessions directory");
			ENGINE_TRY_ASSIGN(state.Token, GenerateAuthToken());
			ENGINE_TRY_ASSIGN(state.Transport, ProtocolServer::Start({ .Port = specification.Port, .Token = state.Token }, server->m_Watchdog));
			ENGINE_TRY(state.WriteSessionFileIfChanged(editor, specification, std::chrono::steady_clock::now()));
		}
		return server;
	}

	void AutomationServer::Pump()
	{
		m_Watchdog.Heartbeat(std::chrono::steady_clock::now());
		const WatchdogPhaseScope phase(&m_Watchdog, "Pump");
		State& state = *m_State;

		if (state.Transport != nullptr)
		{
			// Requests before events: a request's Connected was queued before it, so the events taken next hold it, and a
			// request whose client disconnected meanwhile is dropped (ProtocolServer.h).
			std::vector<InboundRequest> requests = state.Transport->TakeRequests();
			const std::vector<ClientEvent> events = state.Transport->TakeClientEvents();
			for (const ClientEvent& event : events)
			{
				if (event.Kind != ClientEventKind::Connected)
					continue;
				const ClientId client = state.NextClientId++;
				state.Clients[client] = State::ClientRecord{ .Name = event.Name, .Version = event.Version, .InProcess = false, .Connection = event.Client };
				state.Connections[event.Client] = client;
				state.Calls->AddClient(client, event.Name, true);
				ENGINE_INFO("Automation client '{}' {} connected ({})", event.Name, event.Version, client);
			}
			for (InboundRequest& request : requests)
			{
				const auto client = state.Connections.find(request.Client);
				if (client != state.Connections.end())
					state.Calls->Enqueue(client->second, std::move(request.Request));
			}
			for (const ClientEvent& event : events)
			{
				if (event.Kind != ClientEventKind::Disconnected)
					continue;
				const auto client = state.Connections.find(event.Client);
				if (client != state.Connections.end())
					state.RemoveClient(client->second, *m_Editor);
			}
		}

		for (OutboundMessage& message : state.Calls->Pump(m_Specification.PumpBudget))
		{
			const auto client = state.Clients.find(message.Client);
			if (client == state.Clients.end())
				continue;
			if (client->second.InProcess)
			{
				state.InProcessResponses[message.Client].push_back(std::move(message.Message));
				continue;
			}
			const Status sent = state.Transport->Send(client->second.Connection, message.Message);
			if (!sent && sent.error().GetCode() != ErrorCode::NotFound)
				ENGINE_WARN("Automation client '{}' ({}): {}", client->second.Name, message.Client, sent.error().GetMessageText());
		}

		if (state.Transport != nullptr)
		{
			const Status written = state.WriteSessionFileIfChanged(*m_Editor, m_Specification, std::chrono::steady_clock::now());
			if (!written)
				ENGINE_WARN("Could not rewrite the automation session file (tried again every {} ms): {}", m_Specification.SessionFileRetryInterval.count(),
					written.error().ToString());
		}
	}

	uint16_t AutomationServer::GetPort() const
	{
		return m_State->Transport != nullptr ? m_State->Transport->GetPort() : 0;
	}

	std::vector<AutomationClientInfo> AutomationServer::GetClients() const
	{
		std::vector<AutomationClientInfo> clients;
		clients.reserve(m_State->Clients.size());
		for (const auto& [id, record] : m_State->Clients)
			clients.push_back(AutomationClientInfo{ .Id = id, .Name = record.Name, .Version = record.Version, .InProcess = record.InProcess });
		return clients;
	}

	ClientId AutomationServer::ConnectInProcess(std::string name, bool offloadLargeResults)
	{
		State& state = *m_State;
		const ClientId client = state.NextClientId++;
		state.Clients[client] = State::ClientRecord{ .Name = name, .Version = {}, .InProcess = true, .Connection = NoClient };
		state.InProcessResponses[client];
		state.Calls->AddClient(client, std::move(name), offloadLargeResults);
		return client;
	}

	void AutomationServer::DisconnectInProcess(ClientId client)
	{
		const auto record = m_State->Clients.find(client);
		if (record == m_State->Clients.end() || !record->second.InProcess)
			return;
		m_State->RemoveClient(client, *m_Editor);
	}

	void AutomationServer::SubmitInProcess(ClientId client, RpcRequest request)
	{
		const auto record = m_State->Clients.find(client);
		ENGINE_ASSERT(record != m_State->Clients.end() && record->second.InProcess, "SubmitInProcess: {} is not an in-process client", client);
		if (record != m_State->Clients.end() && record->second.InProcess)
			m_State->Calls->Enqueue(client, std::move(request));
	}

	std::vector<Json> AutomationServer::TakeInProcessResponses(ClientId client)
	{
		std::vector<Json> responses;
		const auto queue = m_State->InProcessResponses.find(client);
		if (queue != m_State->InProcessResponses.end())
			responses.swap(queue->second);
		return responses;
	}

	Status AutomationServer::CheckAvailability(const MethodDescriptor& method) const
	{
		if (!m_Editor->HasProject() && !method.Specification.AvailableInLauncher)
			return MakeError(ErrorCode::InvalidState, "no project open; call project.create or project.open");
		return {};
	}

	Scope<MethodContext> AutomationServer::CreateContext(MethodRequest request)
	{
		return CreateScope<EditorMethodContext>(*m_Editor, *this, std::move(request));
	}

	Status AutomationServer::AdmitRequest(MethodContext& context)
	{
		const MethodSpecification& specification = context.GetMethod().Specification;
		if (specification.Mutates && !context.IsDryRun() && m_Editor->IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "the editor is read-only (--read-only): '{}' changes the project", specification.Name);

		if (const std::optional<uint64_t> expected = context.GetOptions().IfRevision)
		{
			const uint64_t current = m_Editor->GetRevision();
			if (*expected != current)
			{
				context.SetErrorData("currentRevision", current);
				return std::unexpected(Error(ErrorCode::Conflict, std::format("the editor's revision is {}, not {}", current, *expected))
						.WithHint("read the current state again (its _meta.revision) and retry with that ifRevision"));
			}
		}

		if (context.IsDryRun())
		{
			ENGINE_ASSERT(m_State->DryRun == nullptr, "A dry run is already open for another request");
			ENGINE_TRY_ASSIGN(m_State->DryRun, EditorDryRunScope::Begin(*m_Editor));
			m_State->DryRunRequest = &context;
		}
		return {};
	}

	void AutomationServer::EnterInvocation(MethodContext& context)
	{
		const RequestInfo& request = context.GetRequest();
		m_Editor->SetWriteAttribution(WriteAttribution{
			.Method = request.Method,
			.RequestId = VariantValue(request.Id),
			.Client = request.ClientName,
			.TranscriptLine = request.TranscriptLine,
		});
	}

	void AutomationServer::LeaveInvocation(MethodContext& /*context*/)
	{
		m_Editor->SetWriteAttribution(std::nullopt);
	}

	void AutomationServer::FinishRequest(MethodContext& context)
	{
		if (m_State->DryRunRequest == &context)
		{
			m_State->DryRun.reset();
			m_State->DryRunRequest = nullptr;
		}
	}

	MetaState AutomationServer::GetMetaState() const
	{
		const EditorContext& editor = *m_Editor;
		MetaState state;
		state.Revision = editor.GetRevision();
		state.Dirty = editor.HasScene() && editor.IsSceneDirty();
		state.UndoLabel = editor.HasScene() ? editor.GetHistory().GetUndoLabel() : std::string();
		state.PlayState = "Edit";
		return state;
	}

	std::string AutomationServer::GetOffloadServerTag() const
	{
		return m_State->ServerTag;
	}

	Result<std::string> AutomationServer::WriteOffloadedResult(std::string_view fileName, std::string_view text)
	{
		VirtualFileSystem& vfs = m_Editor->GetVfs();
		const bool inProject = m_Editor->HasProject() && !m_Editor->IsReadOnly();
		ENGINE_TRY_ASSIGN(const VfsPath directory, inProject ? VfsPath::Create("project", OffloadDirectory) : VfsPath::Create("user", "Automation/Out"));
		ENGINE_TRY_ASSIGN(const VfsPath file, directory.Join(fileName));
		std::vector<VfsPath>& used = m_State->OffloadDirectories;
		if (std::find(used.begin(), used.end(), directory) == used.end())
		{
			// First use: the leftovers of servers that are gone (see the file comment).
			Utils::RemoveOffloadFiles(vfs, directory, [](std::string_view name)
			{
				const std::optional<uint32_t> processId = Utils::ParseOffloadProcessId(name);
				return processId.has_value() && !Process::IsRunning(*processId);
			});
			used.push_back(directory);
		}
		ENGINE_TRY(vfs.CreateDirectories(directory));
		ENGINE_TRY(vfs.WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size()))));

		if (inProject)
			return FileSystem::PathToUtf8(m_Editor->GetProject().GetRoot() / OffloadDirectory / fileName);
		if (!m_Specification.SessionsDirectory.empty())
			return FileSystem::PathToUtf8(m_Specification.SessionsDirectory.parent_path() / "Out" / fileName);
		return file.ToString();
	}

	void AutomationServer::State::RemoveClient(ClientId client, EditorContext& editor)
	{
		const auto record = Clients.find(client);
		if (record == Clients.end())
			return;
		Calls->RemoveClient(client);
		const std::string name = record->second.Name;
		if (!record->second.InProcess)
			Connections.erase(record->second.Connection);
		Clients.erase(record);
		InProcessResponses.erase(client);
		EngineEvent event;
		event.Type = EngineEventType::AutomationClientDisconnected;
		event.Name = name;
		editor.AppendEvent(std::move(event));
		ENGINE_INFO("Automation client '{}' disconnected ({})", name, client);
	}

	void AutomationServer::State::RemoveOwnOffloadFiles(VirtualFileSystem& vfs) const
	{
		const std::string prefix = ServerTag + "-";
		for (const VfsPath& directory : OffloadDirectories)
		{
			Utils::RemoveOffloadFiles(vfs, directory, [&prefix](std::string_view name)
			{
				return name.starts_with(prefix);
			});
		}
	}

	Status AutomationServer::State::WriteSessionFileIfChanged(const EditorContext& editor, const AutomationServerSpecification& specification,
		std::chrono::steady_clock::time_point now)
	{
		std::string project = editor.HasProject() ? FileSystem::PathToUtf8(editor.GetProject().GetProjectFile()) : std::string();
		if (SessionProject.has_value() && *SessionProject == project)
			return {};
		if (FailedSessionProject.has_value() && now < NextSessionFileAttempt)
			return {};
		const SessionFileContent content{
			.Pid = Process::GetCurrentId(),
			.Port = Transport != nullptr ? Transport->GetPort() : uint16_t{ 0 },
			.Token = Token,
			.ProtocolVersionText = CurrentProtocolVersion.ToString(),
			.EngineVersionText = std::string(EngineVersionString),
			.ProjectPath = project,
			.Headless = specification.Headless,
			.StartedAt = StartedAt,
		};
		const Status written = SessionFile::Write(specification.SessionsDirectory, content);
		if (!written)
		{
			// Tried again later; reported once per project rather than on every attempt.
			NextSessionFileAttempt = now + specification.SessionFileRetryInterval;
			const bool reported = FailedSessionProject == project;
			FailedSessionProject = std::move(project);
			return reported ? Status() : written;
		}
		SessionProject = std::move(project);
		FailedSessionProject.reset();
		SessionFileWritten = true;
		return {};
	}

}
