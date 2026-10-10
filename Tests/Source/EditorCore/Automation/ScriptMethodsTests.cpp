#include "TestsPCH.h"

#include "EditorCore/Automation/ScriptMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <array>

namespace Engine {

	namespace {

		Json CallEditorScriptMethod(Test::AutomationFixture& fixture, std::string_view method, const Json& params)
		{
			auto result = fixture.Call(method, params);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ScriptMethods: all three templates create readable typed scripts with stable handles across undo" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptTemplates");
			for (const std::string_view name : std::array{ "Behaviour", "Module", "Test" })
			{
				const std::string path = "Assets/Scripts/" + std::string(name) + ".luau";
				const Json created = CallEditorScriptMethod(fixture, "script.create", Json{ { "path", path }, { "template", name } });
				CHECK(created["script"]["type"] == Json("Script"));
				CHECK(created["script"]["path"] == Json(path));
				CHECK(created["undoIndex"] != Json(0));
				const Json read = CallEditorScriptMethod(fixture, "script.read", Json{ { "path", path } });
				CHECK_FALSE(read["source"].empty());
				const Json checked = CallEditorScriptMethod(fixture, "script.check", Json{ { "paths", Json::array({ path }) } });
				CHECK(checked["passed"] == Json(true));
				CallEditorScriptMethod(fixture, "edit.undo", Json::object());
				CHECK(fixture.Request("script.read", Json{ { "path", path } })["error"]["data"]["errorCode"] == Json("NotFound"));
				CallEditorScriptMethod(fixture, "edit.redo", Json::object());
				const Json restored = CallEditorScriptMethod(fixture, "script.read", Json{ { "path", path } });
				CHECK(restored["script"]["id"] == created["script"]["id"]);
				CHECK(restored["source"] == read["source"]);
			}
		}

		TEST_CASE("ScriptMethods: write preserves exact bytes and undo restores source with the same handle" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptWriteUndo");
			const std::string path = "Assets/Scripts/Library.luau";
			const std::string first = "--!strict\r\nreturn { Value = 1 }\r\n";
			const std::string second = "--!strict\nreturn { Value = 2 }\n";
			const Json created = CallEditorScriptMethod(fixture, "script.write", Json{ { "path", path }, { "source", first } });
			const Json changed = CallEditorScriptMethod(fixture, "script.write", Json{ { "path", path }, { "source", second } });
			CHECK(changed["script"]["id"] == created["script"]["id"]);
			CHECK(CallEditorScriptMethod(fixture, "script.read", Json{ { "path", path } })["source"] == Json(second));
			CallEditorScriptMethod(fixture, "edit.undo", Json::object());
			CHECK(CallEditorScriptMethod(fixture, "script.read", Json{ { "path", path } })["source"] == Json(first));
		}

		TEST_CASE("ScriptMethods: write returns full diagnostic ranges while retaining invalid source" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptWriteDiagnostics");
			const std::string path = "Assets/Scripts/Mistake.luau";
			const std::string source = "--!strict\nlocal count: number = \"wrong\"\nreturn count\n";
			const Json written = CallEditorScriptMethod(fixture, "script.write", Json{ { "path", path }, { "source", source } });
			REQUIRE_FALSE(written["diagnostics"].empty());
			const Json& diagnostic = written["diagnostics"][0];
			CHECK(diagnostic["file"] == Json(path));
			CHECK(diagnostic["line"] == Json(2));
			CHECK(diagnostic["column"] != Json(0));
			CHECK(diagnostic["endLine"] == Json(2));
			CHECK(diagnostic["endColumn"] > diagnostic["column"]);
			CHECK_FALSE(diagnostic["code"].empty());
			CHECK(diagnostic["severity"] == Json("Error"));
			CHECK_FALSE(diagnostic["message"].empty());
			CHECK(CallEditorScriptMethod(fixture, "script.read", Json{ { "path", path } })["source"] == Json(source));
		}

		TEST_CASE("ScriptMethods: create refuses an existing path and write accepts an empty source" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptCreateConflict");
			const Json params{ { "path", "Assets/Scripts/Existing.luau" }, { "template", "Module" } };
			CallEditorScriptMethod(fixture, "script.create", params);
			CHECK(fixture.Request("script.create", params)["error"]["data"]["errorCode"] == Json("AlreadyExists"));
			CallEditorScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Empty.luau" }, { "source", "" } });
			CHECK(CallEditorScriptMethod(fixture, "script.read", Json{ { "path", "Assets/Scripts/Empty.luau" } })["source"] == Json(""));
		}

		TEST_CASE("ScriptMethods: dry runs leave no source or metadata and do not change history" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptDryRun");
			const Json before = CallEditorScriptMethod(fixture, "edit.history", Json::object());
			const Json dry = CallEditorScriptMethod(fixture, "script.create", Json{ { "path", "Assets/Scripts/Dry.luau" }, { "template", "Behaviour" }, { "dryRun", true } });
			CHECK(dry["dryRun"] == Json(true));
			CHECK(dry["undoIndex"] == Json(0));
			CHECK_FALSE(fixture.Call("script.read", Json{ { "path", "Assets/Scripts/Dry.luau" } }).has_value());
			CHECK_FALSE(fixture.Call("asset.info", Json{ { "asset", "Assets/Scripts/Dry.luau" } }).has_value());
			const Json after = CallEditorScriptMethod(fixture, "edit.history", Json::object());
			CHECK(after["entries"] == before["entries"]);
		}

		TEST_CASE("ScriptMethods: a failed batch rolls back script bytes and metadata" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptBatchRollback");
			const Json response = fixture.Request("edit.batch", Json{ { "ops", Json::array({ Json{ { "method", "script.write" }, { "params", Json{ { "path", "Assets/Scripts/RolledBack.luau" }, { "source", "return 42" } } } }, Json{ { "method", "entity.update" }, { "params", Json{ { "entity", "/Missing" }, { "name", "Never" } } } } }) } });
			CHECK(response.contains("error"));
			CHECK(response["error"]["data"]["failedOp"] == Json(1));
			CHECK(response["error"]["data"]["errorCode"] == Json("NotFound"));
			CHECK_FALSE(fixture.Call("script.read", Json{ { "path", "Assets/Scripts/RolledBack.luau" } }).has_value());
			CHECK_FALSE(fixture.Call("asset.info", Json{ { "asset", "Assets/Scripts/RolledBack.luau" } }).has_value());
		}

		TEST_CASE("ScriptMethods: script paths are confined to project Assets and use the luau extension" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptPathValidation");
			for (const std::string_view path : std::array{ "../outside.luau", "engine://outside.luau", "Library/Cache.luau", "Assets/Data.txt" })
			{
				const Json response = fixture.Request("script.write", Json{ { "path", path }, { "source", "return 1" } });
				CHECK(response["error"]["code"] == Json(-32602));
				CHECK_FALSE(response["error"]["data"]["issues"].empty());
			}
		}

		TEST_CASE("ScriptMethods: checking all scripts reports required module locations without editing history" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptCheckGraph");
			CallEditorScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Dependency.luau" }, { "source", "--!strict\nlocal value: number = \"wrong\"\nreturn value" } });
			CallEditorScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Consumer.luau" }, { "source", "return require('./Dependency')" } });
			const Json before = CallEditorScriptMethod(fixture, "edit.history", Json::object());
			const Json checked = CallEditorScriptMethod(fixture, "script.check", Json::object());
			CHECK(checked["passed"] == Json(false));
			CHECK(checked["paths"] == Json::array({ "Assets/Scripts/Consumer.luau", "Assets/Scripts/Dependency.luau" }));
			REQUIRE_FALSE(checked["diagnostics"].empty());
			CHECK(checked["diagnostics"][0]["file"] == Json("Assets/Scripts/Dependency.luau"));
			CHECK(CallEditorScriptMethod(fixture, "edit.history", Json::object())["entries"] == before["entries"]);
		}

		TEST_CASE("ScriptMethods: fields preserves array element ranges defaults and asset types" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptFieldsSchema");
			CallEditorScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Fields.luau" }, { "source", "local Fields = {}; Fields.Fields = { Values = Field.Array(Field.Number(2, { Min = 1, Max = 5 })), "
																														"Sound = Field.Asset('AudioClip') }; return Script.Define('Fields', Fields)" } });
			const Json fields = CallEditorScriptMethod(fixture, "script.fields", Json{ { "script", "Assets/Scripts/Fields.luau" } });
			CHECK(fields["kind"] == Json("Behaviour"));
			REQUIRE(fields["fields"].size() == 2);
			CHECK(fields["fields"][0]["name"] == Json("Sound"));
			CHECK(fields["fields"][0]["schema"]["x-assetType"] == Json("AudioClip"));
			CHECK(fields["fields"][1]["name"] == Json("Values"));
			CHECK(fields["fields"][1]["schema"]["items"]["minimum"] == Json(1));
			CHECK(fields["fields"][1]["schema"]["items"]["maximum"] == Json(5));
			CHECK(fields["fields"][1]["schema"]["items"]["default"] == Json(2));
		}
	}

}
