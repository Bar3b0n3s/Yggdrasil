#include "TestsPCH.h"

#include "EditorCore/Automation/ShaderMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "Engine/Core/Json/Json.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

// shaders.reload (M18, R-SHR; Docs/Decisions/0029-m18-contract.md decision 3) through the in-process server with an
// injected reload.

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("ShaderMethods: shaders.reload reports the swapped-in pipelines and passes a failed reload's error on")
		{
			Test::EditorTestFixture fixture("ShadersReload");
			uint32_t reloads = 0;
			bool fail = false;
			auto specification = Test::MakeTestServerSpecification();
			specification.RendererName = "vulkan";
			specification.ReloadShaders = [&reloads, &fail]() -> Result<ShadersReloadResult>
			{
				if (fail)
					return MakeError(ErrorCode::Validation, "while reloading the shaders; the previous pipelines stay in use: Tonemap/CSMain no longer matches its layout");
				++reloads;
				return ShadersReloadResult{ .PipelineCount = 58, .Generation = 1 + reloads };
			};
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			// Not in the launcher state.
			CHECK(client.Call("shaders.reload", Json::object()).error().GetCode() == ErrorCode::InvalidState);
			CHECK(reloads == 0);
			fixture.CreateAndOpenProject();
			const Result<Json> result = client.Call("shaders.reload", Json::object());
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK((*result)["pipelineCount"] == 58);
			CHECK((*result)["generation"] == 2);
			CHECK(reloads == 1);
			// A failed reload's error reaches the caller unchanged; the host kept its pipelines.
			fail = true;
			const Result<Json> refused = client.Call("shaders.reload", Json::object());
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			CHECK(refused.error().ToString().contains("previous pipelines stay in use"));
			CHECK(reloads == 1);
			const auto* method = client.GetServer().GetMethods().Find("shaders.reload");
			REQUIRE(method != nullptr);
			CHECK_FALSE(method->Specification.ExposeAsTool);
			CHECK_FALSE(method->Specification.Mutates);
			CHECK_FALSE(method->Specification.SupportsDryRun);
			CHECK_FALSE(method->Specification.AllowedInBatch);
			CHECK_FALSE(method->Specification.AvailableInRuntime);
			CHECK_FALSE(method->Specification.AvailableInLauncher);
		}

		TEST_CASE("ShaderMethods: shaders.reload is Unsupported without a renderer")
		{
			// --renderer none: the editor injects no reload.
			Test::EditorTestFixture fixture("ShadersReloadNone");
			fixture.CreateAndOpenProject();
			Test::AutomationTestClient client(fixture.GetEditor());
			const Result<Json> result = client.Call("shaders.reload", Json::object());
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Unsupported);
		}
	}

}
