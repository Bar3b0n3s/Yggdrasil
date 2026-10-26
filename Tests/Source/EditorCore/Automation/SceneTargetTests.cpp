#include "TestsPCH.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The scene target of M20 (E-SCENES; Docs/Decisions/0031-m20-contract.md decision 16): the reserved request member "scene"
// that names the open scene a scene-addressing method addresses, and the inventory that decides, for every method, how it
// relates to the open scenes.

namespace Engine {

	namespace {

		// How a method relates to the editor's open scenes (decision 16). Every method the editor registers has exactly one
		// entry, so a method added later (M21 to M26 included) fails the inventory until its author decides.
		enum class SceneRelation : uint8_t
		{
			Target,      // addresses one open scene: takes the optional scene target, absent meaning the active scene
			ActiveScene, // works on the active scene only (what the editor shows and plays), or replaces it
			AllScenes,   // acts on every open scene (saves, refusals for unsaved changes, prefab updates, recovery)
			None         // addresses no open scene: assets, project settings, logs, the play session, sessions
		};

		const std::map<std::string, SceneRelation, std::less<>>& GetSceneRelations()
		{
			static const std::map<std::string, SceneRelation, std::less<>> Relations = {
				// The open scene's content, history and selection, and the reads and renders of one scene.
				{ "edit.batch", SceneRelation::Target }, // its ops inherit the batch's target and carry none of their own
				{ "edit.getSelection", SceneRelation::Target },
				{ "edit.history", SceneRelation::Target },
				{ "edit.redo", SceneRelation::Target },
				{ "edit.select", SceneRelation::Target },
				{ "edit.undo", SceneRelation::Target },
				{ "entity.bounds", SceneRelation::Target },
				{ "entity.create", SceneRelation::Target },
				{ "entity.destroy", SceneRelation::Target },
				{ "entity.duplicate", SceneRelation::Target },
				{ "entity.get", SceneRelation::Target },
				{ "entity.reparent", SceneRelation::Target },
				{ "entity.update", SceneRelation::Target },
				// M19's native component reads and writes (Docs/Decisions/0031-m20-contract.md, "Merge with M19").
				{ "native.getComponents", SceneRelation::Target },
				{ "native.setComponents", SceneRelation::Target },
				{ "prefab.apply", SceneRelation::Target }, // the other open scenes' instances follow in their own histories
				{ "prefab.create", SceneRelation::Target },
				{ "prefab.instantiate", SceneRelation::Target },
				{ "prefab.revert", SceneRelation::Target },
				{ "prefab.unpack", SceneRelation::Target },
				{ "project.validate", SceneRelation::Target }, // scope "scene"; scope "project" reads every open scene's state
				{ "scene.activate", SceneRelation::Target },   // its subject, required
				{ "scene.close", SceneRelation::Target },
				{ "scene.diff", SceneRelation::Target },
				{ "scene.get", SceneRelation::Target },
				{ "scene.query", SceneRelation::Target },
				{ "scene.raycast", SceneRelation::Target },
				{ "scene.save", SceneRelation::Target },
				{ "scene.tree", SceneRelation::Target },
				{ "script.errors", SceneRelation::Target }, // entity paths are read from the addressed scene
				{ "script.eval", SceneRelation::Target },   // context "edit" evaluates over the addressed scene
				{ "viewport.screenshot", SceneRelation::Target },
				// The active scene only.
				{ "editor.screenshot", SceneRelation::ActiveScene }, // the editor window shows the active tab
				{ "editor.state", SceneRelation::ActiveScene },
				{ "play.start", SceneRelation::ActiveScene }, // its own "scene" member names a scene file to play instead
				{ "project.info", SceneRelation::ActiveScene },
				{ "scene.new", SceneRelation::ActiveScene },  // replaces the active scene, or adds one with keepOpen
				{ "scene.open", SceneRelation::ActiveScene }, // the same
				{ "session.info", SceneRelation::ActiveScene },
				{ "viewport.frame", SceneRelation::ActiveScene }, // the scene view shows the active scene
				{ "viewport.pick", SceneRelation::ActiveScene },
				// Every open scene.
				{ "asset.reimport", SceneRelation::AllScenes },          // a changed prefab's instances in every open scene
				{ "asset.setImportSettings", SceneRelation::AllScenes }, // the same
				{ "native.load", SceneRelation::AllScenes },             // migrates every open scene's native components (M19)
				{ "project.export", SceneRelation::AllScenes },          // warns about every unsaved open scene
				{ "project.open", SceneRelation::AllScenes },            // recover opens every recovered scene
				{ "project.refreshAssets", SceneRelation::AllScenes },   // SceneChangedOnDisk for every open scene
				{ "project.save", SceneRelation::AllScenes },            // writes every dirty open scene
				{ "project.upgrade", SceneRelation::AllScenes },         // refuses unsaved scenes, reloads the rewritten ones
				{ "scene.list", SceneRelation::AllScenes },
				{ "session.shutdown", SceneRelation::AllScenes }, // save writes every dirty open scene
				{ "test.run", SceneRelation::AllScenes },         // refuses an unsaved open scene a selected suite plays
				// No open scene.
				{ "asset.create", SceneRelation::None },
				{ "asset.delete", SceneRelation::None },
				{ "asset.getImportSettings", SceneRelation::None },
				{ "asset.getProperties", SceneRelation::None },
				{ "asset.import", SceneRelation::None },
				{ "asset.info", SceneRelation::None },
				{ "asset.list", SceneRelation::None },
				{ "asset.move", SceneRelation::None },
				{ "asset.setProperties", SceneRelation::None },
				{ "audio.stats", SceneRelation::None },
				{ "component.list", SceneRelation::None },
				{ "component.schema", SceneRelation::None },
				{ "docs.get", SceneRelation::None },
				{ "events.read", SceneRelation::None },
				{ "input.inject", SceneRelation::None },
				{ "input.readReplay", SceneRelation::None }, // M19: a replay file's events
				{ "input.record", SceneRelation::None },
				{ "input.replay", SceneRelation::None },
				{ "log.read", SceneRelation::None },
				{ "native.build", SceneRelation::None }, // M19: the module's sources and builds
				{ "native.read", SceneRelation::None },
				{ "native.status", SceneRelation::None },
				{ "native.write", SceneRelation::None },
				{ "physics.bodyInfo", SceneRelation::None }, // the play session's scene
				{ "play.pause", SceneRelation::None },
				{ "play.resume", SceneRelation::None },
				{ "play.setTimeScale", SceneRelation::None },
				{ "play.state", SceneRelation::None },
				{ "play.step", SceneRelation::None },
				{ "play.stop", SceneRelation::None },
				{ "play.waitFor", SceneRelation::None },
				{ "project.create", SceneRelation::None },
				{ "project.getSettings", SceneRelation::None },
				{ "project.setSettings", SceneRelation::None }, // recorded in the active scene's history, as before M20
				{ "rpc.discover", SceneRelation::None },
				{ "script.check", SceneRelation::None },
				{ "script.create", SceneRelation::None },
				{ "script.fields", SceneRelation::None },
				{ "script.read", SceneRelation::None },
				{ "script.write", SceneRelation::None },
				{ "session.hello", SceneRelation::None },
				{ "stats.get", SceneRelation::None },
				{ "test.list", SceneRelation::None },
				{ "viewport.camera", SceneRelation::None },
				{ "viewport.setOptions", SceneRelation::None },
			};
			return Relations;
		}

		std::string SceneTargetName(const Json& entity)
		{
			return JsonReader(entity["name"]).ReadString().value_or(std::string());
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Decision 16: "scene" is reserved only for the scene-addressing methods; any other refuses it as an unknown member,
		// and an op of edit.batch cannot carry one (every op runs in the batch's scene), both before anything runs.
		TEST_CASE("SceneTarget: a method without the scene target and an op of a batch refuse scene")
		{
			Test::AutomationFixture setup("SceneTargetRefused");
			const Result<Json> listed = setup.Call("asset.list", Json{ { "scene", "Assets/Scenes/Main.scene" } });
			REQUIRE_FALSE(listed.has_value());
			CHECK(listed.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(listed.error().GetIssues().empty());
			CHECK(listed.error().GetIssues().front().JsonPointer == "/scene");

			const uint64_t revision = setup.GetEditor().GetRevision();
			Json batch{ { "label", "Stray" } };
			batch["ops"] = Json::array({ Json{ { "method", "entity.create" }, { "params", Json{ { "name", "First" } } } },
				Json{ { "method", "entity.create" }, { "params", Json{ { "name", "Stray" }, { "scene", "Assets/Scenes/Main.scene" } } } } });
			const Json refused = setup.Request("edit.batch", batch);
			REQUIRE(refused.contains("error"));
			CHECK(refused["error"]["data"]["failedOp"] == Json(1));
			CHECK(refused["error"]["data"]["issues"][0]["pointer"] == Json("/ops/1/params/scene"));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
			CHECK(setup.GetEditor().GetRevision() == revision);
		}

		// Decision 16 (Roadmap M20 "an optional scene target on every scene-addressing method, with an inventory test"):
		// every registered method is classified, and exactly the scene-addressing ones accept the reserved "scene" member,
		// which their schemas list and PrepareParams moves into the request options. play.start keeps its own "scene" (a
		// scene file to play).
		TEST_CASE("SceneTarget: every scene-addressing method takes the optional scene target")
		{
			Test::EditorTestFixture fixture("SceneTargetInventory");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterEditorMethods(methods, {});
			methods.Freeze();
			const auto& relations = GetSceneRelations();
			std::set<std::string, std::less<>> registered;
			for (const MethodDescriptor* method : methods.GetMethods())
				registered.insert(method->Specification.Name);
			std::set<std::string, std::less<>> classified;
			for (const auto& [name, relation] : relations)
				classified.insert(name);
			CHECK(registered == classified);

			// The params of the inventory's calls: every member at its default (the required ones included), plus "scene".
			const auto withScene = [](const MethodDescriptor& method, Json scene)
			{
				Json params = method.Params->MakeDefaultJson();
				params["scene"] = std::move(scene);
				return params;
			};
			// The methods that declare a "scene" member of their own (a scene file), which keeps its meaning.
			const std::set<std::string, std::less<>> ownScene = { "input.record", "play.start" };
			for (const MethodDescriptor* method : methods.GetMethods())
			{
				const std::string& name = method->Specification.Name;
				INFO(name);
				const auto relation = relations.find(name);
				if (relation == relations.end())
					continue;
				const bool takesTarget = relation->second == SceneRelation::Target;
				CHECK(method->Specification.SupportsSceneTarget == takesTarget);
				// scene.activate alone requires its target (its subject), and its schemas say so.
				const bool requiresTarget = name == "scene.activate";
				CHECK(method->Specification.RequiresSceneTarget == requiresTarget);
				for (const SchemaStyle style : { SchemaStyle::Full, SchemaStyle::Compact })
				{
					const Json schema = methods.GetParamsSchema(*method, style);
					const bool listsScene = schema.contains("properties") && schema["properties"].contains("scene");
					CHECK(listsScene == (takesTarget || ownScene.contains(name)));
					const Json required = schema.contains("required") ? schema["required"] : Json::array();
					const bool requiresScene = std::find(required.begin(), required.end(), Json("scene")) != required.end();
					CHECK(requiresScene == requiresTarget);
				}
				if (requiresTarget)
				{
					// InvalidParams naming the missing member, as for a missing RequiredParams member.
					const Result<PreparedParams> missing = methods.PrepareParams(*method, method->Params->MakeDefaultJson());
					REQUIRE_FALSE(missing.has_value());
					CHECK(missing.error().GetCode() == ErrorCode::InvalidArgument);
					REQUIRE_FALSE(missing.error().GetIssues().empty());
					CHECK(missing.error().GetIssues().front().JsonPointer == "/scene");
				}
				const Result<PreparedParams> prepared = methods.PrepareParams(*method, withScene(*method, "Assets/Scenes/Main.scene"));
				REQUIRE_MESSAGE(prepared.has_value(), prepared.error().ToString());
				if (takesTarget)
				{
					// A path or an id moves into the request options; anything else is refused.
					CHECK_FALSE(prepared->Params.contains("scene"));
					REQUIRE(prepared->Options.Scene.has_value());
					CHECK(prepared->Options.Scene->Path == "Assets/Scenes/Main.scene");
					CHECK(prepared->Options.Scene->Id == 0);
					const Result<PreparedParams> byId = methods.PrepareParams(*method, withScene(*method, 3));
					REQUIRE(byId.has_value());
					REQUIRE(byId->Options.Scene.has_value());
					CHECK(byId->Options.Scene->Id == 3);
					CHECK(byId->Options.Scene->Path.empty());
					// The id is a uint32 (OpenSceneId), so 2^32 is out of range like 0 and negative ids.
					for (const Json& invalid : { Json(true), Json(-1), Json(0), Json(uint64_t{ 1 } << 32), Json(""), Json::object() })
					{
						CAPTURE(invalid.dump());
						CHECK_FALSE(methods.PrepareParams(*method, withScene(*method, invalid)).has_value());
					}
				}
				else
				{
					// Left in the params: the method's own member, or an unknown one its strict read refuses.
					CHECK(prepared->Params.contains("scene"));
					CHECK_FALSE(prepared->Options.Scene.has_value());
				}
			}
		}

		// Decision 16 (Roadmap M20 acceptance "transcripts without scene targets behave as before"): without "scene" a
		// request addresses the active scene; with one it addresses that open scene, its revision, "_meta", history and
		// ifRevision included, while the active scene stays. The play scene belongs to the scene the session plays.
		TEST_CASE("SceneTarget: a request addresses the active scene without a target and the named open scene with one")
		{
			Test::AutomationFixture setup("SceneTargetRequests");
			const Result<Json> second = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			EditorContext& editor = setup.GetEditor();
			const OpenSceneId mainId = editor.FindOpenScene(*VfsPath::Parse("project://Assets/Scenes/Main.scene"));
			REQUIRE(mainId != NoOpenScene);

			REQUIRE(setup.Call("entity.create", Json{ { "name", "InSecond" } }).has_value());
			const Json inMain{ { "scene", "Assets/Scenes/Main.scene" } };
			Json create = inMain;
			create["name"] = "InMain";
			const Result<Json> created = setup.Call("entity.create", create);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			CHECK(editor.GetScene().GetName() == "Second");
			const Result<OpenSceneInfo> mainInfo = editor.DescribeOpenScene(mainId);
			REQUIRE(mainInfo.has_value());
			CHECK(mainInfo->EntityCount == 1);
			CHECK((*created)["_meta"]["revision"] == Json(mainInfo->Revision));
			CHECK((*created)["_meta"]["dirty"] == Json(true));

			// Entity references resolve in the addressed scene only.
			CHECK(setup.Call("entity.get", Json{ { "entity", "/InMain" } }).error().GetCode() == ErrorCode::NotFound);
			Json get = inMain;
			get["entity"] = "/InMain";
			CHECK(setup.Call("entity.get", get).has_value());

			// A dry run runs in a copy of the targeted scene, which the scope addresses before the sandbox opens: its parent
			// resolves there, nothing changes, and "_meta" reports the target's real state.
			Json planned = inMain;
			planned["name"] = "Planned";
			planned["parent"] = "/InMain";
			planned["dryRun"] = true;
			const Result<Json> dryRun = setup.Call("entity.create", planned);
			REQUIRE_MESSAGE(dryRun.has_value(), dryRun.error().ToString());
			CHECK((*dryRun)["dryRun"] == Json(true));
			CHECK((*dryRun)["_meta"]["revision"] == Json(mainInfo->Revision));
			CHECK(editor.DescribeOpenScene(mainId)->EntityCount == 1);
			CHECK(editor.DescribeOpenScene(mainId)->Revision == mainInfo->Revision);
			CHECK_FALSE(editor.GetScene().FindEntityByPath("/Planned").IsValid());

			// ifRevision is checked against the addressed scene's revision, which never equals another scene's.
			Json stale = inMain;
			stale["ifRevision"] = editor.GetRevision();
			CHECK(setup.Call("scene.tree", stale).error().GetCode() == ErrorCode::Conflict);

			// Undo in the targeted scene only.
			REQUIRE(setup.Call("edit.undo", inMain).has_value());
			CHECK(editor.DescribeOpenScene(mainId)->EntityCount == 0);
			CHECK(editor.GetScene().FindEntityByPath("/InSecond").IsValid());

			// Unknown and malformed targets, and a target on a method that takes none.
			CHECK(setup.Call("scene.tree", Json{ { "scene", "Assets/Scenes/Closed.scene" } }).error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.Call("scene.tree", Json{ { "scene", 999999 } }).error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.Call("scene.tree", Json{ { "scene", "Assets/Scenes/Main.txt" } }).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Call("asset.list", inMain).error().GetCode() == ErrorCode::InvalidArgument);

			// While Second plays, "play" with Main as the target is refused; with Second it reads the play scene.
			REQUIRE(setup.Call("play.start", Json::object()).has_value());
			Json playMain = inMain;
			playMain["target"] = "play";
			CHECK(setup.Call("scene.tree", playMain).error().GetCode() == ErrorCode::InvalidArgument);
			const Result<Json> playSecond = setup.Call("scene.tree", Json{ { "scene", "Assets/Scenes/Second.scene" }, { "target", "play" } });
			CHECK(playSecond.has_value());

			// Without a scene target the play rules reach the session whatever tab is active: with Main active, a read
			// without target and "play" both read Second's play copy, while "_meta" reports the addressed scene, Main.
			REQUIRE(setup.Call("scene.activate", inMain).has_value());
			CHECK(editor.GetPlay().IsPlaying());
			for (const Json& read : { Json{ { "format", "json" } }, Json{ { "format", "json" }, { "target", "play" } } })
			{
				CAPTURE(read.dump());
				const Result<Json> tree = setup.Call("scene.tree", read);
				REQUIRE_MESSAGE(tree.has_value(), tree.error().ToString());
				REQUIRE((*tree)["entities"].size() == 1);
				CHECK(SceneTargetName((*tree)["entities"][0]) == "InSecond");
				CHECK((*tree)["_meta"]["revision"] == Json(editor.DescribeOpenScene(mainId)->Revision));
			}
			// Closing the played scene's tab is allowed, as scene.open's replacement of it is: the session plays on and
			// belongs to no open scene, so "play" still reads it without a scene target and no target names it.
			REQUIRE(setup.Call("scene.close", Json{ { "scene", "Assets/Scenes/Second.scene" }, { "discardChanges", true } }).has_value());
			CHECK(editor.GetPlay().IsPlaying());
			const Result<Json> orphan = setup.Call("scene.tree", Json{ { "format", "json" }, { "target", "play" } });
			REQUIRE_MESSAGE(orphan.has_value(), orphan.error().ToString());
			CHECK(SceneTargetName((*orphan)["entities"][0]) == "InSecond");
			CHECK(setup.Call("scene.tree", playMain).error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(setup.Call("play.stop", Json::object()).has_value());
		}

		// Decision 13: a play selection belongs to the played scene, also when edit.select makes it from another tab, where
		// a request without a scene target reaches the session (decision 16). The result lists the selection it stored, in
		// the played scene's document, which edit.getSelection with that scene as target reads back; the active scene keeps
		// its own selection.
		TEST_CASE("SceneTarget: edit.select during play from another tab reports the selection it stores in the played scene")
		{
			Test::AutomationFixture setup("SceneTargetPlaySelection");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Ball" } }).has_value());
			const Result<Json> second = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Kept" } }).has_value());
			REQUIRE(setup.Call("edit.select", Json{ { "entities", Json::array({ "/Kept" }) } }).has_value());
			const Json inMain{ { "scene", "Assets/Scenes/Main.scene" } };
			REQUIRE(setup.Call("scene.activate", inMain).has_value());
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true } }).has_value());
			REQUIRE(setup.Call("scene.activate", Json{ { "scene", (*second)["id"] } }).has_value());

			const Result<Json> selected = setup.Call("edit.select", Json{ { "entities", Json::array({ "/Ball" }) } });
			REQUIRE_MESSAGE(selected.has_value(), selected.error().ToString());
			REQUIRE((*selected)["selection"].size() == 1);
			CHECK(SceneTargetName((*selected)["selection"][0]) == "Ball");
			const Result<Json> played = setup.Call("edit.getSelection", inMain);
			REQUIRE(played.has_value());
			REQUIRE((*played)["selection"].size() == 1);
			CHECK(SceneTargetName((*played)["selection"][0]) == "Ball");
			const Result<Json> active = setup.Call("edit.getSelection", Json::object());
			REQUIRE(active.has_value());
			REQUIRE((*active)["selection"].size() == 1);
			CHECK(SceneTargetName((*active)["selection"][0]) == "Kept");
			REQUIRE(setup.Call("play.stop", Json::object()).has_value());
		}

		// Decision 16's play rules for script.eval: context "play" reaches the play VM without a scene target, whatever tab
		// is active, and with a target naming the played scene; a target naming another open scene leaves the session out of
		// reach, so "play" is refused at /scene there, as "target": "play" is, while "edit" evaluates over that scene.
		TEST_CASE("SceneTarget: script.eval in Play reaches the play VM only through the played scene")
		{
			Test::AutomationFixture setup("SceneTargetEvalPlay");
			const Result<Json> second = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			REQUIRE(setup.Call("play.start", Json{ { "mode", "play" }, { "lockstep", true } }).has_value());
			const Json code{ { "context", "play" }, { "code", "return 1 + 2" } };
			const Result<Json> untargeted = setup.Call("script.eval", code);
			REQUIRE_MESSAGE(untargeted.has_value(), untargeted.error().ToString());
			CHECK((*untargeted)["value"] == Json(3));
			Json played = code;
			played["scene"] = "Assets/Scenes/Second.scene";
			const Result<Json> inPlayed = setup.Call("script.eval", played);
			REQUIRE_MESSAGE(inPlayed.has_value(), inPlayed.error().ToString());
			CHECK((*inPlayed)["value"] == Json(3));

			Json other = code;
			other["scene"] = "Assets/Scenes/Main.scene";
			const Result<Json> refused = setup.Call("script.eval", other);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(refused.error().GetIssues().empty());
			CHECK(refused.error().GetIssues().front().JsonPointer == "/scene");
			Json edit = other;
			edit["context"] = "edit";
			const Result<Json> inEdit = setup.Call("script.eval", edit);
			CHECK_MESSAGE(inEdit.has_value(), (inEdit ? std::string() : inEdit.error().ToString()));
			REQUIRE(setup.Call("play.stop", Json::object()).has_value());
		}

		// Decision 14 (the deny-mutations policy, EditorAutomationPolicy): discarding unsaved changes writes no file and runs
		// no command, so the server refuses every request that would drop an open scene's changes, not only the active
		// scene's: scene.close {discardChanges} of a dirty tab, scene.open {reload, discardChanges} of a dirty open scene that
		// is not active, and session.shutdown {force} while a tab that is not active is dirty. Each is PermissionDenied and
		// changes nothing; closing a clean tab discards nothing and stays allowed.
		TEST_CASE("SceneTarget: deny-mutations refuses discarding the unsaved changes of any open scene")
		{
			Test::AutomationFixture setup("SceneTargetDenyDiscard");
			EditorContext& editor = setup.GetEditor();
			REQUIRE(setup.Call("entity.create", Json{ { "name", "HumanEdit" } }).has_value());
			const Result<Json> second = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			const OpenSceneId mainId = editor.FindOpenScene(*VfsPath::Parse("project://Assets/Scenes/Main.scene"));
			REQUIRE(mainId != NoOpenScene);
			const Result<OpenSceneInfo> before = editor.DescribeOpenScene(mainId);
			REQUIRE(before.has_value());
			REQUIRE(before->Dirty);
			REQUIRE_FALSE(editor.IsSceneDirty());
			setup.GetClient().GetServer().SetEditorPolicy({ .DenyMutations = true });

			for (const auto& [method, params] : std::vector<std::pair<std::string, Json>>{
					 { "scene.close", Json{ { "scene", "Assets/Scenes/Main.scene" }, { "discardChanges", true } } },
					 { "scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true }, { "discardChanges", true } } },
					 { "session.shutdown", Json{ { "force", true } } } })
			{
				INFO(method, " ", params.dump());
				const Result<Json> denied = setup.Call(method, params);
				REQUIRE_FALSE(denied.has_value());
				CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);
			}
			const Result<OpenSceneInfo> after = editor.DescribeOpenScene(mainId);
			REQUIRE(after.has_value());
			CHECK(after->Dirty);
			CHECK(after->Revision == before->Revision);
			REQUIRE(editor.FindScene(mainId) != nullptr);
			CHECK(editor.FindScene(mainId)->FindEntityByPath("/HumanEdit").IsValid());
			CHECK_FALSE(editor.GetShutdownRequest().has_value());
			const Result<Json> closed = setup.Call("scene.close", Json::object());
			CHECK_MESSAGE(closed.has_value(), (closed ? std::string() : closed.error().ToString()));
			CHECK(editor.GetActiveSceneId() == mainId);
		}

		// Decision 16 and ADR 0028's ledger ("a path or $ref resolves in the batch's target scene, the active scene when the
		// batch names none"; Roadmap M20 acceptance "a batch $ref and an entity path resolve in the targeted scene"): a batch
		// with a target runs every op there, as one undo step of that scene's history; an op cannot carry its own target.
		TEST_CASE("SceneTarget: a batch resolves entity paths and $ref in its target scene")
		{
			Test::AutomationFixture setup("SceneTargetBatch");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Board" } }).has_value());
			const Result<Json> second = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Board" } }).has_value());
			const Result<Json> secondHistory = setup.Call("edit.history", Json::object());
			REQUIRE(secondHistory.has_value());

			Json batch{ { "scene", "Assets/Scenes/Main.scene" }, { "label", "Pieces" } };
			batch["ops"] = Json::array({ Json{ { "method", "entity.create" }, { "params", Json{ { "name", "Piece" }, { "parent", "/Board" } } } },
				Json{ { "method", "entity.update" }, { "params", Json{ { "entity", Json{ { "$ref", "0.entity.id" } } }, { "name", "Renamed" } } } } });
			const Result<Json> ran = setup.Call("edit.batch", batch);
			REQUIRE_MESSAGE(ran.has_value(), ran.error().ToString());

			const Json inMain{ { "scene", "Assets/Scenes/Main.scene" }, { "format", "json" } };
			const Result<Json> mainTree = setup.Call("scene.tree", inMain);
			REQUIRE(mainTree.has_value());
			REQUIRE((*mainTree)["entities"].size() == 2);
			CHECK(SceneTargetName((*mainTree)["entities"][1]) == "Renamed");
			const Result<Json> mainHistory = setup.Call("edit.history", Json{ { "scene", "Assets/Scenes/Main.scene" } });
			REQUIRE(mainHistory.has_value());
			CHECK(JsonReader((*mainHistory)["entries"].back()["label"]).ReadString().value_or(std::string()).contains("Pieces"));
			// The active scene's Board has no child, and its history did not change.
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 1);
			const Result<Json> unchanged = setup.Call("edit.history", Json::object());
			REQUIRE(unchanged.has_value());
			CHECK((*unchanged)["entries"].size() == (*secondHistory)["entries"].size());

			// An op carrying its own target is refused before anything runs.
			Json opTarget{ { "ops", Json::array({ Json{ { "method", "entity.create" }, { "params", Json{ { "name", "Stray" }, { "scene", "Assets/Scenes/Main.scene" } } } } }) } };
			const Result<Json> refused = setup.Call("edit.batch", opTarget);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// The merge with M19 (Docs/Decisions/0031-m20-contract.md, "Merge with M19"; ADR 0028's ledger, "Entity paths and
		// scenes": M20 passes the target to M19's resolver): a named $ref and an entity path inside component data (a script's
		// Field.Entity()) resolve in the batch's target scene, never in the active scene, which holds the same paths with
		// other ids; a path that names an entity of the active scene only rolls the targeted batch back.
		TEST_CASE("SceneTarget: a batch's named $ref and the entity paths in its component data resolve in the target scene")
		{
			Test::AutomationFixture setup("SceneTargetResolver");
			const std::string hud = "local Hud = { Fields = { Score = Field.Entity() } }\nreturn Script.Define(\"Hud\", Hud)\n";
			REQUIRE(setup.Call("script.write", Json{ { "path", "Assets/Scripts/Hud.luau" }, { "source", hud } }).has_value());
			const Result<Json> second = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			// The active scene holds the same paths, and one more entity.
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Game" } }).has_value());
			const Result<Json> activeScore = setup.Call("entity.create", Json{ { "name", "ScoreText" }, { "parent", "/Game" } });
			REQUIRE(activeScore.has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "OnlyActive" } }).has_value());
			EditorContext& editor = setup.GetEditor();
			const size_t activeCount = editor.GetScene().GetEntityCount();

			const Json slot{ { "Script", "Assets/Scripts/Hud.luau" }, { "Fields", Json{ { "Score", "/Game/ScoreText" } } } };
			const Json hudComponents{ { "Script", Json{ { "Scripts", Json::array({ slot }) } } } };
			Json batch{ { "scene", "Assets/Scenes/Main.scene" }, { "label", "Hud" } };
			batch["ops"] = Json::array({ Json{ { "method", "entity.create" }, { "params", Json{ { "name", "Game" } } }, { "name", "game" } },
				Json{ { "method", "entity.create" }, { "params", Json{ { "name", "ScoreText" }, { "parent", Json{ { "$ref", "game.entity.id" } } } } } },
				Json{ { "method", "entity.create" }, { "params", Json{ { "name", "Hud" }, { "components", hudComponents } } } } });
			const Result<Json> ran = setup.Call("edit.batch", batch);
			REQUIRE_MESSAGE(ran.has_value(), ran.error().ToString());
			const Json mainScore = (*ran)["results"][1]["entity"]["id"];
			CHECK(mainScore != (*activeScore)["entity"]["id"]);
			CHECK((*ran)["results"][1]["entity"]["path"] == Json("/Game/ScoreText"));

			const Result<Json> hudEntity = setup.Call("entity.get", Json{ { "scene", "Assets/Scenes/Main.scene" }, { "entity", "/Hud" } });
			REQUIRE_MESSAGE(hudEntity.has_value(), hudEntity.error().ToString());
			CHECK((*hudEntity)["entity"]["components"]["Script"]["Scripts"][0]["Fields"]["Score"] == mainScore);
			CHECK(editor.GetScene().GetEntityCount() == activeCount);
			CHECK(editor.GetScene().GetName() == "Second");

			// A path that only the active scene holds names nothing in the target: the batch rolls back there.
			const OpenSceneId mainId = editor.FindOpenScene(*VfsPath::Parse("project://Assets/Scenes/Main.scene"));
			const Result<OpenSceneInfo> before = editor.DescribeOpenScene(mainId);
			REQUIRE(before.has_value());
			const Json stray{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Fields", Json{ { "Score", "/OnlyActive" } } } } }) } } } };
			Json missing{ { "scene", "Assets/Scenes/Main.scene" } };
			missing["ops"] = Json::array({ Json{ { "method", "entity.create" }, { "params", Json{ { "name", "Lives" } } } },
				Json{ { "method", "entity.update" }, { "params", Json{ { "entity", "/Hud" }, { "components", stray } } } } });
			const Json refused = setup.Request("edit.batch", missing);
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			CHECK(refused["error"]["data"]["errorCode"] == Json("NotFound"));
			CHECK(refused["error"]["data"]["failedOp"] == Json(1));
			CHECK(editor.DescribeOpenScene(mainId)->EntityCount == before->EntityCount);
			const Scene* main = std::as_const(editor).FindScene(mainId);
			REQUIRE(main != nullptr);
			CHECK_FALSE(main->FindEntityByPath("/Lives").IsValid());
		}
	}

}
