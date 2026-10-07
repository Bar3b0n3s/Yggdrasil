#pragma once

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string_view>

// In-process automation for EditorCore tests (Architecture §15.2: "an in-process round trip for every method"): an
// AutomationServer that does not listen, and one in-process client whose requests take the same Dispatcher path as TCP
// requests.

namespace Engine {

	namespace Test {

		// The server options tests use unless they need others: no TCP, test hooks on, the repository as the docs root.
		[[nodiscard]] AutomationServerSpecification MakeTestServerSpecification();

		// One server and one client of one test. Not copyable or movable.
		class AutomationTestClient
		{
		public:
			// Creates the server over `editor` (a documented back-reference that outlives the client) and connects the client
			// "test"; fails the test case on error. With `offloadLargeResults` false the client receives every result inline,
			// like BatchRunner's (AutomationServer::ConnectInProcess), so a test can inspect a result over the offload threshold.
			explicit AutomationTestClient(EditorContext& editor, AutomationServerSpecification specification = MakeTestServerSpecification(),
				bool offloadLargeResults = true);
			~AutomationTestClient();

			AutomationTestClient(const AutomationTestClient&) = delete;
			AutomationTestClient& operator=(const AutomationTestClient&) = delete;

			// Sends {"jsonrpc": "2.0", "id": <next id>, "method": method, "params": params} and pumps the server until its
			// response arrives; fails the test case after `maxPumps` pumps without one. Returns the whole response message.
			[[nodiscard]] Json Request(std::string_view method, const Json& params, uint32_t maxPumps = 1000);

			// Request, then the result object of a success, or an Error whose code is the response's data.errorCode, whose
			// message is data.detail and whose issues are data.issues.
			[[nodiscard]] Result<Json> Call(std::string_view method, const Json& params);

			// Submits without waiting (pending operations, disconnect tests); returns the request id.
			[[nodiscard]] int64_t Submit(std::string_view method, const Json& params);

			[[nodiscard]] AutomationServer& GetServer() { return *m_Server; }
			[[nodiscard]] ClientId GetClient() const { return m_Client; }
		private:
			Scope<AutomationServer> m_Server;
			ClientId m_Client = NoClient;
			int64_t m_NextId = 1;
		};

		// The usual setup of a method test: an EditorTestFixture with a project open (and, unless `openScene` is false, the
		// scene Assets/Scenes/Main.scene open), and an AutomationTestClient on its editor. Not copyable or movable.
		class AutomationFixture
		{
		public:
			explicit AutomationFixture(std::string_view label, bool openScene = true);

			AutomationFixture(const AutomationFixture&) = delete;
			AutomationFixture& operator=(const AutomationFixture&) = delete;

			[[nodiscard]] EditorTestFixture& GetEditorFixture() { return m_Fixture; }
			[[nodiscard]] EditorContext& GetEditor() { return m_Fixture.GetEditor(); }
			[[nodiscard]] AutomationTestClient& GetClient() { return *m_Client; }

			// AutomationTestClient::Call and Request.
			[[nodiscard]] Result<Json> Call(std::string_view method, const Json& params) { return m_Client->Call(method, params); }
			[[nodiscard]] Json Request(std::string_view method, const Json& params) { return m_Client->Request(method, params); }
		private:
			EditorTestFixture m_Fixture;
			Scope<AutomationTestClient> m_Client;
		};

	}

}
