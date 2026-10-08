#include "TestsPCH.h"

#include "Engine/Automation/Methods/RpcMethods.h"

#include "Support/AutomationTestClient.h"

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("RpcMethods: rpc.discover lists every method with schemas and flags")
		{
			Test::EditorTestFixture fixture("RpcDiscover");
			// The whole catalogue, with full schemas, is far over the offload threshold (§13.4 bounded output): a client that
			// allows offloading gets a file, so this test reads the result inline through one that does not.
			{
				Test::AutomationTestClient bounded(fixture.GetEditor());
				Result<Json> offloaded = bounded.Call("rpc.discover", Json::object());
				REQUIRE(offloaded.has_value());
				CHECK((*offloaded)["truncated"] == Json(true));
				CHECK((*offloaded)["summary"].contains("methods"));
			}
			Test::AutomationTestClient client(fixture.GetEditor(), Test::MakeTestServerSpecification(), false);
			Result<Json> discovered = client.Call("rpc.discover", Json::object());
			REQUIRE(discovered.has_value());
			CHECK((*discovered)["protocolVersion"] == Json("1.0"));
			CHECK((*discovered)["methods"].size() == client.GetServer().GetMethods().GetMethods().size());
			bool sawEntityCreate = false;
			for (Json& method : (*discovered)["methods"])
			{
				if (method["name"] == Json("entity.create"))
				{
					sawEntityCreate = true;
					CHECK(method["mutates"] == Json(true));
					CHECK(method["supportsDryRun"] == Json(true));
					CHECK(method["exposeAsTool"] == Json(true));
					CHECK(method["params"]["$defs"].contains("RigidBody"));
					// Component values in params may set writable virtual fields, never read-only ones.
					CHECK(method["params"]["$defs"]["Transform"]["properties"].contains("EulerAngles"));
					CHECK_FALSE(method["params"]["$defs"]["Transform"]["properties"].contains("WorldScale"));
					CHECK(method["result"]["properties"].contains("undoIndex"));
				}
			}
			CHECK(sawEntityCreate);
			CHECK((*discovered)["domains"].dump().contains("\"edit\""));
		}

		TEST_CASE("RpcMethods: rpc.discover filters by method and by domain")
		{
			Test::EditorTestFixture fixture("RpcFilter");
			Test::AutomationTestClient client(fixture.GetEditor());
			Result<Json> one = client.Call("rpc.discover", Json{ { "method", "scene.tree" } });
			REQUIRE(one.has_value());
			REQUIRE((*one)["methods"].size() == 1);
			CHECK((*one)["methods"][0]["name"] == Json("scene.tree"));

			Result<Json> session = client.Call("rpc.discover", Json{ { "domain", "session" } });
			REQUIRE(session.has_value());
			CHECK((*session)["methods"].size() == 3);

			const Result<Json> unknown = client.Call("rpc.discover", Json{ { "method", "scene.tre" } });
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
			const Result<Json> both = client.Call("rpc.discover", Json{ { "method", "scene.tree" }, { "domain", "scene" } });
			REQUIRE_FALSE(both.has_value());
			CHECK(both.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
