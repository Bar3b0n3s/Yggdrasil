#include "TestsPCH.h"
#include "Engine/Automation/Methods/PlayMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <nlohmann/json.hpp>

#include <chrono>

namespace Engine {

	namespace {

		Json WaitCall(Test::AutomationFixture& fixture, std::string_view method, const Json& params)
		{
			const auto result = fixture.Call(method, params);
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			return *result;
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("PlayWaitFor: evaluates after every tick and returns the satisfying value")
		{
			Test::AutomationFixture fixture("WaitForValue");
			WaitCall(fixture, "entity.create", Json{ { "name", "Counter" } });
			WaitCall(fixture, "play.start", Json{ { "lockstep", true } });
			const Json result = WaitCall(fixture, "play.waitFor", Json{ { "until", "local e = Scene.FindByName('Counter'); e.Name = e.Name; return Time.GetTick() >= 3 and { answer = 42 }" }, { "timeoutTicks", 10 } });
			CHECK(result["satisfied"] == Json(true));
			CHECK(result["value"] == Json({ { "answer", 42 } }));
			CHECK(result["tick"] == Json(3));
			CHECK_FALSE(fixture.GetEditor().GetPlay().GetSession()->IsStepping());
			const Json zero = WaitCall(fixture, "play.waitFor", Json{ { "until", "0" }, { "timeoutTicks", 5 } });
			CHECK(zero["satisfied"] == Json(true));
			CHECK(zero["tick"] == Json(4));
			CHECK(zero["value"] == Json(0));
		}

		TEST_CASE("PlayWaitFor: timeout reports false while script faults retain their source location")
		{
			Test::AutomationFixture fixture("WaitForFailure");
			WaitCall(fixture, "play.start", Json{ { "paused", true } });
			const Json timed = WaitCall(fixture, "play.waitFor", Json{ { "until", "false" }, { "timeoutTicks", 3 } });
			CHECK(timed["satisfied"] == Json(false));
			CHECK(timed["value"] == Json(false));
			CHECK(timed["tick"] == Json(3));
			{
				Test::ExpectLog expected(LogLevel::Error, "predicate failure");
				const Json failure = fixture.Request("play.waitFor", Json{ { "until", "local n = 1\nerror('predicate failure')" }, { "timeoutTicks", 9 } });
				CHECK(failure["error"]["data"]["errorCode"] == Json("Script"));
				CHECK(failure["error"]["data"]["scriptError"]["line"] == Json(2));
			}
			CHECK_FALSE(fixture.GetEditor().GetPlay().GetSession()->IsStepping());
			CHECK(WaitCall(fixture, "play.state", Json::object())["state"] == Json("Paused"));
		}

		TEST_CASE("PlayWaitFor: syntax errors and unavailable VMs do not advance the session")
		{
			Test::AutomationFixture fixture("WaitForAdmission");
			WaitCall(fixture, "play.start", Json{ { "lockstep", true } });
			CHECK(fixture.Request("play.waitFor", Json{ { "until", "return (" }, { "timeoutTicks", 3 } }).contains("error"));
			CHECK(WaitCall(fixture, "play.state", Json::object())["tick"] == Json(0));
			CHECK_FALSE(fixture.GetEditor().GetPlay().GetSession()->IsStepping());
			WaitCall(fixture, "play.stop", Json::object());
			WaitCall(fixture, "play.start", Json{ { "mode", "Simulate" }, { "paused", true } });
			CHECK(fixture.Request("play.waitFor", Json{ { "until", "true" }, { "timeoutTicks", 1 } })["error"]["data"]["errorCode"] == Json("InvalidState"));
			CHECK(WaitCall(fixture, "play.state", Json::object())["tick"] == Json(0));
		}

		TEST_CASE("PlayWaitFor: disconnect cancels without another poll and releases the stepping flag")
		{
			Test::EditorTestFixture fixture("WaitForCancel");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient client(fixture.GetEditor(), std::move(specification));
			REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }).has_value());
			static_cast<void>(client.Submit("play.waitFor", Json{ { "until", "false" }, { "timeoutTicks", 1000 } }));
			client.GetServer().Pump();
			PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			REQUIRE(session->IsStepping());
			const uint64_t tick = session->GetTick();
			client.GetServer().DisconnectInProcess(client.GetClient());
			CHECK_FALSE(session->IsStepping());
			CHECK_FALSE(session->IsLockstep());
			CHECK(session->IsPaused());
			CHECK(session->GetTick() == tick);
		}
	}

}
