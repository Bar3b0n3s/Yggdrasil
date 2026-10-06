#include "TestsPCH.h"

#include "Engine/Automation/Protocol/Dispatcher.h"

#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/RingBufferSink.h"
#include "Support/DeathTest.h"
#include "Support/ProtocolTestTypes.h"

namespace Engine {

	namespace {

		// The test types, methods and host, a ring of the test's own and a dispatcher over them, with clients 1 and 2.
		struct DispatcherSetup
		{
			Scope<TypeRegistry> Types = Test::CreateProtocolTestTypes();
			Scope<MethodRegistry> Methods;
			Test::TestHostState State;
			Test::TestMethodHost Host{ State };
			RingBufferSink Log{ 256 };
			Scope<Dispatcher> Calls;

			explicit DispatcherSetup(DispatcherSpecification specification = {})
			{
				Methods = CreateScope<MethodRegistry>(*Types);
				Test::RegisterProtocolTestMethods(*Methods);
				Methods->Freeze();
				Calls = CreateScope<Dispatcher>(*Methods, Host, Log, nullptr, specification);
				Calls->AddClient(1, "first");
				Calls->AddClient(2, "second");
			}

			// The host calls recorded for `method`, in order, without the method name.
			std::vector<std::string> CallsOf(std::string_view method) const
			{
				std::vector<std::string> calls;
				for (const std::string& call : State.Calls)
				{
					const size_t space = call.find(' ');
					if (space != std::string::npos && call.substr(space + 1) == method)
						calls.push_back(call.substr(0, space));
				}
				return calls;
			}

			// Pumps until no work is left (at most 100 pumps) and returns every message.
			std::vector<OutboundMessage> PumpAll()
			{
				std::vector<OutboundMessage> messages;
				for (int pump = 0; pump < 100 && Calls->HasWork(); ++pump)
				{
					for (OutboundMessage& message : Calls->Pump(std::chrono::microseconds(1000000)))
						messages.push_back(std::move(message));
				}
				return messages;
			}
		};

	}

	ENGINE_DEATH_TEST("Automation/DispatcherHandlerExceptionAssertsInDebug")
	{
		DispatcherSetup setup;
		setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.throw", Json::object()));
		static_cast<void>(setup.Calls->Pump(std::chrono::microseconds(1000000)));
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("Dispatcher: requests of one client run in order and every response carries _meta" * doctest::skip(true))
		{
			DispatcherSetup setup;
			setup.State.Revision = 7;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.echo", Json{ { "text", "a" } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			CHECK(messages[0].Client == 1);
			CHECK(messages[0].Message["id"] == Json(1));
			CHECK(messages[0].Message["result"]["text"] == Json("a"));
			CHECK(messages[0].Message["result"]["_meta"]["revision"] == Json(7));
			CHECK(messages[1].Message["id"] == Json(2));
			CHECK(messages[1].Message["result"]["revision"] == Json(7));
			const std::vector<std::string> calls = { "available test.echo", "admit test.echo", "enter test.echo", "handler test.echo",
				"leave test.echo", "finish test.echo", "available test.read", "admit test.read", "enter test.read", "handler test.read",
				"leave test.read", "finish test.read" };
			CHECK(setup.State.Calls == calls);
		}

		TEST_CASE("Dispatcher: availability is checked before the params are read" * doctest::skip(true))
		{
			DispatcherSetup setup;
			setup.State.LauncherState = true;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.echo", Json::object())); // its required "text" is missing
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			CHECK(messages[0].Message["error"]["code"] == Json(-32002)); // InvalidState, not InvalidParams
			CHECK(messages[0].Message["error"]["data"]["detail"].dump().contains("no project open"));
			CHECK(setup.CallsOf("test.echo") == std::vector<std::string>{ "available" }); // nothing after the refusal
			CHECK(messages[1].Message.contains("result"));
		}

		TEST_CASE("Dispatcher: a pending operation is admitted once and every Poll runs between enter and leave" * doctest::skip(true))
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 2 } }));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 1);
			CHECK(messages[0].Message["result"]["polls"] == Json(2));
			const std::vector<std::string> calls = { "available", "admit", "enter", "leave", "enter", "leave", "enter", "leave", "finish" };
			CHECK(setup.CallsOf("test.pend") == calls);
		}

		TEST_CASE("Dispatcher: Cancel runs on disconnect even while the host refuses new requests" * doctest::skip(true))
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 1000 } }));
			CHECK(setup.Calls->Pump(std::chrono::microseconds(1000000)).empty());
			REQUIRE(setup.Calls->GetPendingCount(1) == 1);

			setup.State.DenyEverything = true; // a stale ifRevision or a read-only switch would refuse an admission now
			setup.Calls->RemoveClient(1);
			// The Cancel runs between enter and leave, and FinishRequest closes the request; it is never admitted again.
			const std::vector<std::string> calls = setup.CallsOf("test.pend");
			REQUIRE(calls.size() >= 4);
			const std::vector<std::string> last(calls.end() - 4, calls.end());
			CHECK(last == std::vector<std::string>{ "enter", "cancel", "leave", "finish" });
			CHECK(std::count(calls.begin(), calls.end(), "admit") == 1);
		}

		TEST_CASE("Dispatcher: a pending operation is polled once per pump and holds back its client's next request" * doctest::skip(true))
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 3 } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			setup.Calls->Enqueue(2, Test::MakeTestRequest(3, "test.read", Json::object()));

			std::vector<OutboundMessage> first = setup.Calls->Pump(std::chrono::microseconds(1000000));
			REQUIRE(first.size() == 1); // client 2 runs while client 1 waits for its operation
			CHECK(first[0].Client == 2);
			CHECK(setup.Calls->GetPendingCount(1) == 1);
			CHECK(setup.Calls->GetQueuedCount(1) == 1);

			CHECK(setup.Calls->Pump(std::chrono::microseconds(1000000)).empty());
			std::vector<OutboundMessage> third = setup.Calls->Pump(std::chrono::microseconds(1000000));
			REQUIRE_FALSE(third.empty());
			CHECK(third[0].Message["id"] == Json(1));
			CHECK(third[0].Message["result"]["polls"] == Json(3));
			std::vector<OutboundMessage> rest = setup.PumpAll();
			CHECK((third.size() == 2 || rest.size() == 1));
		}

		TEST_CASE("Dispatcher: removing a client cancels its pending operations without a response" * doctest::skip(true))
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 1000 } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			CHECK(setup.Calls->Pump(std::chrono::microseconds(1000000)).empty());
			REQUIRE(setup.Calls->GetPendingCount(1) == 1);

			setup.Calls->RemoveClient(1);
			CHECK(setup.Calls->GetPendingCount(1) == 0);
			CHECK(setup.Calls->GetQueuedCount(1) == 0);
			CHECK_FALSE(setup.Calls->HasWork());
			CHECK(std::find(setup.State.Calls.begin(), setup.State.Calls.end(), "cancel test.pend") != setup.State.Calls.end());
			CHECK(setup.PumpAll().empty());
		}

		TEST_CASE("Dispatcher: an exception escaping a handler asserts in Debug builds and is Internal otherwise" * doctest::skip(true))
		{
#if defined(ENGINE_DEBUG)
			ENGINE_CHECK_DEATH("Automation/DispatcherHandlerExceptionAssertsInDebug", "test.throw");
#else
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.throw", Json::object()));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			CHECK(messages[0].Message["error"]["code"] == Json(-32000));
			CHECK(messages[1].Message.contains("result")); // the dispatcher keeps serving
#endif
		}

		TEST_CASE("Dispatcher: unknown methods, invalid params and host refusals are error responses" * doctest::skip(true))
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.ecko", Json::object()));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.echo", Json{ { "count", 2 } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(3, "test.read", Json{ { "dryRun", true } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(4, "test.fail", Json::object()));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 4);
			CHECK(messages[0].Message["error"]["code"] == Json(-32601));
			CHECK(messages[0].Message["error"]["data"]["issues"][0]["hint"].dump().contains("test.echo"));
			CHECK(messages[1].Message["error"]["code"] == Json(-32602));
			CHECK(messages[2].Message["error"]["code"] == Json(-32009));
			CHECK(messages[3].Message["error"]["code"] == Json(-32001));
			CHECK(messages[3].Message["error"]["data"].contains("_meta"));

			setup.State.DenyEverything = true;
			setup.State.Calls.clear();
			setup.Calls->Enqueue(2, Test::MakeTestRequest(5, "test.read", Json::object()));
			std::vector<OutboundMessage> denied = setup.PumpAll();
			REQUIRE(denied.size() == 1);
			CHECK(denied[0].Message["error"]["code"] == Json(-32008));
			// A refused request never reaches its handler, and FinishRequest still runs.
			const std::vector<std::string> calls = { "available", "admit", "finish" };
			CHECK(setup.CallsOf("test.read") == calls);
		}

		TEST_CASE("Dispatcher: results over the threshold are offloaded through the host" * doctest::skip(true))
		{
			DispatcherSetup setup(DispatcherSpecification{ .OffloadThresholdBytes = 1024 });
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.large", Json{ { "count", 100 } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.large", Json{ { "count", 1 } }));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			Json& offloaded = messages[0].Message["result"];
			CHECK(offloaded["truncated"] == Json(true));
			CHECK(offloaded["path"] == Json(std::format("/offload/{}-00000001.json", Test::TestOffloadServerTag)));
			CHECK(offloaded["summary"]["items"]["count"] == Json(100));
			CHECK(offloaded.contains("_meta"));
			REQUIRE(setup.State.Offloaded.size() == 1);
			CHECK(setup.State.Offloaded[0].second.contains("\"items\""));
			CHECK_FALSE(messages[1].Message["result"].contains("truncated"));
		}

		TEST_CASE("Dispatcher: a client added without offloading keeps large results inline" * doctest::skip(true))
		{
			DispatcherSetup setup(DispatcherSpecification{ .OffloadThresholdBytes = 1024 });
			setup.Calls->AddClient(3, "batch", false);
			setup.Calls->Enqueue(3, Test::MakeTestRequest(1, "test.large", Json{ { "count", 100 } }));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 1);
			CHECK_FALSE(messages[0].Message["result"].contains("truncated"));
			CHECK(messages[0].Message["result"]["items"].size() == 100);
			CHECK(setup.State.Offloaded.empty());
		}

		TEST_CASE("Dispatcher: dry runs carry dryRun and notifications get no response" * doctest::skip(true))
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.echo", Json{ { "text", "a" }, { "dryRun", true } }));
			RpcRequest notification = Test::MakeTestRequest(0, "test.read", Json::object());
			notification.Id = Json();
			notification.IsNotification = true;
			setup.Calls->Enqueue(1, std::move(notification));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 1);
			CHECK(messages[0].Message["result"]["dryRun"] == Json(true));
			CHECK(std::count(setup.State.Calls.begin(), setup.State.Calls.end(), "handler test.read") == 1);
		}
	}

}
