#include "TestsPCH.h"

#include "EditorCore/Automation/DebugMethods.h"

#include "Support/AutomationTestClient.h"

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("DebugMethods: device loss delegates only through the rendering host test callback")
		{
			Test::EditorTestFixture fixture("DebugDeviceLost");
			uint32_t queued = 0;
			bool fail = false;
			auto specification = Test::MakeTestServerSpecification();
			specification.RendererName = "vulkan";
			specification.QueueDeviceLost = [&queued, &fail]() -> Status
			{
				if (fail)
					return MakeError(ErrorCode::Conflict, "Injected queue refusal");
				++queued;
				return {};
			};
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			CHECK(client.Call("debug.deviceLost", Json::object()).error().GetCode() == ErrorCode::InvalidState);
			CHECK(queued == 0);
			fixture.CreateAndOpenProject();
			const auto result = client.Call("debug.deviceLost", Json::object());
			REQUIRE(result.has_value());
			CHECK((*result)["queued"] == true);
			CHECK(queued == 1);
			fail = true;
			const auto refused = client.Call("debug.deviceLost", Json::object());
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Conflict);
			CHECK(refused.error().ToString().contains("Injected queue refusal"));
			CHECK(queued == 1);
			const auto* method = client.GetServer().GetMethods().Find("debug.deviceLost");
			REQUIRE(method != nullptr);
			CHECK(method->Specification.TestHook);
			CHECK_FALSE(method->Specification.ExposeAsTool);
			CHECK_FALSE(method->Specification.AvailableInRuntime);
			CHECK_FALSE(method->Specification.AvailableInLauncher);
			CHECK_FALSE(method->Specification.AllowedInBatch);
			CHECK_FALSE(method->Specification.SupportsDryRun);
			CHECK_FALSE(client.Call("debug.deviceLost", Json{ { "dryRun", true } }).has_value());
			CHECK_FALSE(client.GetServer().GetMethods().BuildToolCatalog().dump().contains("debug.deviceLost"));
			CHECK_FALSE(client.GetServer().GetMethods().BuildMethodCatalog().dump().contains("debug.deviceLost"));
			CHECK(queued == 1);
		}

		TEST_CASE("DebugMethods: device loss without a rendering callback is Unsupported")
		{
			Test::EditorTestFixture fixture("DebugDeviceLostUnavailable");
			fixture.CreateAndOpenProject();
			uint32_t queued = 0;
			auto specification = Test::MakeTestServerSpecification();
			SUBCASE("rendering host without callback")
			{
				specification.RendererName = "vulkan";
			}
			SUBCASE("renderer none even with callback")
			{
				specification.QueueDeviceLost = [&queued]() -> Status
				{
					++queued;
					return {};
				};
			}
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			const auto result = client.Call("debug.deviceLost", Json::object());
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Unsupported);
			CHECK(queued == 0);
		}

		TEST_CASE("DebugMethods: debug.stall blocks the main thread with its phase set")
		{
			Test::EditorTestFixture fixture("DebugStall");
			Test::AutomationTestClient client(fixture.GetEditor());
			// Like every method outside §12.1's list, the hooks need an open project.
			CHECK(client.Call("debug.stall", Json{ { "ms", 10 } }).error().GetCode() == ErrorCode::InvalidState);
			fixture.CreateAndOpenProject();
			Result<Json> stalled = client.Call("debug.stall", Json{ { "ms", 10 } });
			REQUIRE(stalled.has_value());
			CHECK((*stalled)["stalledMs"] == Json(10));
			CHECK(client.Call("debug.stall", Json{ { "ms", 60001 } }).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("DebugMethods: debug.pend resolves after its frames")
		{
			Test::EditorTestFixture fixture("DebugPend");
			fixture.CreateAndOpenProject();
			Test::AutomationTestClient client(fixture.GetEditor());
			Json response = client.Request("debug.pend", Json{ { "frames", 3 } });
			CHECK(response["result"]["frames"] == Json(3));
		}

		TEST_CASE("DebugMethods: the hooks exist only with test hooks enabled")
		{
			Test::EditorTestFixture fixture("DebugHooksOff");
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.TestHooks = false;
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			Json response = client.Request("debug.stall", Json{ { "ms", 1 } });
			CHECK(response["error"]["code"] == Json(-32601));
			CHECK(client.GetServer().GetMethods().Find("debug.pend") == nullptr);
			CHECK(client.GetServer().GetMethods().Find("debug.deviceLost") == nullptr);
			CHECK(client.Request("debug.deviceLost", Json::object())["error"]["code"] == Json(-32601));
		}
	}

}
