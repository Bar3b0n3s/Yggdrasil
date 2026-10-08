#pragma once

#include "Engine/Automation/Protocol/Dispatcher.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The Runtime's automation server (Architecture §13.2, §13.5 "Runtime subset", §13.9 `Runtime --automation[=port]`): the
// protocol of the editor's AutomationServer (ProtocolServer, Dispatcher, MethodRegistry, session file, handshake, _meta)
// hosted on the Runtime's play session, serving the methods the Editor and the Runtime share
// (RegisterSharedMethods with AutomationHost::Runtime), typed on AutomationMethodContext. It does not exist in Dist builds
// (§13.2, §14.4): the Runtime creates it only under #if !defined(ENGINE_DIST) when --automation is given, so the Dist map
// file lists none of its symbols (M15's check). Frozen by the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 12).
//
// Differences from the editor's server: no project and no edit scene (SceneTarget "edit" is InvalidState); no launcher
// state; no Mutates refusals beyond those of the methods; the revision in "_meta" is the play scene's revision; "_meta"
// reports PlayState ("Play" or "Paused") and the tick; the session file names the game's name in "projectPath" and lives
// under <UserData>/<Name>/Automation/Sessions (the manifest's Name, §14.1), so a client finds an exported game by its
// name; output files (screenshots) and offloaded results go to user://Automation/Out/. A disconnect releases the client's
// lockstep and pauses play, like the editor (§13.2).

namespace Engine {

	class EventLog;
	class PlaySession;
	class TypeRegistry;
	class VirtualFileSystem;
	struct RenderSnapshot;
	struct ViewportScreenshotRequest;

	struct RuntimeAutomationServerSpecification
	{
		// Listen on TCP 127.0.0.1 and write the session file (--automation); without it only in-process clients exist (tests).
		bool Listen = false;
		// --automation=<port>; 0: OS-assigned.
		uint16_t Port = 0;
		// The game's name (GameManifest::Name), reported by session.info and in the session file's projectPath.
		std::string GameName{};
		// Reported by session.info and in the session file.
		bool Headless = false;
		std::string RendererName = "none"; // "vulkan" or "none"
		// <UserData>/<Name>/Automation/Sessions (§13.2); required when Listen.
		std::filesystem::path SessionsDirectory{};
		// The Runtime's ViewportCapture (AutomationMethodContext::CaptureView); empty with --renderer none, which makes
		// viewport.screenshot Unsupported.
		std::function<Result<Image>(const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request)> View{};
		// As AutomationServerSpecification::SystemErrors: the Runtime maps Vulkan errors like its frame-boundary catch.
		SystemErrorHandler SystemErrors{};
		// §4.2 step 3: the pump's budget per frame.
		std::chrono::microseconds PumpBudget{ 4000 };
		std::chrono::milliseconds WatchdogStallThreshold = DefaultWatchdogStallThreshold;
	};

	// Main thread only (its ProtocolServer's I/O thread is internal); not copyable or movable.
	class RuntimeAutomationServer
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class RuntimeAutomationServer;
		};

		// Use Create.
		explicit RuntimeAutomationServer(ConstructionKey key);
		// Stops the transport, cancels every pending operation and removes the session file.
		~RuntimeAutomationServer();

		RuntimeAutomationServer(const RuntimeAutomationServer&) = delete;
		RuntimeAutomationServer& operator=(const RuntimeAutomationServer&) = delete;

		// Builds the method registry over `registry` (on which RegisterAutomationSharedTypes and RegisterSharedMethodTypes
		// ran before it froze: the Runtime's EngineContextSpecification::RegisterTypes), the Dispatcher and, with Listen, the
		// transport and the session file. `registry`, `events`, `vfs` (user:// for output files) and `session` are documented
		// back-references that outlive the server. Errors: those of GenerateAuthToken, ProtocolServer::Start (AlreadyExists for
		// a port in use) and SessionFile::Write, with nothing left running.
		[[nodiscard]] static Result<Scope<RuntimeAutomationServer>> Create(const TypeRegistry& registry, EventLog& events, VirtualFileSystem& vfs,
			PlaySession& session, const RuntimeAutomationServerSpecification& specification);

		// The frame's automation work at the safe point (§4.2 step 3), as AutomationServer::Pump.
		void Pump();

		// The bound TCP port; 0 without Listen.
		[[nodiscard]] uint16_t GetPort() const;
		[[nodiscard]] const MethodRegistry& GetMethods() const;
		// session.shutdown's request: the exit code the Runtime ends with after the current frame.
		[[nodiscard]] std::optional<int> GetShutdownRequest() const;

		// In-process clients for the C++ tests, as AutomationServer's.
		[[nodiscard]] ClientId ConnectInProcess(std::string name);
		void DisconnectInProcess(ClientId client);
		void SubmitInProcess(ClientId client, RpcRequest request);
		[[nodiscard]] std::vector<Json> TakeInProcessResponses(ClientId client);
	private:
		// The registry, the Dispatcher, the ProtocolServer, the token, the session file, the clients, the back-references and
		// the shutdown request (RuntimeAutomationServer.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
