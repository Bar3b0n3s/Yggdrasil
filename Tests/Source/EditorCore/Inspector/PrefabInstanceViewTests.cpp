#include "TestsPCH.h"
#include "EditorCore/Inspector/PrefabInstanceView.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		void CallPrefabView(Test::AutomationFixture& setup, std::string_view method, const Json& params)
		{
			const Result<Json> result = setup.Call(method, params);
			INFO(std::string(method));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
		}

		// A Room prefab nesting a live Lamp instance with a Bulb, instantiated as /Room; the room's own mesh and the bulb's
		// visibility are overridden in the scene.
		void MakeNestedRoom(Test::AutomationFixture& setup)
		{
			CallPrefabView(setup, "entity.create", Json{ { "name", "Room" }, { "components", Json{ { "MeshRenderer", Json{ { "Mesh", "engine://Meshes/Cube" } } } } } });
			CallPrefabView(setup, "entity.create", Json{ { "name", "Lamp" }, { "parent", "/Room" } });
			CallPrefabView(setup, "entity.create", Json{ { "name", "Bulb" }, { "parent", "/Room/Lamp" }, { "components", Json{ { "MeshRenderer", Json{ { "Mesh", "engine://Meshes/Sphere" } } } } } });
			CallPrefabView(setup, "prefab.create", Json{ { "entity", "/Room/Lamp" }, { "path", "Assets/Prefabs/Lamp.prefab" }, { "replaceWithInstance", true } });
			CallPrefabView(setup, "prefab.create", Json{ { "entity", "/Room" }, { "path", "Assets/Prefabs/Room.prefab" }, { "replaceWithInstance", true } });
			CallPrefabView(setup, "entity.update", Json{ { "entity", "/Room" }, { "components", Json{ { "MeshRenderer", Json{ { "CastShadows", false } } } } } });
			CallPrefabView(setup, "entity.update", Json{ { "entity", "/Room/Lamp/Bulb" }, { "components", Json{ { "MeshRenderer", Json{ { "Visible", false } } } } } });
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Docs/Decisions/0031-m20-contract.md decisions 8 and 9: a member of a nested instance belongs to the nested
		// instance, whose levels are the outermost room and the lamp itself; its overrides are those the room records within
		// the lamp, by path.
		TEST_CASE("PrefabInstanceView: a member of a nested instance lists its levels outermost first and the overrides within its instance")
		{
			Test::AutomationFixture setup("PrefabViewNested");
			EditorContext& editor = setup.GetEditor();
			MakeNestedRoom(setup);
			Scene& scene = editor.GetScene();
			const Entity room = scene.FindEntityByPath("/Room");
			const Entity lamp = scene.FindEntityByPath("/Room/Lamp");
			const Entity bulb = scene.FindEntityByPath("/Room/Lamp/Bulb");
			REQUIRE(bulb.IsValid());

			const Result<PrefabInstanceView> view = DescribePrefabInstance(editor, bulb);
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			CHECK(view->Instance == lamp.GetUUID());
			CHECK(view->Outermost == room.GetUUID());
			const std::vector<UUID> path{ lamp.GetComponent<PrefabLinkComponent>().OuterPrefabEntityID, bulb.GetComponent<PrefabLinkComponent>().PrefabEntityID };
			CHECK(view->EntityPath == path);
			REQUIRE(view->Levels.size() == 2);
			CHECK(view->Levels[0].Root == room.GetUUID());
			CHECK(view->Levels[0].Label == "Room.prefab (/Room)");
			CHECK(view->Levels[1].Root == lamp.GetUUID());
			CHECK(view->Levels[1].Label == "Lamp.prefab (/Room/Lamp)");
			REQUIRE(view->Overrides.size() == 1);
			const PrefabOverrideRow& row = view->Overrides.front();
			CHECK(row.Target == "/Room/Lamp/Bulb");
			CHECK(row.Description == "MeshRenderer.Visible");
			CHECK(row.RevertKey == Json{ { "path", Json::array({ path[0].ToString(), path[1].ToString() }) }, { "kind", "Field" }, { "component", "MeshRenderer" }, { "field", "Visible" } });

			// The room itself: one level, every override the room records.
			const Result<PrefabInstanceView> outer = DescribePrefabInstance(editor, room);
			REQUIRE_MESSAGE(outer.has_value(), outer.error().ToString());
			CHECK(outer->Instance == room.GetUUID());
			REQUIRE(outer->Levels.size() == 1);
			REQUIRE(outer->Overrides.size() == 2);
			CHECK(outer->Overrides[0].Target == "/Room");
			CHECK(outer->Overrides[0].Description == "MeshRenderer.CastShadows");
			CHECK(outer->Overrides[1].Target == "/Room/Lamp/Bulb");

			// The key selects exactly that override: reverting with it leaves the room's own.
			CallPrefabView(setup, "prefab.revert", Json{ { "instance", "/Room" }, { "overrides", Json::array({ row.RevertKey }) } });
			const auto& kept = scene.FindEntityByPath("/Room").GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(kept.size() == 1);
			CHECK(kept.front().Field == "CastShadows");

			const Result<PrefabInstanceView> plain = DescribePrefabInstance(editor, scene.CreateEntity("Plain"));
			REQUIRE_FALSE(plain.has_value());
			CHECK(plain.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// Docs/Decisions/0031-m20-contract.md decision 7: a deleted member has no entity in the scene, so its RemoveEntity row
		// names it from the outermost prefab's document, also inside a nested instance, and its key brings it back.
		TEST_CASE("PrefabInstanceView: a deleted member is named from its prefab and its key restores it")
		{
			Test::AutomationFixture setup("PrefabViewRemoved");
			EditorContext& editor = setup.GetEditor();
			MakeNestedRoom(setup);
			CallPrefabView(setup, "entity.destroy", Json{ { "entities", Json::array({ "/Room/Lamp/Bulb" }) } });
			Scene& scene = editor.GetScene();
			REQUIRE_FALSE(scene.FindEntityByPath("/Room/Lamp/Bulb").IsValid());

			for (const std::string_view selected : { "/Room", "/Room/Lamp" })
			{
				CAPTURE(selected);
				const Result<PrefabInstanceView> view = DescribePrefabInstance(editor, scene.FindEntityByPath(selected));
				REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
				const auto removed = std::find_if(view->Overrides.begin(), view->Overrides.end(), [](const PrefabOverrideRow& row)
				{
					return row.Override.Kind == PrefabOverrideKind::RemoveEntity;
				});
				REQUIRE(removed != view->Overrides.end());
				CHECK(removed->Target == "Bulb");
				CHECK(removed->Description == "Removed");
				CHECK(removed->RevertKey["kind"] == Json("RemoveEntity"));
				CHECK_FALSE(removed->RevertKey.contains("component"));
				CHECK_FALSE(removed->RevertKey.contains("field"));
			}

			const Result<PrefabInstanceView> view = DescribePrefabInstance(editor, scene.FindEntityByPath("/Room/Lamp"));
			REQUIRE(view.has_value());
			const Json key = std::find_if(view->Overrides.begin(), view->Overrides.end(), [](const PrefabOverrideRow& row)
			{
				return row.Override.Kind == PrefabOverrideKind::RemoveEntity;
			})->RevertKey;
			CallPrefabView(setup, "prefab.revert", Json{ { "instance", "/Room" }, { "overrides", Json::array({ key }) } });
			CHECK(scene.FindEntityByPath("/Room/Lamp/Bulb").IsValid());
		}

		// Docs/Decisions/0031-m20-contract.md decision 19: an override of one script slot is named by its slot and the script
		// the entity holds there; a list that changed length is one override of the whole list.
		TEST_CASE("PrefabInstanceView: an override of one script slot names its slot and script")
		{
			Test::AutomationFixture setup("PrefabViewScriptSlot");
			EditorContext& editor = setup.GetEditor();
			for (const std::string_view name : { std::string_view("Aim"), std::string_view("Fire") })
				CallPrefabView(setup, "script.write", Json{ { "path", std::format("Assets/Scripts/{}.luau", name) }, { "source", std::format("return Script.Define(\"{}\", {{Fields = {{Amount = Field.Number(1)}}}})", name) } });
			const Json slots = Json::array({ Json{ { "Script", "Assets/Scripts/Aim.luau" } }, Json{ { "Script", "Assets/Scripts/Fire.luau" } } });
			CallPrefabView(setup, "entity.create", Json{ { "name", "Turret" }, { "components", Json{ { "Script", Json{ { "Scripts", slots } } } } } });
			CallPrefabView(setup, "prefab.create", Json{ { "entity", "/Turret" }, { "path", "Assets/Prefabs/Turret.prefab" }, { "replaceWithInstance", true } });
			Json changed = slots;
			changed[1]["Fields"] = Json{ { "Amount", 4 } };
			CallPrefabView(setup, "entity.update", Json{ { "entity", "/Turret" }, { "components", Json{ { "Script", Json{ { "Scripts", changed } } } } } });
			Scene& scene = editor.GetScene();

			const Result<PrefabInstanceView> view = DescribePrefabInstance(editor, scene.FindEntityByPath("/Turret"));
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			REQUIRE(view->Overrides.size() == 1);
			const PrefabOverrideRow& row = view->Overrides.front();
			CHECK(row.Override.Field == "Scripts/1/Fields");
			CHECK(row.Target == "/Turret");
			CHECK(row.Description.contains("slot 1"));
			CHECK(row.Description.contains("Fire.luau"));
			CHECK(row.Description.contains("Fields"));
			CHECK(row.RevertKey["field"] == Json("Scripts/1/Fields"));
			// Its key selects that override for prefab.revert (a dry run, which changes nothing).
			const Result<Json> reverted = setup.Call("prefab.revert", Json{ { "instance", "/Turret" }, { "overrides", Json::array({ row.RevertKey }) }, { "dryRun", true } });
			REQUIRE_MESSAGE(reverted.has_value(), reverted.error().ToString());
			CHECK((*reverted)["removedOverrides"] == Json(1));

			// A third slot changes the list's length: one override of the whole list, named as any field's.
			changed.push_back(Json::object());
			CallPrefabView(setup, "entity.update", Json{ { "entity", "/Turret" }, { "components", Json{ { "Script", Json{ { "Scripts", changed } } } } } });
			const Result<PrefabInstanceView> whole = DescribePrefabInstance(editor, scene.FindEntityByPath("/Turret"));
			REQUIRE(whole.has_value());
			REQUIRE(whole->Overrides.size() == 1);
			CHECK(whole->Overrides.front().Description == "Script.Scripts");
		}

		// A malformed file can give an override an empty path, which names no entity: its row names none either, never the
		// outermost root, and the row's key keeps the empty path.
		TEST_CASE("PrefabInstanceView: an override with an empty path names no entity")
		{
			Test::AutomationFixture setup("PrefabViewEmptyPath");
			EditorContext& editor = setup.GetEditor();
			MakeNestedRoom(setup);
			Scene& scene = editor.GetScene();
			Entity room = scene.FindEntityByPath("/Room");
			room.Patch<PrefabInstanceComponent>([](PrefabInstanceComponent& instance)
			{
				instance.Overrides.insert(instance.Overrides.begin(), PrefabOverride{ .Path = {}, .Kind = PrefabOverrideKind::Field, .Component = "MeshRenderer", .Field = "Visible", .Value = {} });
			});

			const Result<PrefabInstanceView> view = DescribePrefabInstance(editor, room);
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			REQUIRE(view->Overrides.size() == 3);
			const PrefabOverrideRow& row = view->Overrides.front();
			CHECK(row.Override.Path.empty());
			CHECK_FALSE(row.Target.empty());
			CHECK(row.Target != "/Room");
			CHECK(row.RevertKey["path"] == Json::array());

			// A member of the nested lamp lists only the overrides within the lamp.
			const Result<PrefabInstanceView> nested = DescribePrefabInstance(editor, scene.FindEntityByPath("/Room/Lamp/Bulb"));
			REQUIRE(nested.has_value());
			CHECK(nested->Overrides.size() == 1);
		}
	}

}
