#include "TestsPCH.h"

#include "EditorCore/Automation/ScriptMethods.h"

#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Session/ReplayFingerprint.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

	namespace {

		Json CallEditorScriptMethod(Test::AutomationFixture& fixture, std::string_view method, const Json& params)
		{
			auto result = fixture.Call(method, params);
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			return std::move(*result);
		}

		// script.write's changedLines for the lines [beforeLine, beforeEnd) of the replaced source and [afterLine, afterEnd)
		// of the written one.
		Json ChangedLines(uint32_t beforeLine, uint32_t beforeEnd, uint32_t afterLine, uint32_t afterEnd)
		{
			return Json{ { "before", Json{ { "line", beforeLine }, { "endLine", beforeEnd } } }, { "after", Json{ { "line", afterLine }, { "endLine", afterEnd } } } };
		}

		Json WriteScript(Test::AutomationFixture& fixture, std::string_view path, std::string_view source)
		{
			return CallEditorScriptMethod(fixture, "script.write", Json{ { "path", path }, { "source", source } });
		}

		// The hash the replay start fingerprint of the open scene records for `script`, by the recorders' own function once
		// the editor has published every import (without the session's script field schemas, which only type a script's
		// asset fields); empty when the fingerprint lists no such asset.
		std::string FingerprintHash(Test::AutomationFixture& fixture, const Json& script)
		{
			const Result<UUID> handle = JsonReader(script["id"]).ReadUUID();
			REQUIRE(handle.has_value());
			EditorAssetManager& assets = fixture.GetEditor().GetAssets();
			assets.WaitIdle();
			const Result<ReplayFingerprint> fingerprint = ComputeReplayFingerprint(fixture.GetEditor().GetScene(), nullptr, &assets);
			REQUIRE_MESSAGE(fingerprint.has_value(), (fingerprint ? "" : fingerprint.error().ToString()));
			const auto found = std::ranges::find(fingerprint->Assets, *handle, &ReplayAssetFingerprint::Handle);
			return found == fingerprint->Assets.end() ? std::string() : found->Hash;
		}

		// Five lines of a module, each ending in a newline.
		constexpr std::string_view FiveLines = "local A = 1\nlocal B = 2\nlocal C = 3\nlocal D = 4\nreturn { A = A, B = B, C = C, D = D }\n";

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ScriptMethods: all three templates create readable typed scripts with stable handles across undo")
		{
			Test::AutomationFixture fixture("ScriptTemplates", true, std::nullopt, true);
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

		TEST_CASE("ScriptMethods: write preserves exact bytes and undo restores source with the same handle")
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

		TEST_CASE("ScriptMethods: write returns full diagnostic ranges while retaining invalid source")
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

		TEST_CASE("ScriptMethods: create refuses an existing path and write accepts an empty source")
		{
			Test::AutomationFixture fixture("ScriptCreateConflict", true, std::nullopt, true);
			const Json params{ { "path", "Assets/Scripts/Existing.luau" }, { "template", "Module" } };
			CallEditorScriptMethod(fixture, "script.create", params);
			CHECK(fixture.Request("script.create", params)["error"]["data"]["errorCode"] == Json("AlreadyExists"));
			CallEditorScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Empty.luau" }, { "source", "" } });
			CHECK(CallEditorScriptMethod(fixture, "script.read", Json{ { "path", "Assets/Scripts/Empty.luau" } })["source"] == Json(""));
		}

		TEST_CASE("ScriptMethods: dry runs leave no source or metadata and do not change history")
		{
			Test::AutomationFixture fixture("ScriptDryRun", true, std::nullopt, true);
			const Json before = CallEditorScriptMethod(fixture, "edit.history", Json::object());
			const Json dry = CallEditorScriptMethod(fixture, "script.create", Json{ { "path", "Assets/Scripts/Dry.luau" }, { "template", "Behaviour" }, { "dryRun", true } });
			CHECK(dry["dryRun"] == Json(true));
			CHECK(dry["undoIndex"] == Json(0));
			CHECK_FALSE(fixture.Call("script.read", Json{ { "path", "Assets/Scripts/Dry.luau" } }).has_value());
			CHECK_FALSE(fixture.Call("asset.info", Json{ { "asset", "Assets/Scripts/Dry.luau" } }).has_value());
			const Json after = CallEditorScriptMethod(fixture, "edit.history", Json::object());
			CHECK(after["entries"] == before["entries"]);
		}

		TEST_CASE("ScriptMethods: a failed batch rolls back script bytes and metadata")
		{
			Test::AutomationFixture fixture("ScriptBatchRollback");
			const Json response = fixture.Request("edit.batch", Json{ { "ops", Json::array({ Json{ { "method", "script.write" }, { "params", Json{ { "path", "Assets/Scripts/RolledBack.luau" }, { "source", "return 42" } } } }, Json{ { "method", "entity.update" }, { "params", Json{ { "entity", "/Missing" }, { "name", "Never" } } } } }) } });
			CHECK(response.contains("error"));
			CHECK(response["error"]["data"]["failedOp"] == Json(1));
			CHECK(response["error"]["data"]["errorCode"] == Json("NotFound"));
			CHECK_FALSE(fixture.Call("script.read", Json{ { "path", "Assets/Scripts/RolledBack.luau" } }).has_value());
			CHECK_FALSE(fixture.Call("asset.info", Json{ { "asset", "Assets/Scripts/RolledBack.luau" } }).has_value());
		}

		TEST_CASE("ScriptMethods: script paths are confined to project Assets and use the luau extension")
		{
			Test::AutomationFixture fixture("ScriptPathValidation");
			for (const std::string_view path : std::array{ "../outside.luau", "engine://outside.luau", "Library/Cache.luau", "Assets/Data.txt" })
			{
				const Json response = fixture.Request("script.write", Json{ { "path", path }, { "source", "return 1" } });
				CHECK(response["error"]["code"] == Json(-32602));
				CHECK_FALSE(response["error"]["data"]["issues"].empty());
			}
		}

		TEST_CASE("ScriptMethods: checking all scripts reports required module locations without editing history")
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

		TEST_CASE("ScriptMethods: fields preserves array element ranges defaults and asset types")
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

		// Docs/Decisions/0038-m27-rolling-ball.md friction F24: a builder shows that two reads hold the same source by their
		// hashes, and the hash is the one a replay's start fingerprint records for the script, so it names what a replay saw.
		TEST_CASE("ScriptMethods: script.read and script.write report the content hash a replay's start fingerprint records for the script")
		{
			Test::AutomationFixture fixture("ScriptContentHash");
			const std::string path = "Assets/Scripts/Spinner.luau";
			const std::string first = "local Spinner = {}\n-- Turns slowly.\nfunction Spinner.OnFixedUpdate(self: any, dt: number)\nend\nreturn Script.Define(\"Spinner\", Spinner)\n";
			Json written = WriteScript(fixture, path, first);
			REQUIRE(written["hash"].is_string());
			const std::string hash = JsonReader(written["hash"]).ReadString().value_or("");
			CHECK(hash.size() == 16);
			CHECK(std::ranges::all_of(hash, [](char digit)
			{
				return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
			}));
			Json read = CallEditorScriptMethod(fixture, "script.read", Json{ { "path", path } });
			CHECK(read["hash"] == written["hash"]);
			CHECK(CallEditorScriptMethod(fixture, "script.read", Json{ { "path", path } })["hash"] == read["hash"]);
			CallEditorScriptMethod(fixture, "entity.create", Json{ { "name", "Spinner" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", path } } }) } } } } } });
			CHECK(FingerprintHash(fixture, written["script"]) == hash);

			// A comment-only change changes it alike in the write, the read and the fingerprint; the old bytes bring it back.
			const std::string second = "local Spinner = {}\n-- Turns slowly, always.\nfunction Spinner.OnFixedUpdate(self: any, dt: number)\nend\nreturn Script.Define(\"Spinner\", Spinner)\n";
			Json rewritten = WriteScript(fixture, path, second);
			CHECK(rewritten["hash"] != written["hash"]);
			CHECK(CallEditorScriptMethod(fixture, "script.read", Json{ { "path", path } })["hash"] == rewritten["hash"]);
			CHECK(Json(FingerprintHash(fixture, written["script"])) == rewritten["hash"]);
			CHECK(WriteScript(fixture, path, first)["hash"] == written["hash"]);
			CHECK(FingerprintHash(fixture, written["script"]) == hash);

			// A source that does not cook has no artifact, so no hash, while its findings say why.
			const std::string broken = "Assets/Scripts/Broken.luau";
			Json invalid = WriteScript(fixture, broken, "local = 1\n");
			CHECK_FALSE(invalid["diagnostics"].empty());
			CHECK(invalid["hash"] == Json(""));
			CHECK(CallEditorScriptMethod(fixture, "script.read", Json{ { "path", broken } })["hash"] == Json(""));
		}

		TEST_CASE("ScriptMethods: script.read and script.write count a last line without a newline and a CRLF line end as one line")
		{
			Test::AutomationFixture fixture("ScriptLineCount");
			const std::string path = "Assets/Scripts/Lines.luau";
			struct LineCase
			{
				std::string_view Source{};
				uint32_t Lines = 0;
			};
			for (const LineCase& lines : std::array{ LineCase{ "", 0 }, LineCase{ "return 1", 1 }, LineCase{ "return 1\n", 1 },
					 LineCase{ "local A = 1\nreturn A", 2 }, LineCase{ "local A = 1\r\nreturn A\r\n", 2 }, LineCase{ "\n\n", 2 }, LineCase{ FiveLines, 5 } })
			{
				CAPTURE(lines.Source);
				Json written = WriteScript(fixture, path, lines.Source);
				CHECK(written["lineCount"] == Json(lines.Lines));
				Json read = CallEditorScriptMethod(fixture, "script.read", Json{ { "path", path } });
				CHECK(read["lineCount"] == Json(lines.Lines));
			}
		}

		TEST_CASE("ScriptMethods: script.write reports the single span of lines an edit changed in the middle, at the start and at the end")
		{
			Test::AutomationFixture fixture("ScriptChangedLines");
			const std::string path = "Assets/Scripts/Values.luau";
			WriteScript(fixture, path, FiveLines);
			struct Edit
			{
				std::string_view Label{};
				std::string_view Source{};
				Json Expected{};
			};
			const std::array edits{
				Edit{ "one line in the middle", "local A = 1\nlocal B = 2\nlocal C = 30\nlocal D = 4\nreturn { A = A, B = B, C = C, D = D }\n", ChangedLines(3, 4, 3, 4) },
				Edit{ "a line added at the start", "--!strict\nlocal A = 1\nlocal B = 2\nlocal C = 30\nlocal D = 4\nreturn { A = A, B = B, C = C, D = D }\n", ChangedLines(1, 1, 1, 2) },
				Edit{ "a line added at the end", "--!strict\nlocal A = 1\nlocal B = 2\nlocal C = 30\nlocal D = 4\nreturn { A = A, B = B, C = C, D = D }\n-- end\n", ChangedLines(7, 7, 7, 8) },
				Edit{ "the last line removed", "--!strict\nlocal A = 1\nlocal B = 2\nlocal C = 30\nlocal D = 4\nreturn { A = A, B = B, C = C, D = D }\n", ChangedLines(7, 8, 7, 7) },
				Edit{ "the first line removed", "local A = 1\nlocal B = 2\nlocal C = 30\nlocal D = 4\nreturn { A = A, B = B, C = C, D = D }\n", ChangedLines(1, 2, 1, 1) },
				Edit{ "one line replaced by three", "local A = 1\nlocal B = 2\nlocal C = 3\nlocal E = 5\nlocal F = 6\nlocal D = 4\nreturn { A = A, B = B, C = C, D = D }\n", ChangedLines(3, 4, 3, 6) },
				Edit{ "two edits apart, one span", "local A = 1\nlocal B = 20\nlocal C = 3\nlocal E = 5\nlocal F = 60\nlocal D = 4\nreturn { A = A, B = B, C = C, D = D }\n", ChangedLines(2, 6, 2, 6) },
			};
			for (const Edit& edit : edits)
			{
				CAPTURE(edit.Label);
				Json written = WriteScript(fixture, path, edit.Source);
				CHECK(written["changedLines"] == edit.Expected);
			}
		}

		TEST_CASE("ScriptMethods: script.write reports a new script's whole file as changed and no changed lines for unchanged bytes")
		{
			Test::AutomationFixture fixture("ScriptChangedLinesNew");
			const std::string path = "Assets/Scripts/Values.luau";
			Json created = WriteScript(fixture, path, FiveLines);
			CHECK(created["changedLines"] == ChangedLines(1, 1, 1, 6));
			Json empty = WriteScript(fixture, "Assets/Scripts/Empty.luau", "");
			CHECK(empty["changedLines"] == ChangedLines(1, 1, 1, 1));
			Json unchanged = WriteScript(fixture, path, FiveLines);
			CHECK(unchanged["undoIndex"] == Json(0));
			REQUIRE(unchanged.contains("changedLines"));
			CHECK(unchanged["changedLines"].is_null());
			CHECK(unchanged["lineCount"] == Json(5));
			CHECK(unchanged["hash"] == created["hash"]);
		}

		TEST_CASE("ScriptMethods: script.write counts a changed line end, CRLF or a newline after the last line, as a changed line")
		{
			Test::AutomationFixture fixture("ScriptChangedLineEnds");
			const std::string path = "Assets/Scripts/Ends.luau";
			WriteScript(fixture, path, "local A = 1\nreturn A");
			Json terminated = WriteScript(fixture, path, "local A = 1\nreturn A\n");
			CHECK(terminated["changedLines"] == ChangedLines(2, 3, 2, 3));
			CHECK(terminated["lineCount"] == Json(2));
			WriteScript(fixture, path, "local A = 1\r\nreturn A\r\n");
			Json mixed = WriteScript(fixture, path, "local A = 1\nreturn A\r\n");
			CHECK(mixed["changedLines"] == ChangedLines(1, 2, 1, 2));
		}

		TEST_CASE("ScriptMethods: a script.write dry run reports the changed lines, line count and hash of the write it would make")
		{
			Test::AutomationFixture fixture("ScriptChangedLinesDryRun");
			const std::string path = "Assets/Scripts/Values.luau";
			Json base = WriteScript(fixture, path, FiveLines);
			const std::string edited = "local A = 1\nlocal B = 20\nlocal C = 3\nlocal D = 4\nreturn { A = A, B = B, C = C, D = D }\n";
			Json dry = CallEditorScriptMethod(fixture, "script.write", Json{ { "path", path }, { "source", edited }, { "dryRun", true } });
			CHECK(dry["dryRun"] == Json(true));
			CHECK(dry["changedLines"] == ChangedLines(2, 3, 2, 3));
			CHECK(dry["lineCount"] == Json(5));
			CHECK(dry["hash"] != base["hash"]);
			CHECK(CallEditorScriptMethod(fixture, "script.read", Json{ { "path", path } })["hash"] == base["hash"]);
			Json real = WriteScript(fixture, path, edited);
			CHECK(real["changedLines"] == dry["changedLines"]);
			CHECK(real["lineCount"] == dry["lineCount"]);
			CHECK(real["hash"] == dry["hash"]);
			Json dryNew = CallEditorScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/New.luau" }, { "source", FiveLines }, { "dryRun", true } });
			CHECK(dryNew["changedLines"] == ChangedLines(1, 1, 1, 6));
			CHECK(dryNew["lineCount"] == Json(5));
			// The artifact names its script's path, so the same bytes at another path have another hash.
			CHECK(dryNew["hash"].is_string());
			CHECK(dryNew["hash"] != Json(""));
			CHECK(dryNew["hash"] != base["hash"]);
			CHECK_FALSE(fixture.Call("script.read", Json{ { "path", "Assets/Scripts/New.luau" } }).has_value());
		}
	}

}
