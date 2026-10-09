#include "TestsPCH.h"
#include "EditorCore/Automation/ViewportMethods.h"

#include "EditorCore/Automation/Private/M10MethodFixture.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "Engine/Scene/Entity.h"

#include <limits>

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("ViewportMethods: camera gets and sets only present members")
		{
			Test::M10MethodFixture setup;
			const auto before = setup.Call("viewport.camera");
			REQUIRE(before.has_value());
			const auto changed = setup.Call("viewport.camera", Json{ { "position", { 6, 4, 8 } } });
			REQUIRE(changed.has_value());
			CHECK((*changed)["target"] == (*before)["target"]);
			CHECK((*changed)["position"] == Json::array({ 6, 4, 8 }));
			CHECK(setup.Call("viewport.camera")->dump() == changed->dump());
			const auto target = setup.Call("viewport.camera", Json{ { "target", { 1, 0, 0 } } });
			REQUIRE(target.has_value());
			CHECK((*target)["position"] == (*changed)["position"]);
		}

		TEST_CASE("ViewportMethods: frame resolves every entity before moving the camera")
		{
			Test::M10MethodFixture setup;
			{
				SceneEdit edit(setup.Fixture.GetEditor(), "Create");
				static_cast<void>(setup.Fixture.GetEditor().GetScene().CreateEntity("Found"));
				REQUIRE(edit.Commit().has_value());
			}
			const auto before = setup.Call("viewport.camera");
			const auto failed = setup.Call("viewport.frame", Json{ { "entities", { "/Found", "/Missing" } } });
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.Call("viewport.camera")->dump() == before->dump());
			const auto framed = setup.Call("viewport.frame", Json{ { "entities", { "/Found" } } });
			REQUIRE(framed.has_value());
			CHECK((*framed)["entities"][0]["name"] == "Found");
			CHECK((*framed)["target"] == Json::array({ 0, 0, 0 }));
		}

		TEST_CASE("ViewportMethods: setOptions patches only supplied options")
		{
			Test::M10MethodFixture setup;
			const auto before = setup.Call("viewport.setOptions");
			const auto result = setup.Call("viewport.setOptions", Json{ { "grid", false }, { "wireframe", true } });
			REQUIRE(result.has_value());
			CHECK((*result)["grid"] == false);
			CHECK((*result)["wireframe"] == true);
			for (std::string_view name : { "gizmos", "colliders", "icons" })
				CHECK((*result)[name] == (*before)[name]);
		}

		TEST_CASE("ViewportMethods: methods work headless without a GPU")
		{
			Test::M10MethodFixture setup;
			CHECK(setup.Call("viewport.camera").has_value());
			CHECK(setup.Call("viewport.setOptions", Json{ { "colliders", true } }).has_value());
			for (std::string_view name : { "viewport.camera", "viewport.frame", "viewport.setOptions" })
			{
				const auto& specification = setup.Methods.Find(name)->Specification;
				CHECK_FALSE(specification.AvailableInRuntime);
				CHECK_FALSE(specification.AvailableInLauncher);
				CHECK_FALSE(specification.AllowedInBatch);
				CHECK_FALSE(specification.SupportsDryRun);
				CHECK_FALSE(specification.Mutates);
			}
		}

		TEST_CASE("ViewportMethods: viewport state changes preserve scene revision and undo history")
		{
			Test::M10MethodFixture setup;
			const auto revision = setup.Fixture.GetEditor().GetRevision();
			const auto history = setup.Fixture.GetEditor().GetHistory().GetCurrentSequence();
			REQUIRE(setup.Call("viewport.camera", Json{ { "position", { 6, 4, 8 } } }).has_value());
			REQUIRE(setup.Call("viewport.setOptions", Json{ { "grid", false } }).has_value());
			CHECK(setup.Fixture.GetEditor().GetRevision() == revision);
			CHECK(setup.Fixture.GetEditor().GetHistory().GetCurrentSequence() == history);
			CHECK_FALSE(setup.Fixture.GetEditor().IsSceneDirty());
		}

		TEST_CASE("ViewportMethods: nonfinite camera and empty frame requests return located errors")
		{
			Test::M10MethodFixture setup;
			const auto empty = setup.Call("viewport.frame", Json{ { "entities", Json::array() } });
			REQUIRE_FALSE(empty.has_value());
			CHECK(empty.error().ToString().contains("/entities"));
			auto context = setup.Context("viewport.camera", Json{ { "position", { 1, 2, 3 } } });
			ViewportCameraParams params;
			params.Position.x = std::numeric_limits<float>::quiet_NaN();
			const auto result = Automation::ViewportCamera(*context, params);
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().ToString().contains("/position"));
			const auto coincident = setup.Call("viewport.camera", Json{ { "position", { 1, 2, 3 } }, { "target", { 1, 2, 3 } } });
			CHECK_FALSE(coincident.has_value());
		}
	}

}
