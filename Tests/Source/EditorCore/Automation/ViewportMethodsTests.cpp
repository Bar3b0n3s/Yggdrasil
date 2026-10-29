#include "TestsPCH.h"
#include "EditorCore/Automation/ViewportMethods.h"

#include "EditorCore/Automation/Private/M10MethodFixture.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Entity.h"

#include <limits>
#include <optional>
#include <string>

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

		// M20 (Docs/Decisions/0031-m20-contract.md decisions 13 and 23): viewport.frame frames entities of the scene the scene
		// view shows: the play scene in the played scene's tab, and in another open scene's tab its edit scene.
		TEST_CASE("ViewportMethods: frame resolves in another open scene's edit scene while a session plays")
		{
			Test::M10MethodFixture setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			{
				SceneEdit edit(editor, "Create");
				static_cast<void>(editor.GetScene().CreateEntity("Played"));
				REQUIRE(edit.Commit().has_value());
			}
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Edited" }, { "components", Json{ { "Transform", Json{ { "Translation", { 4, 0, 0 } } } } } } }).has_value());
			const OpenSceneId second = editor.GetActiveSceneId();
			REQUIRE(setup.Call("scene.activate", Json{ { "scene", main } }).has_value());
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true } }).has_value());
			REQUIRE(setup.Call("scene.activate", Json{ { "scene", second } }).has_value());

			const auto framed = setup.Call("viewport.frame", Json{ { "entities", { "/Edited" } } });
			REQUIRE_MESSAGE(framed.has_value(), framed.error().ToString());
			CHECK((*framed)["entities"][0]["name"] == "Edited");
			CHECK((*framed)["target"] == Json::array({ 4, 0, 0 }));
			const auto missing = setup.Call("viewport.frame", Json{ { "entities", { "/Played" } } });
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);

			REQUIRE(setup.Call("scene.activate", Json{ { "scene", main } }).has_value());
			const auto played = setup.Call("viewport.frame", Json{ { "entities", { "/Played" } } });
			REQUIRE_MESSAGE(played.has_value(), played.error().ToString());
			CHECK((*played)["entities"][0]["name"] == "Played");
			REQUIRE(setup.Call("play.stop").has_value());
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

		TEST_CASE("ViewportMethods: camera reads and sets the projection, field of view, orthographic size and clip range")
		{
			Test::M10MethodFixture setup;
			const auto initial = setup.Call("viewport.camera");
			REQUIRE(initial.has_value());
			CHECK((*initial)["projection"] == "Perspective");
			CHECK((*initial)["verticalFov"] == 60);
			CHECK(JsonReader((*initial)["nearClip"]).ReadFloat().value_or(0.0f) == 0.1f);
			CHECK((*initial)["farClip"] == 1000);
			// What the scene view's Orthographic overlay toggle does, observed and reverted through automation.
			EditorViewportState& state = setup.Fixture.GetEditor().GetViewportState();
			ExplicitRenderCamera toggled = state.GetCamera();
			toggled.Projection = RenderProjection::Orthographic;
			REQUIRE(state.SetCamera(toggled).has_value());
			CHECK((*setup.Call("viewport.camera"))["projection"] == "Orthographic");
			const auto changed = setup.Call("viewport.camera", Json{ { "projection", "perspective" }, { "verticalFov", 45 }, { "orthographicSize", 4 }, { "nearClip", 0.5 }, { "farClip", 250 } });
			REQUIRE_MESSAGE(changed.has_value(), (changed ? std::string() : changed.error().ToString()));
			CHECK((*changed)["projection"] == "Perspective");
			CHECK((*changed)["verticalFov"] == 45);
			CHECK((*changed)["orthographicSize"] == 4);
			CHECK((*changed)["nearClip"] == 0.5);
			CHECK((*changed)["farClip"] == 250);
			CHECK((*changed)["position"] == (*initial)["position"]);
			CHECK(state.GetCamera().Projection == RenderProjection::Perspective);
			CHECK(state.GetCamera().VerticalFov == 45.0f);
			CHECK(state.GetCamera().FarClip == 250.0f);
			// Invalid members are located and change nothing.
			for (const auto& [params, pointer] : { std::pair{ Json{ { "verticalFov", 180 } }, "/verticalFov" }, std::pair{ Json{ { "orthographicSize", 0 } }, "/orthographicSize" },
					 std::pair{ Json{ { "nearClip", 300 } }, "/nearClip" }, std::pair{ Json{ { "farClip", 0.25 } }, "/farClip" } })
			{
				CAPTURE(pointer);
				const auto refused = setup.Call("viewport.camera", params);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(refused.error().ToString().contains(pointer));
				CHECK(setup.Call("viewport.camera")->dump() == changed->dump());
			}
		}

		TEST_CASE("ViewportMethods: setOptions reads and sets the scene debug view and the game resolution")
		{
			Test::M10MethodFixture setup;
			EditorViewportState& state = setup.Fixture.GetEditor().GetViewportState();
			const auto initial = setup.Call("viewport.setOptions");
			REQUIRE(initial.has_value());
			CHECK((*initial)["debugView"] == "Lit");
			CHECK((*initial)["gameResolution"] == Json::array({ 0, 0 }));
			// The panel's dropdowns change the same state the method reports.
			REQUIRE(state.SetDebugView(RenderDebugView::Overdraw).has_value());
			REQUIRE(state.SetGameResolution({ 640, 360 }).has_value());
			const auto observed = setup.Call("viewport.setOptions");
			CHECK((*observed)["debugView"] == "Overdraw");
			CHECK((*observed)["gameResolution"] == Json::array({ 640, 360 }));
			const auto changed = setup.Call("viewport.setOptions", Json{ { "debugView", "normals" }, { "gameResolution", { 1920, 1080 } } });
			REQUIRE_MESSAGE(changed.has_value(), (changed ? std::string() : changed.error().ToString()));
			CHECK((*changed)["debugView"] == "Normals");
			CHECK((*changed)["gameResolution"] == Json::array({ 1920, 1080 }));
			CHECK(state.GetDebugView() == RenderDebugView::Normals);
			CHECK(state.GetGameResolution() == glm::uvec2(1920, 1080));
			REQUIRE(setup.Call("viewport.setOptions", Json{ { "gameResolution", { 0, 0 } } }).has_value());
			CHECK(state.GetGameResolution() == glm::uvec2(0));
			// A failing member leaves every member, the valid ones included, unchanged.
			for (const auto& [params, pointer] : { std::pair{ Json{ { "grid", false }, { "debugView", "Sepia" } }, "/debugView" },
					 std::pair{ Json{ { "grid", false }, { "gameResolution", { 0, 720 } } }, "/gameResolution" },
					 std::pair{ Json{ { "grid", false }, { "gameResolution", { 8193, 720 } } }, "/gameResolution" },
					 std::pair{ Json{ { "grid", false }, { "gameResolution", { 1280 } } }, "/gameResolution" } })
			{
				CAPTURE(pointer);
				const auto refused = setup.Call("viewport.setOptions", params);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(refused.error().ToString().contains(pointer));
				CHECK(state.GetOptions().Grid);
				CHECK(state.GetDebugView() == RenderDebugView::Normals);
				CHECK(state.GetGameResolution() == glm::uvec2(0));
			}
		}

		TEST_CASE("ViewportMethods: a camera inside the renderer's degenerate up band is refused")
		{
			Test::M10MethodFixture setup;
			const auto before = setup.Call("viewport.camera");
			const auto refused = setup.Call("viewport.camera", Json{ { "position", { 0.000005, 10, 0 } }, { "target", { 0, 0, 0 } } });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(refused.error().ToString().contains("/target"));
			CHECK(setup.Call("viewport.camera")->dump() == before->dump());
		}

		TEST_CASE("ViewportMethods: a refused pose is located at the pose even when projection members follow it")
		{
			Test::M10MethodFixture setup;
			const auto before = setup.Call("viewport.camera");
			REQUIRE(before.has_value());
			// The method's own orthographic example, made exactly vertical: the pose is wrong, never the size or the clip range.
			const Json vertical{ { "position", { 0, 20, 0 } }, { "target", { 0, 0, 0 } }, { "projection", "Orthographic" }, { "orthographicSize", 8 },
				{ "nearClip", 0.5 }, { "farClip", 100 } };
			const auto refused = setup.Call("viewport.camera", vertical);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(refused.error().GetLocation().JsonPointer == std::optional<std::string>("/target"));
			// Without a target the position is the member at fault; the current target stays.
			const auto byPosition = setup.Call("viewport.camera", Json{ { "position", (*before)["target"] }, { "verticalFov", 40 } });
			REQUIRE_FALSE(byPosition.has_value());
			CHECK(byPosition.error().GetLocation().JsonPointer == std::optional<std::string>("/position"));
			CHECK(setup.Call("viewport.camera")->dump() == before->dump());
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
