#include "TestsPCH.h"

#include "Engine/Automation/Methods/RaycastMethods.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Automation/Methods/Private/RenderQueryTestContext.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Support/AutomationTestClient.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <variant>

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("SceneRaycast: in-process round trip returns the triangle and located errors")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			ProjectSettings settings;
			TypeRegistry types;
			RegisterAutomationSharedTypes(types);
			RegisterRaycastMethodTypes(types);
			types.Freeze();
			MethodRegistry methods(types);
			RegisterRaycastMethods(methods);
			methods.Freeze();
			const auto* method = methods.Find("scene.raycast");
			REQUIRE(method != nullptr);
			CHECK(method->Specification.SupportsDryRun);
			CHECK(method->Specification.AllowedInBatch);
			CHECK(method->Specification.AvailableInRuntime);
			CHECK_FALSE(method->Specification.Mutates);
			Entity cube = fixture.GetScene().CreateEntity("Cube");
			cube.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			const Json params{ { "origin", { 0, 0, 5 } }, { "direction", { 0, 0, -2 } }, { "maxDistance", 10 } };
			Test::RenderQueryTestContext context(fixture.GetScene(), assets, settings, { .Method = method, .Params = params, .Registry = &methods });
			const uint64_t revision = fixture.GetScene().GetRevision();
			const auto result = methods.Invoke(context);
			REQUIRE(std::holds_alternative<Json>(result));
			const auto& json = std::get<Json>(result);
			CHECK(json["hit"] == Json(true));
			CHECK(json["entity"]["name"] == Json("Cube"));
			CHECK(json["distance"] == Json(4.5f));
			CHECK(json["normal"] == Json::array({ 0, 0, 1 }));
			CHECK(fixture.GetScene().GetRevision() == revision);
			const auto invalid = Automation::SceneRaycast(context, { .Direction = glm::vec3(0) });
			REQUIRE_FALSE(invalid);
			CHECK(invalid.error().GetLocation().JsonPointer == "/direction");
			const auto miss = Automation::SceneRaycast(context, { .Origin = { 0, 0, 5 }, .LayerMask = 0 });
			REQUIRE(miss);
			CHECK_FALSE(miss->Hit);
			context.HasAssets = false;
			const auto absent = Automation::SceneRaycast(context, {});
			REQUIRE_FALSE(absent);
			CHECK(absent.error().GetCode() == ErrorCode::Unsupported);
			context.HasAssets = true;
			context.HasSettings = false;
			CHECK_FALSE(Automation::SceneRaycast(context, {}));
			const auto required = methods.PrepareParams(*method, Json::object());
			CHECK_FALSE(required);
		}
		TEST_CASE("SceneRaycast: editor dispatcher registers the shared visual query")
		{
			Test::AutomationFixture setup("VisualQuery");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", "engine://Meshes/Cube" } } } } } }));
			const auto hit = setup.Call("scene.raycast", Json{ { "origin", { 0, 0, 5 } }, { "direction", { 0, 0, -1 } }, { "maxDistance", 10 } });
			REQUIRE_MESSAGE(hit.has_value(), hit.error().ToString());
			CHECK((*hit)["entity"]["name"] == Json("Cube"));
		}
	}

}
