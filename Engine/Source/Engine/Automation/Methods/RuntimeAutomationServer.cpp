#include "EnginePCH.h"
#include "Engine/Automation/Methods/RuntimeAutomationServer.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/PlayMethods.h"
#include "Engine/Automation/Methods/RegisterSharedMethods.h"
#include "Engine/Automation/Methods/SceneMethods.h"
#include "Engine/Automation/Methods/SessionMethods.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/Handshake.h"
#include "Engine/Automation/Protocol/ProtocolServer.h"
#include "Engine/Automation/Protocol/ResultOffload.h"
#include "Engine/Automation/Protocol/SessionFile.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/Process.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <utility>

// Client ids, as in the editor's AutomationServer: the server gives every client its own id and maps TCP connections
// (numbered by the ProtocolServer) onto them, so in-process clients and connections never collide.
//
// Output and offloaded results go to user://Automation/Out/ (the game's user-data folder, <UserData>/<Name>/, §14.1), named
// "<serverTag>-<sequence>.<extension>" like the editor's; responses name their absolute native path, the parent of the
// sessions directory's "Out" (a server without a sessions directory, in the Tests, names the user:// path). The first write
// removes the files of servers whose process is gone; the destructor removes this server's offloaded results and keeps its
// output files (screenshots), as the editor does.

namespace Engine {

	namespace {

		// One connected client.
		struct RuntimeClientRecord
		{
			std::string Name{};
			std::string Version{};
			bool InProcess = false;
			ClientId Connection = NoClient; // the ProtocolServer's id of a TCP client
		};

		// What the Runtime's request contexts read and write: the server's back-references and specification, its clients,
		// its output files and the shutdown request.
		struct RuntimeHost
		{
			EventLog* Events = nullptr;       // documented back-reference
			VirtualFileSystem* Vfs = nullptr; // documented back-reference
			PlaySession* Session = nullptr;   // documented back-reference
			RuntimeAutomationServerSpecification Specification{};
			std::map<ClientId, RuntimeClientRecord> Clients{};
			std::string ServerTag{};
			uint64_t NextOutputSequence = 1;
			bool OutputDirectoryPrepared = false;
			std::optional<int> ShutdownRequest{};

			// user://Automation/Out/, created and, on first use, pruned of the files of servers that are gone.
			[[nodiscard]] Result<VfsPath> PrepareOutputDirectory();
			// The absolute native path of `file` (in PrepareOutputDirectory's directory), which responses name.
			[[nodiscard]] std::string GetOutputPath(const VfsPath& file) const;
		};

		// The context of one request on the Runtime (AutomationMethodContext's services over the play session and the server).
		class RuntimeMethodContext final : public AutomationMethodContext
		{
		public:
			// `host` is a documented back-reference that outlives the request.
			RuntimeMethodContext(RuntimeHost& host, MethodRequest request)
				: AutomationMethodContext(TypeKeyOf<RuntimeMethodContext>(), std::move(request)), m_Host(&host)
			{
			}

			[[nodiscard]] Scope<MethodContext> CreateNested(MethodRequest request) const override
			{
				return CreateScope<RuntimeMethodContext>(*m_Host, std::move(request));
			}

			[[nodiscard]] Result<Scene*> ResolveTargetScene(SceneTarget target, bool given, bool /*mutation*/) const override
			{
				// The Runtime has only the play scene (§13.4): an explicit "edit" names nothing.
				if (given && target == SceneTarget::Edit)
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidState, "/target", "the Runtime has no edit scene",
						"omit target, or pass \"play\""));
				}
				return &m_Host->Session->GetScene();
			}

			[[nodiscard]] Result<Entity> ResolveEntity(Scene& scene, std::string_view reference, std::string_view pointer) const override
			{
				return Utils::ResolveEntityReference(scene, reference, pointer);
			}

			[[nodiscard]] EntitySummary MakeEntitySummary(ConstEntity entity) const override
			{
				return Utils::SummarizeEntity(entity);
			}

			[[nodiscard]] PlaySession* GetPlaySession() const override
			{
				return m_Host->Session;
			}

			[[nodiscard]] Status StartPlay(const PlayStartOptions& /*options*/) override
			{
				return MakeError(ErrorCode::Unsupported, "the Runtime's play session starts with the process");
			}

			[[nodiscard]] Status StopPlay() override
			{
				return MakeError(ErrorCode::Unsupported, "the Runtime's play session ends with the process: call session.shutdown");
			}

			[[nodiscard]] std::string GetClientName(ClientId client) const override
			{
				const auto record = m_Host->Clients.find(client);
				return record != m_Host->Clients.end() ? record->second.Name : std::string();
			}

			[[nodiscard]] EventLog& GetEventLog() const override
			{
				return *m_Host->Events;
			}

			[[nodiscard]] Result<Image> CaptureView(const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request) override
			{
				if (!m_Host->Specification.View)
					return MakeError(ErrorCode::Unsupported, "the game renders no views with --renderer none: start it without --renderer none");
				return m_Host->Specification.View(snapshot, request);
			}

			[[nodiscard]] std::optional<ExplicitRenderCamera> GetSceneViewCamera() const override
			{
				return std::nullopt;
			}

			[[nodiscard]] Result<std::string> WriteOutputFile(std::string_view extension, std::span<const std::byte> bytes) override
			{
				ENGINE_CORE_ASSERT(!extension.empty() && std::ranges::all_of(extension, [](char character)
				{
					return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
				}),
					"output file extension '{}' must be lowercase letters and digits", extension);
				ENGINE_TRY_ASSIGN(const VfsPath directory, m_Host->PrepareOutputDirectory());
				const std::string fileName = std::format("{}-{:08}.{}", m_Host->ServerTag, m_Host->NextOutputSequence, extension);
				ENGINE_TRY_ASSIGN(const VfsPath file, directory.Join(fileName));
				ENGINE_TRY(m_Host->Vfs->WriteFileAtomic(file, bytes));
				++m_Host->NextOutputSequence;
				return m_Host->GetOutputPath(file);
			}

			[[nodiscard]] SessionHostDescription DescribeSession() const override
			{
				const RuntimeAutomationServerSpecification& specification = m_Host->Specification;
				SessionHostDescription host;
				host.Capabilities = { "ifRevision", "pendingOperations", "offload" };
				// The game stands where the editor reports its project: its name, no project file, and nothing to write.
				host.Project = SessionProjectSummary{ .Open = true, .Name = specification.GameName, .ProjectFile = {}, .ReadOnly = true };
				host.Renderer = specification.RendererName;
				host.ReadOnly = true;
				host.Headless = specification.Headless;
				for (const auto& [id, record] : m_Host->Clients)
				{
					host.Clients.push_back(
						SessionClientSummary{ .Id = id, .Name = record.Name, .Version = record.Version, .InProcess = record.InProcess });
				}
				return host;
			}

			[[nodiscard]] Result<SessionShutdownResult> Shutdown(const SessionShutdownParams& /*params*/) override
			{
				// No scene file to save: the game exits after the response (RuntimeAutomationServer::GetShutdownRequest).
				m_Host->ShutdownRequest = 0;
				return SessionShutdownResult{};
			}

			[[nodiscard]] SceneSummary MakeSceneSummary(const Scene& scene) const override
			{
				SceneSummary summary;
				summary.Path = m_Host->Specification.ScenePath;
				summary.Name = scene.GetName();
				summary.Revision = ToAutomationCounter(scene.GetRevision());
				summary.EntityCount = ToAutomationCounter(scene.GetEntityCount());
				return summary;
			}

			[[nodiscard]] AssetManager* GetAssets() const override
			{
				return m_Host->Specification.Assets;
			}

			[[nodiscard]] std::chrono::steady_clock::time_point GetWallClockTime() const override
			{
				const RuntimeAutomationServerSpecification& specification = m_Host->Specification;
				return specification.WallClock ? specification.WallClock() : std::chrono::steady_clock::now();
			}

			[[nodiscard]] AudioEngine* GetAudioEngine() const override
			{
				return m_Host->Specification.Audio;
			}
		private:
			RuntimeHost* m_Host = nullptr; // documented back-reference
		};

		Result<VfsPath> RuntimeHost::PrepareOutputDirectory()
		{
			ENGINE_TRY_ASSIGN(const VfsPath directory, VfsPath::Create("user", "Automation/Out"));
			if (!OutputDirectoryPrepared)
			{
				// First use: the leftovers of servers that are gone.
				const Result<std::vector<VfsEntry>> entries = Vfs->List(directory, false);
				if (entries.has_value())
				{
					for (const VfsEntry& entry : *entries)
					{
						const std::string_view name = entry.Path.GetFileName();
						uint32_t processId = 0;
						const std::from_chars_result parsed = std::from_chars(name.data(), name.data() + name.size(), processId);
						if (entry.Info.IsDirectory || parsed.ec != std::errc() || parsed.ptr == name.data() || *parsed.ptr != '-' || Process::IsRunning(processId))
							continue;
						const Status removed = Vfs->Remove(entry.Path);
						if (!removed && removed.error().GetCode() != ErrorCode::NotFound)
							ENGINE_CORE_WARN("Could not remove the automation output '{}': {}", entry.Path.ToString(), removed.error().ToString());
					}
				}
				OutputDirectoryPrepared = true;
			}
			ENGINE_TRY(Vfs->CreateDirectories(directory));
			return directory;
		}

		std::string RuntimeHost::GetOutputPath(const VfsPath& file) const
		{
			if (!Specification.SessionsDirectory.empty())
				return FileSystem::PathToUtf8(Specification.SessionsDirectory.parent_path() / "Out" / FileSystem::PathFromUtf8(file.GetFileName()));
			return file.ToString();
		}

	}

	struct RuntimeAutomationServer::State final : IMethodHost
	{
		RuntimeHost Host{};
		Scope<Watchdog> Monitor;
		Scope<MethodRegistry> Methods; // over the context's registry; filled and frozen by Create
		Scope<Dispatcher> Calls;
		Scope<ProtocolServer> Transport;
		std::string Token{};
		std::string StartedAt{};
		std::map<ClientId, ClientId> Connections{}; // ProtocolServer id -> client id
		std::map<ClientId, std::vector<Json>> InProcessResponses{};
		ClientId NextClientId = 1;
		bool SessionFileWritten = false;

		// A client is gone (§13.2 "Disconnect"): its pending operations are cancelled (a running play.step clears its
		// stepping flag), then its lockstep is released and play paused, and AutomationClientDisconnected is appended.
		void RemoveClient(ClientId client);

		[[nodiscard]] Status CheckAvailability(const MethodDescriptor& /*method*/) const override
		{
			// The registry holds the Runtime subset only (RegisterSharedMethods with AutomationHost::Runtime).
			return {};
		}

		[[nodiscard]] Scope<MethodContext> CreateContext(MethodRequest request) override
		{
			return CreateScope<RuntimeMethodContext>(Host, std::move(request));
		}

		[[nodiscard]] Status AdmitRequest(MethodContext& context) override
		{
			// None of the Runtime's methods supports a dry run (they read, or they advance time).
			if (context.IsDryRun())
				return MakeError(ErrorCode::Unsupported, "the Runtime has no dry runs ('{}')", context.GetMethod().Specification.Name);
			if (const std::optional<uint64_t> expected = context.GetOptions().IfRevision)
			{
				const uint64_t current = Host.Session->GetScene().GetRevision();
				if (*expected != current)
				{
					context.SetErrorData("currentRevision", current);
					return std::unexpected(Error(ErrorCode::Conflict, std::format("the play scene's revision is {}, not {}", current, *expected))
							.WithHint("read the current state again (its _meta.revision) and retry with that ifRevision"));
				}
			}
			return {};
		}

		void EnterInvocation(MethodContext& /*context*/) override
		{
			// The Runtime writes no project files, so there is no write attribution to set.
		}

		void LeaveInvocation(MethodContext& /*context*/) override
		{
		}

		void FinishRequest(MethodContext& /*context*/) override
		{
		}

		[[nodiscard]] MetaState GetMetaState() const override
		{
			const PlaySession& session = *Host.Session;
			MetaState state;
			state.Revision = session.GetScene().GetRevision();
			state.Tick = session.GetTick();
			state.PlayState = std::string(PlayRunStateToString(GetPlayRunState(&session)));
			return state;
		}

		[[nodiscard]] std::string GetOffloadServerTag() const override
		{
			return Host.ServerTag;
		}

		[[nodiscard]] Result<std::string> WriteOffloadedResult(std::string_view fileName, std::string_view text) override
		{
			ENGINE_TRY_ASSIGN(const VfsPath directory, Host.PrepareOutputDirectory());
			ENGINE_TRY_ASSIGN(const VfsPath file, directory.Join(fileName));
			ENGINE_TRY(Host.Vfs->WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size()))));
			return Host.GetOutputPath(file);
		}
	};

	void RuntimeAutomationServer::State::RemoveClient(ClientId client)
	{
		const auto record = Host.Clients.find(client);
		if (record == Host.Clients.end())
			return;
		Calls->RemoveClient(client);
		PlaySession& session = *Host.Session;
		if (const Utils::DisconnectedLockstep released = Utils::ReleaseDisconnectedLockstep(session, client); released.Released)
		{
			if (released.StateChanged)
				Host.Events->Append(Utils::MakePlayStateChangedEvent(&session));
			ENGINE_CORE_INFO("Automation client '{}' disconnected while it owned lockstep: lockstep released, play paused at tick {}",
				record->second.Name, session.GetTick());
		}
		const std::string name = record->second.Name;
		if (!record->second.InProcess)
			Connections.erase(record->second.Connection);
		Host.Clients.erase(record);
		InProcessResponses.erase(client);
		Host.Events->Append(EngineEvent{ .Seq = 0, .Tick = std::nullopt, .Type = EngineEventType::AutomationClientDisconnected, .Id = UUID(), .Path = {}, .Name = name, .Message = {}, .Dirty = false });
		ENGINE_CORE_INFO("Automation client '{}' disconnected ({})", name, client);
	}

	RuntimeAutomationServer::RuntimeAutomationServer(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	RuntimeAutomationServer::~RuntimeAutomationServer()
	{
		// No new requests, then every pending operation is cancelled while the session still exists, then the files go.
		State& state = *m_State;
		if (state.Transport != nullptr)
			state.Transport->Stop();
		state.Calls.reset();
		state.Transport.reset();
		if (state.Host.OutputDirectoryPrepared)
		{
			const Result<VfsPath> directory = VfsPath::Create("user", "Automation/Out");
			const Result<std::vector<VfsEntry>> entries = directory.has_value() ? state.Host.Vfs->List(*directory, false) : Result<std::vector<VfsEntry>>();
			const std::string prefix = state.Host.ServerTag + "-";
			for (const VfsEntry& entry : entries.value_or(std::vector<VfsEntry>()))
			{
				const std::string_view name = entry.Path.GetFileName();
				if (entry.Info.IsDirectory || !name.starts_with(prefix) || !name.ends_with(".json"))
					continue;
				const Status removed = state.Host.Vfs->Remove(entry.Path);
				if (!removed && removed.error().GetCode() != ErrorCode::NotFound)
					ENGINE_CORE_WARN("Could not remove the offloaded automation result '{}': {}", entry.Path.ToString(), removed.error().ToString());
			}
		}
		if (state.SessionFileWritten)
		{
			const Status removed = SessionFile::Remove(state.Host.Specification.SessionsDirectory, Process::GetCurrentId());
			if (!removed)
				ENGINE_CORE_WARN("Could not remove the automation session file: {}", removed.error().ToString());
		}
	}

	Result<Scope<RuntimeAutomationServer>> RuntimeAutomationServer::Create(const TypeRegistry& registry, EventLog& events, VirtualFileSystem& vfs,
		PlaySession& session, const RuntimeAutomationServerSpecification& specification)
	{
		if (specification.Listen && specification.SessionsDirectory.empty())
			return MakeError(ErrorCode::InvalidArgument, "a listening automation server needs a sessions directory");

		Scope<RuntimeAutomationServer> server = CreateScope<RuntimeAutomationServer>(ConstructionKey());
		State& state = *server->m_State;
		state.Host.Events = &events;
		state.Host.Vfs = &vfs;
		state.Host.Session = &session;
		state.Host.Specification = specification;
		state.Monitor = CreateScope<Watchdog>(specification.WatchdogStallThreshold, std::chrono::steady_clock::now());
		state.Methods = CreateScope<MethodRegistry>(registry);
		RegisterSharedMethods(*state.Methods, AutomationHost::Runtime);
		state.Methods->Freeze();

		const std::chrono::system_clock::time_point started = std::chrono::system_clock::now();
		const int64_t startSeconds = std::chrono::floor<std::chrono::seconds>(started.time_since_epoch()).count();
		state.Host.ServerTag = MakeOffloadServerTag(Process::GetCurrentId(), startSeconds);
		state.StartedAt = SessionFile::FormatUtcTimestamp(started);
		IMethodHost& host = state;
		state.Calls = CreateScope<Dispatcher>(*state.Methods, host, Log::GetRingBuffer(), state.Monitor.get(),
			DispatcherSpecification{ .SystemErrors = specification.SystemErrors });

		if (specification.Listen)
		{
			ENGINE_TRY_ASSIGN(state.Token, GenerateAuthToken());
			ENGINE_TRY_ASSIGN(state.Transport, ProtocolServer::Start({ .Port = specification.Port, .Token = state.Token }, *state.Monitor));
			const SessionFileContent content{
				.Pid = Process::GetCurrentId(),
				.Port = state.Transport->GetPort(),
				.Token = state.Token,
				.ProtocolVersionText = CurrentProtocolVersion.ToString(),
				.EngineVersionText = std::string(EngineVersionString),
				.ProjectPath = specification.GameName,
				.Headless = specification.Headless,
				.StartedAt = state.StartedAt,
			};
			ENGINE_TRY(SessionFile::Write(specification.SessionsDirectory, content));
			state.SessionFileWritten = true;
		}
		return server;
	}

	void RuntimeAutomationServer::Pump()
	{
		State& state = *m_State;
		state.Monitor->Heartbeat(std::chrono::steady_clock::now());
		const WatchdogPhaseScope phase(state.Monitor.get(), "Pump");

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
				state.Host.Clients[client] = RuntimeClientRecord{ .Name = event.Name, .Version = event.Version, .InProcess = false, .Connection = event.Client };
				state.Connections[event.Client] = client;
				state.Calls->AddClient(client, event.Name, true);
				ENGINE_CORE_INFO("Automation client '{}' {} connected ({})", event.Name, event.Version, client);
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
					state.RemoveClient(client->second);
			}
		}

		for (OutboundMessage& message : state.Calls->Pump(state.Host.Specification.PumpBudget))
		{
			const auto client = state.Host.Clients.find(message.Client);
			if (client == state.Host.Clients.end())
				continue;
			if (client->second.InProcess)
			{
				state.InProcessResponses[message.Client].push_back(std::move(message.Message));
				continue;
			}
			const Status sent = state.Transport->Send(client->second.Connection, message.Message);
			if (!sent && sent.error().GetCode() != ErrorCode::NotFound)
				ENGINE_CORE_WARN("Automation client '{}' ({}): {}", client->second.Name, message.Client, sent.error().GetMessageText());
		}
	}

	uint16_t RuntimeAutomationServer::GetPort() const
	{
		return m_State->Transport != nullptr ? m_State->Transport->GetPort() : 0;
	}

	const MethodRegistry& RuntimeAutomationServer::GetMethods() const
	{
		ENGINE_CORE_ASSERT(m_State->Methods != nullptr, "RuntimeAutomationServer::GetMethods needs a server made by Create");
		return *m_State->Methods;
	}

	std::optional<int> RuntimeAutomationServer::GetShutdownRequest() const
	{
		return m_State->Host.ShutdownRequest;
	}

	ClientId RuntimeAutomationServer::ConnectInProcess(std::string name)
	{
		State& state = *m_State;
		const ClientId client = state.NextClientId++;
		state.Host.Clients[client] = RuntimeClientRecord{ .Name = name, .Version = {}, .InProcess = true, .Connection = NoClient };
		state.InProcessResponses[client];
		state.Calls->AddClient(client, std::move(name), true);
		return client;
	}

	void RuntimeAutomationServer::DisconnectInProcess(ClientId client)
	{
		const auto record = m_State->Host.Clients.find(client);
		if (record == m_State->Host.Clients.end() || !record->second.InProcess)
			return;
		m_State->RemoveClient(client);
	}

	void RuntimeAutomationServer::SubmitInProcess(ClientId client, RpcRequest request)
	{
		const auto record = m_State->Host.Clients.find(client);
		ENGINE_CORE_ASSERT(record != m_State->Host.Clients.end() && record->second.InProcess, "SubmitInProcess: {} is not an in-process client", client);
		if (record != m_State->Host.Clients.end() && record->second.InProcess)
			m_State->Calls->Enqueue(client, std::move(request));
	}

	std::vector<Json> RuntimeAutomationServer::TakeInProcessResponses(ClientId client)
	{
		std::vector<Json> responses;
		const auto queue = m_State->InProcessResponses.find(client);
		if (queue != m_State->InProcessResponses.end())
			responses.swap(queue->second);
		return responses;
	}

}
