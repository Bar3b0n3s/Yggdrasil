#include "TestsPCH.h"

#include "EditorCore/Automation/DebugMethods.h"

#include "Support/AutomationTestClient.h"

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("DebugMethods: debug.stall blocks the main thread with its phase set" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("DebugStall");
			Test::AutomationTestClient client(fixture.GetEditor());
			Result<Json> stalled = client.Call("debug.stall", Json{ { "ms", 10 } });
			REQUIRE(stalled.has_value());
			CHECK((*stalled)["stalledMs"] == Json(10));
			CHECK(client.Call("debug.stall", Json{ { "ms", 60001 } }).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("DebugMethods: debug.pend resolves after its frames" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("DebugPend");
			Test::AutomationTestClient client(fixture.GetEditor());
			Json response = client.Request("debug.pend", Json{ { "frames", 3 } });
			CHECK(response["result"]["frames"] == Json(3));
		}

		TEST_CASE("DebugMethods: the hooks exist only with test hooks enabled" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("DebugHooksOff");
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.TestHooks = false;
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			Json response = client.Request("debug.stall", Json{ { "ms", 1 } });
			CHECK(response["error"]["code"] == Json(-32601));
			CHECK(client.GetServer().GetMethods().Find("debug.pend") == nullptr);
		}
	}

}
