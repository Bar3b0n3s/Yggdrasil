#include "TestsPCH.h"
#include "Engine/Scripting/ScriptError.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <limits>

namespace Engine {

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptError: a repeated diagnostic advances the cursor and preserves its count")
		{
			ScriptErrorStream errors;
			CHECK(errors.GetCursor() == 0);
			CHECK(errors.Read().empty());
			ScriptError first{ .Script = "Assets/Scripts/Ball.luau", .Line = 12, .Message = "bad field" };
			const uint64_t firstId = errors.Add(first).ID;
			first.Tick = 18;
			const ScriptError repeated = errors.Add(first);
			CHECK(repeated.ID > firstId);
			CHECK(repeated.Count == 2);
			CHECK(repeated.Tick == 18);
			const auto updates = errors.Read(firstId);
			REQUIRE(updates.size() == 1);
			CHECK(updates.front().Count == 2);
			CHECK(errors.GetErrors().size() == 1);
			CHECK(errors.GetCursor() == repeated.ID);
			CHECK(errors.Read(repeated.ID).empty());
			CHECK(errors.Read(0, 0).empty());
			CHECK(errors.Read(std::numeric_limits<uint64_t>::max()).empty());
		}

		TEST_CASE("ScriptError: JSON retains structured source frames and entity identity")
		{
			ScriptError error{
				.ID = 17,
				.Kind = ScriptErrorKind::Runtime,
				.Script = "Assets/Scripts/Board.luau",
				.Line = 88,
				.Column = 9,
				.Message = "bad field",
				.Callback = "OnFixedUpdate",
				.Entity = UUID(0x8f3a2c1d9e4b7a60),
				.EntityName = "Game",
				.Tick = 412,
				.Count = 2,
				.Traceback = { { "Assets/Scripts/Board.luau", 88, "LockPiece" } },
			};
			const Json json = ScriptErrorToJson(error);
			const Json expected = {
				{ "id", 17 },
				{ "kind", "runtime" },
				{ "script", "Assets/Scripts/Board.luau" },
				{ "line", 88 },
				{ "column", 9 },
				{ "message", "bad field" },
				{ "callback", "OnFixedUpdate" },
				{ "entity", { { "id", "8f3a2c1d9e4b7a60" }, { "name", "Game" } } },
				{ "tick", 412 },
				{ "count", 2 },
				{ "traceback", Json::array({ { { "script", "Assets/Scripts/Board.luau" }, { "line", 88 }, { "function", "LockPiece" } } }) },
			};
			CHECK(json == expected);
			error.Message = "changed";
			error.Traceback.clear();
			CHECK(json == expected);
			const std::array kinds = { ScriptErrorKind::Compile, ScriptErrorKind::Type, ScriptErrorKind::Runtime,
				ScriptErrorKind::Timeout, ScriptErrorKind::Memory };
			const std::array<std::string_view, 5> names = { "compile", "type", "runtime", "timeout", "memory" };
			for (size_t index = 0; index < kinds.size(); ++index)
				CHECK(ScriptErrorKindToString(kinds[index]) == names[index]);
		}

		TEST_CASE("ScriptError: separate embedded replay expectations retain distinct cursors and authored locations")
		{
			ScriptErrorStream errors;
			ScriptError first{
				.Script = "Assets/Tests/Level.replay",
				.Line = 2,
				.Column = 7,
				.Message = "bad predicate",
				.JsonPointer = "/Expect/0/Luau",
			};
			const ScriptError saved = errors.Add(first);
			first.JsonPointer = "/Expect/1/Luau";
			const ScriptError second = errors.Add(first);
			CHECK(second.ID > saved.ID);
			REQUIRE(errors.GetErrors().size() == 2);
			CHECK(errors.GetErrors()[0].Count == 1);
			CHECK(errors.GetErrors()[1].Count == 1);
			CHECK(ScriptErrorToJson(saved)["jsonPointer"] == "/Expect/0/Luau");
			CHECK(ScriptErrorToJson(second)["jsonPointer"] == "/Expect/1/Luau");
			CHECK(ScriptErrorToJson(second)["line"] == 2);
			CHECK(ScriptErrorToJson(second)["column"] == 7);
		}

		TEST_CASE("ScriptError: repeats move behind newer distinct diagnostics and replace their context")
		{
			ScriptErrorStream errors;
			ScriptError first{ .Script = "Assets/Shared.luau", .Line = 2, .Message = "failure", .EntityName = "First" };
			static_cast<void>(errors.Add(first));
			ScriptError other = first;
			other.Line = 3;
			const ScriptError middle = errors.Add(other);
			first.EntityName = "Second";
			first.Callback = "OnStart";
			first.Column = 17;
			first.Traceback = { { "Assets/Caller.luau", 42, "OnStart" } };
			const ScriptError latest = errors.Add(first);
			const auto limited = errors.Read(0, 1);
			REQUIRE(limited.size() == 1);
			CHECK(limited.front().ID == middle.ID);
			const auto updates = errors.Read(middle.ID);
			REQUIRE(updates.size() == 1);
			CHECK(updates.front().ID == latest.ID);
			CHECK(updates.front().EntityName == "Second");
			CHECK(updates.front().Column == 17);
			CHECK(updates.front().Callback == "OnStart");
			REQUIRE(updates.front().Traceback.size() == 1);
			CHECK(updates.front().Traceback.front().Script == "Assets/Caller.luau");
			CHECK(updates.front().Count == 2);
		}
	}

}
