#include "TestsPCH.h"
#include "EditorCore/Inspector/ScriptSlotList.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/ScriptComponent.h"
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
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		Json CallScriptSlotList(Test::AutomationFixture& setup, std::string_view method, const Json& params)
		{
			Result<Json> result = setup.Call(method, params);
			INFO(std::string(method));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

		// The script handles of the entity's slots, in slot order ("null" for a slot without one).
		std::vector<std::string> ReadSlotScripts(const Scene& scene, std::string_view path)
		{
			const Result<Json> component = ComponentAccess::GetComponentJson(scene.FindEntityByPath(path), "Script");
			REQUIRE(component.has_value());
			std::vector<std::string> scripts;
			for (const Json& slot : (*component)["Scripts"])
			{
				const auto script = slot.find("Script");
				scripts.push_back(script == slot.end() || script->is_null() ? std::string("null") : JsonReader(*script).ReadString().value_or(std::string()));
			}
			return scripts;
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Docs/Decisions/0031-m20-contract.md decision 17: the inspector adds, removes and reorders script slots through one
		// edit.batch of entity.update, each selected entity changing its own list by index, its slots keeping their fields.
		TEST_CASE("ScriptSlotList: a slot change rewrites each selected entity's own list in one undo step")
		{
			Test::AutomationFixture setup("ScriptSlotList");
			EditorContext& editor = setup.GetEditor();
			std::string aim;
			std::string fire;
			for (const auto& [name, handle] : { std::pair<std::string_view, std::string*>{ "Aim", &aim }, std::pair<std::string_view, std::string*>{ "Fire", &fire } })
			{
				const Json written = CallScriptSlotList(setup, "script.write", Json{ { "path", std::format("Assets/Scripts/{}.luau", name) }, { "source", std::format("return Script.Define(\"{}\", {{Fields = {{Amount = Field.Number(1)}}}})", name) } });
				*handle = JsonReader(written["script"]["id"]).ReadString().value_or(std::string());
			}
			CallScriptSlotList(setup, "entity.create", Json{ { "name", "Turret" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", aim }, { "Fields", Json{ { "Amount", 2 } } } }, Json{ { "Script", fire } } }) } } } } } });
			CallScriptSlotList(setup, "entity.create", Json{ { "name", "Tower" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", fire } }, Json{ { "Script", aim } }, Json{ { "Script", aim }, { "ExecutionOrder", 5 } } }) } } } } } });
			Scene& scene = editor.GetScene();
			const std::vector<UUID> both{ scene.FindEntityByPath("/Turret").GetUUID(), scene.FindEntityByPath("/Tower").GetUUID() };
			const size_t steps = editor.GetHistory().GetUndoCount();

			const Result<Json> moved = MakeScriptSlotChange(scene, both, ScriptSlotChange::MoveDown, 0, SceneTarget::Edit);
			REQUIRE_MESSAGE(moved.has_value(), moved.error().ToString());
			CHECK((*moved)["label"] == Json("Move script down"));
			CallScriptSlotList(setup, "edit.batch", *moved);
			CHECK(ReadSlotScripts(scene, "/Turret") == std::vector<std::string>{ fire, aim });
			CHECK(ReadSlotScripts(scene, "/Tower") == std::vector<std::string>{ aim, fire, aim });
			CHECK(editor.GetHistory().GetUndoCount() == steps + 1);
			const Result<Json> turret = ComponentAccess::GetComponentJson(scene.FindEntityByPath("/Turret"), "Script");
			REQUIRE(turret.has_value());
			CHECK((*turret)["Scripts"][1]["Fields"] == Json{ { "Amount", 2 } });

			const Result<Json> up = MakeScriptSlotChange(scene, both, ScriptSlotChange::MoveUp, 1, SceneTarget::Edit);
			REQUIRE(up.has_value());
			CallScriptSlotList(setup, "edit.batch", *up);
			CHECK(ReadSlotScripts(scene, "/Turret") == std::vector<std::string>{ aim, fire });

			const Result<Json> removed = MakeScriptSlotChange(scene, both, ScriptSlotChange::Remove, 0, SceneTarget::Edit);
			REQUIRE(removed.has_value());
			CallScriptSlotList(setup, "edit.batch", *removed);
			CHECK(ReadSlotScripts(scene, "/Turret") == std::vector<std::string>{ fire });
			CHECK(ReadSlotScripts(scene, "/Tower") == std::vector<std::string>{ aim, aim });
			const Result<Json> tower = ComponentAccess::GetComponentJson(scene.FindEntityByPath("/Tower"), "Script");
			REQUIRE(tower.has_value());
			CHECK((*tower)["Scripts"][1]["ExecutionOrder"] == Json(5));

			const Result<Json> appended = MakeScriptSlotChange(scene, both, ScriptSlotChange::Append, 0, SceneTarget::Edit);
			REQUIRE(appended.has_value());
			CallScriptSlotList(setup, "edit.batch", *appended);
			CHECK(ReadSlotScripts(scene, "/Turret") == std::vector<std::string>{ fire, "null" });
			CHECK(ReadSlotScripts(scene, "/Tower") == std::vector<std::string>{ aim, aim, "null" });
			CHECK(editor.GetHistory().GetUndoCount() == steps + 4);
			CallScriptSlotList(setup, "edit.undo", Json{ { "steps", 4 } });
			CHECK(ReadSlotScripts(scene, "/Turret") == std::vector<std::string>{ aim, fire });
			CHECK(ReadSlotScripts(scene, "/Tower") == std::vector<std::string>{ fire, aim, aim });
		}

		// A file keeps a script field value its script rejects (an unknown name, the wrong type) until it is fixed (§11.2), but a
		// slot change writes the whole list through entity.update, which validates every value it writes: while a slot holds
		// such a value the change is refused, naming it, and nothing changes (an accepted limit of list writes).
		TEST_CASE("ScriptSlotList: a slot change is refused while a slot holds a value its script rejects")
		{
			Test::AutomationFixture setup("ScriptSlotListRejected");
			EditorContext& editor = setup.GetEditor();
			for (const std::string_view name : { std::string_view("Aim"), std::string_view("Fire") })
				CallScriptSlotList(setup, "script.write", Json{ { "path", std::format("Assets/Scripts/{}.luau", name) }, { "source", std::format("return Script.Define(\"{}\", {{Fields = {{Amount = Field.Number(1)}}}})", name) } });
			CallScriptSlotList(setup, "entity.create", Json{ { "name", "Turret" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Aim.luau" } }, Json{ { "Script", "Assets/Scripts/Fire.luau" } } }) } } } } } });
			Scene& scene = editor.GetScene();
			const Entity turret = scene.FindEntityByPath("/Turret");
			turret.Patch<ScriptComponent>([](ScriptComponent& component)
			{
				component.Scripts[1].Fields.insert_or_assign("Amount", VariantValue(Json("wrong type")));
			});
			const Result<Json> before = ComponentAccess::GetComponentJson(turret, "Script");
			REQUIRE(before.has_value());
			const size_t steps = editor.GetHistory().GetUndoCount();
			const std::vector<UUID> one{ turret.GetUUID() };

			const Result<Json> moved = MakeScriptSlotChange(scene, one, ScriptSlotChange::MoveDown, 0, SceneTarget::Edit);
			REQUIRE(moved.has_value());
			const Result<Json> refused = setup.Call("edit.batch", *moved);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(std::ranges::any_of(refused.error().GetIssues(), [](const auto& issue)
			{
				return issue.JsonPointer.ends_with("/Scripts/0/Fields/Amount");
			}));
			const Result<Json> after = ComponentAccess::GetComponentJson(scene.FindEntityByPath("/Turret"), "Script");
			REQUIRE(after.has_value());
			CHECK(*after == *before);
			CHECK(editor.GetHistory().GetUndoCount() == steps);
		}

		TEST_CASE("ScriptSlotList: a slot an entity does not have is refused before any change")
		{
			Test::AutomationFixture setup("ScriptSlotListRefused");
			EditorContext& editor = setup.GetEditor();
			CallScriptSlotList(setup, "entity.create", Json{ { "name", "One" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json::object() }) } } } } } });
			CallScriptSlotList(setup, "entity.create", Json{ { "name", "Plain" } });
			Scene& scene = editor.GetScene();
			const std::vector<UUID> one{ scene.FindEntityByPath("/One").GetUUID() };
			struct Refused
			{
				ScriptSlotChange Change = ScriptSlotChange::Remove;
				size_t Slot = 0;
			};
			for (const Refused refused : { Refused{ ScriptSlotChange::Remove, 1 }, Refused{ ScriptSlotChange::MoveUp, 0 }, Refused{ ScriptSlotChange::MoveDown, 0 } })
			{
				CAPTURE(static_cast<int>(refused.Change));
				const Result<Json> change = MakeScriptSlotChange(scene, one, refused.Change, refused.Slot, SceneTarget::Edit);
				REQUIRE_FALSE(change.has_value());
				CHECK(change.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const std::vector<UUID> plain{ scene.FindEntityByPath("/Plain").GetUUID() };
			const Result<Json> unscripted = MakeScriptSlotChange(scene, plain, ScriptSlotChange::Append, 0, SceneTarget::Edit);
			REQUIRE_FALSE(unscripted.has_value());
			CHECK(unscripted.error().GetCode() == ErrorCode::NotFound);
			const Result<Json> play = MakeScriptSlotChange(scene, one, ScriptSlotChange::Append, 0, SceneTarget::Play);
			REQUIRE(play.has_value());
			CHECK((*play)["ops"][0]["params"]["target"] == Json("Play"));
		}
	}

}
