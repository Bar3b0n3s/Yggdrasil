#include "TestsPCH.h"

#include "EditorCore/Automation/EditorMethodContext.h"

#include "EditorCore/Commands/SceneEdit.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Support/AutomationTestClient.h"

namespace Engine {

	// A context for session.info, the simplest method, over the fixture's editor and server.
	static Scope<EditorMethodContext> MakeEditorMethodContext(Test::AutomationFixture& setup)
	{
		const MethodRegistry& methods = setup.GetClient().GetServer().GetMethods();
		const MethodDescriptor* info = methods.Find("session.info");
		REQUIRE(info != nullptr);
		return CreateScope<EditorMethodContext>(setup.GetEditor(), setup.GetClient().GetServer(),
			MethodRequest{ .Info = { .Client = 1, .ClientName = "test", .Id = Json(1), .Method = "session.info", .TranscriptLine = std::nullopt },
				.Options = {},
				.Method = info,
				.Params = Json::object(),
				.Registry = &methods,
				.PhaseMarker = nullptr,
				.NestingDepth = 0 });
	}

	// A Script component value of one slot (Script version 2, Docs/Decisions/0031-m20-contract.md decision 17).
	static Json ScriptSlots(Json slot)
	{
		return Json{ { "Scripts", Json::array({ std::move(slot) }) } };
	}

	static void CreateContextScriptEntity(Test::AutomationFixture& setup)
	{
		const auto written = setup.Call("script.write", Json{ { "path", "Assets/Fields.luau" }, { "source", R"(
local Fields = { Fields = {
	Amount = Field.Number(1, {Min = 0, Max = 5}), Mode = Field.Enum({"Idle", "Moving"}),
	References = Field.Array(Field.Array(Field.Asset("Script"))),
} }
return Script.Define("Fields", Fields)
)" } });
		REQUIRE_MESSAGE(written.has_value(), (written ? std::string() : written.error().ToString()));
		const auto created = setup.Call("entity.create", Json{ { "name", "Owner" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Script", "Assets/Fields.luau" }, { "Fields", Json{ { "Amount", 1 } } } }) } } } });
		REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorMethodContext: fields-only script patches use the existing owner for paths enums bounds and undo")
		{
			Test::AutomationFixture setup("ContextScriptFields");
			CreateContextScriptEntity(setup);
			const auto before = setup.Call("entity.get", Json{ { "entity", "/Owner" } });
			REQUIRE(before.has_value());
			const Json script = (*before)["entity"]["components"]["Script"]["Scripts"][0]["Script"];
			const Json fields{ { "Amount", 3 }, { "Mode", "mOvInG" }, { "References", Json::array({ Json::array({ "Assets/Fields.luau" }) }) } };
			const auto changed = setup.Call("entity.update", Json{ { "entity", "/Owner" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", fields } }) } } } });
			REQUIRE_MESSAGE(changed.has_value(), (changed ? std::string() : changed.error().ToString()));
			const Json component = (*changed)["entity"]["components"]["Script"];
			const Json& slot = component["Scripts"][0];
			CHECK(slot["Script"] == script);
			CHECK(slot["Fields"]["Amount"] == Json(3));
			CHECK(slot["Fields"]["Mode"] == Json("Moving"));
			CHECK(slot["Fields"]["References"] == Json::array({ Json::array({ script }) }));
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			const auto undone = setup.Call("entity.get", Json{ { "entity", "/Owner" } });
			REQUIRE(undone.has_value());
			CHECK((*undone)["entity"]["components"]["Script"] == (*before)["entity"]["components"]["Script"]);
			REQUIRE(setup.Call("edit.redo", Json::object()).has_value());

			const auto rejected = setup.Call("entity.update", Json{ { "entity", "/Owner" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", Json{ { "Amount", 6 } } } }) } } } });
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(rejected.error().GetIssues().empty());
			CHECK(rejected.error().GetIssues()[0].JsonPointer == "/components/Script/Scripts/0/Fields/Amount");
			CHECK_FALSE(rejected.error().ToString().contains("no script is assigned"));
			const auto malformed = setup.Call("entity.update", Json{ { "entity", "/Owner" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", Json::array({ 1 }) } }) } } } });
			REQUIRE_FALSE(malformed.has_value());
			CHECK(malformed.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(malformed.error().GetIssues().empty());
			CHECK(malformed.error().GetIssues()[0].JsonPointer == "/components/Script/Scripts/0/Fields");
			const auto after = setup.Call("entity.get", Json{ { "entity", "/Owner" } });
			REQUIRE(after.has_value());
			CHECK((*after)["entity"]["components"]["Script"] == component);
		}

		TEST_CASE("EditorMethodContext: fields-only script patches borrow from the addressed play scene")
		{
			Test::AutomationFixture setup("ContextPlayScriptFields");
			CreateContextScriptEntity(setup);
			REQUIRE(setup.Call("play.start", Json{ { "paused", true } }).has_value());
			REQUIRE(setup.Call("entity.update", Json{ { "entity", "/Owner" }, { "removeComponents", Json::array({ "Script" }) } }).has_value());
			const auto changed = setup.Call("entity.update", Json{ { "entity", "/Owner" }, { "target", "play" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", Json{ { "Amount", 4 } } } }) } } } });
			REQUIRE_MESSAGE(changed.has_value(), (changed ? std::string() : changed.error().ToString()));
			CHECK((*changed)["entity"]["components"]["Script"]["Scripts"][0]["Fields"]["Amount"] == Json(4));
			CHECK((*changed)["undoIndex"] == Json(0));
			const auto edited = setup.Call("entity.get", Json{ { "entity", "/Owner" }, { "target", "edit" } });
			REQUIRE(edited.has_value());
			CHECK_FALSE((*edited)["entity"]["components"].contains("Script"));
			REQUIRE(setup.Call("play.stop", Json::object()).has_value());
		}

		TEST_CASE("EditorMethodContext: script owner completion never replaces an explicit handle or a removed component")
		{
			Test::AutomationFixture setup("ContextScriptOwnerPresence");
			CreateContextScriptEntity(setup);
			const Json fields{ { "Amount", 2 } };
			const Json clearedComponent = ScriptSlots(Json{ { "Script", nullptr }, { "Fields", fields } });
			const Json clearedParams{ { "entity", "/Owner" }, { "components", Json{ { "Script", clearedComponent } } } };
			const auto cleared = setup.Call("entity.update", clearedParams);
			REQUIRE_FALSE(cleared.has_value());
			CHECK(cleared.error().GetCode() == ErrorCode::InvalidArgument);
			const Json replacedComponent = ScriptSlots(Json{ { "Fields", fields } });
			const Json replacedParams{ { "entity", "/Owner" }, { "removeComponents", Json::array({ "Script" }) },
				{ "components", Json{ { "Script", replacedComponent } } } };
			const auto replaced = setup.Call("entity.update", replacedParams);
			REQUIRE_FALSE(replaced.has_value());
			CHECK(replaced.error().GetCode() == ErrorCode::InvalidArgument);
			const auto unchanged = setup.Call("entity.get", Json{ { "entity", "/Owner" } });
			REQUIRE(unchanged.has_value());
			CHECK((*unchanged)["entity"]["components"]["Script"]["Scripts"][0]["Fields"]["Amount"] == Json(1));
		}

		// Docs/Decisions/0031-m20-contract.md decision 17: a Fields-only slot borrows the Script handle of the slot at its own
		// index, so each slot's values resolve and validate against that slot's script, for entity.update and for
		// prefab.instantiate's per-instance values.
		TEST_CASE("EditorMethodContext: a fields-only script slot borrows the handle of the slot at its index")
		{
			Test::AutomationFixture setup("ContextScriptSlotOwners");
			REQUIRE(setup.Call("script.write", Json{ { "path", "Assets/Aim.luau" }, { "source", "return Script.Define(\"Aim\", {Fields = {Count = Field.Number(1, {Min = 0, Max = 5})}})" } })
					.has_value());
			REQUIRE(setup.Call("script.write", Json{ { "path", "Assets/Fire.luau" }, { "source", "return Script.Define(\"Fire\", {Fields = {Mode = Field.Enum({\"Single\", \"Burst\"})}})" } })
					.has_value());
			const Json slots{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Aim.luau" } }, Json{ { "Script", "Assets/Fire.luau" } } }) } };
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Turret" }, { "components", Json{ { "Script", slots } } } }).has_value());
			const auto before = setup.Call("entity.get", Json{ { "entity", "/Turret" } });
			REQUIRE(before.has_value());
			const Json& attached = (*before)["entity"]["components"]["Script"]["Scripts"];

			const Json fieldsOnly{ { "Scripts", Json::array({ Json{ { "Fields", Json{ { "Count", 3 } } } }, Json{ { "Fields", Json{ { "Mode", "bUrSt" } } } } }) } };
			const auto changed = setup.Call("entity.update", Json{ { "entity", "/Turret" }, { "components", Json{ { "Script", fieldsOnly } } } });
			REQUIRE_MESSAGE(changed.has_value(), (changed ? std::string() : changed.error().ToString()));
			const Json& written = (*changed)["entity"]["components"]["Script"]["Scripts"];
			CHECK(written[0]["Script"] == attached[0]["Script"]);
			CHECK(written[1]["Script"] == attached[1]["Script"]);
			CHECK(written[0]["Fields"]["Count"] == Json(3));
			CHECK(written[1]["Fields"]["Mode"] == Json("Burst"));

			// Slot 1's value checked against slot 0's script: Fire declares no Count.
			const Json swapped{ { "Scripts", Json::array({ Json{ { "Fields", Json{ { "Count", 2 } } } }, Json{ { "Fields", Json{ { "Count", 2 } } } } }) } };
			const auto rejected = setup.Call("entity.update", Json{ { "entity", "/Turret" }, { "components", Json{ { "Script", swapped } } } });
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(rejected.error().GetIssues().empty());
			CHECK(rejected.error().GetIssues()[0].JsonPointer.starts_with("/components/Script/Scripts/1/Fields"));

			// A slot beyond the entity's list has no handle to borrow.
			const Json beyond{ { "Scripts", Json::array({ Json::object(), Json::object(), Json{ { "Fields", Json{ { "Count", 2 } } } } }) } };
			const auto unowned = setup.Call("entity.update", Json{ { "entity", "/Turret" }, { "components", Json{ { "Script", beyond } } } });
			REQUIRE_FALSE(unowned.has_value());
			CHECK(unowned.error().GetCode() == ErrorCode::InvalidArgument);

			// prefab.instantiate's values borrow from the prefab root's slot at the same index.
			REQUIRE(setup.Call("prefab.create", Json{ { "entity", "/Turret" }, { "path", "Assets/Turret.prefab" } }).has_value());
			const Json instanceValues{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Aim.luau" } }, Json{ { "Fields", Json{ { "Mode", "single" } } } } }) } };
			const auto instance = setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Turret.prefab" }, { "name", "Copy" }, { "components", Json{ { "Script", instanceValues } } } });
			REQUIRE_MESSAGE(instance.has_value(), (instance ? std::string() : instance.error().ToString()));
			const auto copy = setup.Call("entity.get", Json{ { "entity", "/Copy" } });
			REQUIRE(copy.has_value());
			const Json& copied = (*copy)["entity"]["components"]["Script"]["Scripts"];
			CHECK(copied[1]["Script"] == attached[1]["Script"]);
			CHECK(copied[1]["Fields"]["Mode"] == Json("Single"));
		}

		TEST_CASE("EditorMethodContext: entity references resolve by id, unique prefix and path")
		{
			Test::AutomationFixture setup("ContextEntities");
			Scene& scene = setup.GetEditor().GetScene();
			UUID gameId;
			{
				SceneEdit edit(setup.GetEditor(), "Create");
				const Entity game = scene.CreateEntity("Game");
				static_cast<void>(scene.CreateEntity("Board", game));
				static_cast<void>(scene.CreateEntity("Piece", game));
				static_cast<void>(scene.CreateEntity("Piece", game));
				gameId = game.GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			Scope<EditorMethodContext> context = MakeEditorMethodContext(setup);
			const std::string id = gameId.ToString();

			std::string upper = id;
			std::transform(upper.begin(), upper.end(), upper.begin(), [](char character)
			{
				return static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
			});
			for (const std::string& reference : { id, upper, id.substr(0, 6) })
			{
				INFO(reference);
				const Result<Entity> resolved = context->ResolveEntity(scene, reference, "/entity");
				REQUIRE_MESSAGE(resolved.has_value(), resolved.error().ToString());
				CHECK(resolved->GetUUID() == gameId);
			}
			const Result<Entity> board = context->ResolveEntity(scene, "/Game/Board", "/entity");
			REQUIRE_MESSAGE(board.has_value(), board.error().ToString());
			CHECK(board->GetName() == "Board");
			CHECK(context->ResolveEntity(scene, "/Game/Piece[1]", "/entity").has_value());

			const Result<Entity> ambiguous = context->ResolveEntity(scene, "/Game/Piece", "/entities/1");
			REQUIRE_FALSE(ambiguous.has_value());
			CHECK(ambiguous.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(ambiguous.error().GetIssues().size() == 2);
			CHECK(ambiguous.error().ToString().contains("/entities/1"));
			CHECK(context->ResolveEntity(scene, "/Gmae", "/entity").error().GetCode() == ErrorCode::NotFound);
			CHECK(context->ResolveEntity(scene, "00000000000000aa", "/entity").error().GetCode() == ErrorCode::NotFound);
			CHECK(context->ResolveEntity(scene, "abc", "/entity").error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(context->ResolveEntity(scene, "Game", "/entity").error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("EditorMethodContext: project paths are confined to project://")
		{
			Test::AutomationFixture setup("ContextPaths", false);
			Scope<EditorMethodContext> context = MakeEditorMethodContext(setup);
			const Result<VfsPath> relative = context->ResolveProjectPath("Assets/Scenes/Main.scene", "/path", ".scene");
			REQUIRE_MESSAGE(relative.has_value(), relative.error().ToString());
			CHECK(relative->ToString() == "project://Assets/Scenes/Main.scene");
			CHECK(context->ResolveProjectPath("project://Assets/Scenes/Main.scene", "/path", ".scene").has_value());
			for (const std::string_view bad : { "", "../Main.scene", "/abs/Main.scene", "C:/Main.scene", "user://Main.scene",
					 "Assets/Scenes/Main.prefab", "Assets/CON.scene" })
			{
				INFO(std::string(bad));
				const Result<VfsPath> path = context->ResolveProjectPath(bad, "/path", ".scene");
				REQUIRE_FALSE(path.has_value());
				CHECK(path.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		TEST_CASE("EditorMethodContext: the target scene is the edit scene until play sessions exist")
		{
			Test::AutomationFixture setup("ContextTarget");
			Scope<EditorMethodContext> context = MakeEditorMethodContext(setup);
			const Result<Scene*> implicitTarget = context->ResolveTargetScene(SceneTarget::Edit, false, false);
			REQUIRE_MESSAGE(implicitTarget.has_value(), implicitTarget.error().ToString());
			CHECK(*implicitTarget == &setup.GetEditor().GetScene());
			const Result<Scene*> explicitTarget = context->ResolveTargetScene(SceneTarget::Edit, true, true);
			REQUIRE_MESSAGE(explicitTarget.has_value(), explicitTarget.error().ToString());
			CHECK(*explicitTarget == &setup.GetEditor().GetScene());
			CHECK(context->ResolveTargetScene(SceneTarget::Play, true, false).error().GetCode() == ErrorCode::InvalidState);
			setup.GetEditor().CloseScene();
			CHECK(context->ResolveTargetScene(SceneTarget::Edit, false, false).error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EditorMethodContext: entity summaries carry the id, name and path")
		{
			Test::AutomationFixture setup("ContextSummary");
			Entity entity;
			{
				SceneEdit edit(setup.GetEditor(), "Create");
				entity = setup.GetEditor().GetScene().CreateEntity("A/B");
				REQUIRE(edit.Commit().has_value());
			}
			Scope<EditorMethodContext> context = MakeEditorMethodContext(setup);
			const EntitySummary summary = context->MakeEntitySummary(entity);
			CHECK(summary.Id == entity.GetUUID().ToString());
			CHECK(summary.Name == "A/B");
			CHECK(summary.Path == "/A\\/B");
			CHECK(&context->GetEditor() == &setup.GetEditor());
			CHECK(context->GetHostKey() == TypeKeyOf<EditorMethodContext>());
		}
	}

}
