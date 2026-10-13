#include "TestsPCH.h"
#include "Engine/Scripting/ScriptError.h"

#include "Engine/Core/Utf8.h"
#include "Engine/Scripting/ScriptLimits.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <limits>
#include <string>

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

		TEST_CASE("ScriptError: a retained message is cut at a UTF-8 boundary and names its full size")
		{
			ScriptErrorStream errors;
			// Two-byte sequences, so the cut before the marker would split one without the UTF-8 rule.
			std::string message;
			for (size_t index = 0; index < 10000; ++index)
				message += "\xC3\xA9";
			const std::string marker = std::format(" [truncated: {} bytes]", message.size());
			const ScriptError added = errors.Add({ .Script = "Assets/Scripts/Spam.luau", .Line = 4, .Message = message });
			CHECK(added.Message.size() <= MaxScriptLogMessageBytes);
			CHECK(added.Message.size() > MaxScriptLogMessageBytes - marker.size() - 4);
			CHECK(added.Message.ends_with(marker));
			CHECK(IsValidUtf8(added.Message));
			CHECK(message.starts_with(added.Message.substr(0, added.Message.size() - marker.size())));
			// The retained copy is already bounded: another stream (a host's history) keeps it unchanged, and a repeat of
			// the same long message deduplicates against it.
			ScriptErrorStream history;
			CHECK(history.Add(added).Message == added.Message);
			const ScriptError repeated = errors.Add({ .Script = "Assets/Scripts/Spam.luau", .Line = 4, .Message = message });
			CHECK(repeated.Count == 2);
			CHECK(errors.GetErrors().size() == 1);
			// A message of exactly the cap is kept whole; one byte more is cut.
			const std::string exact(MaxScriptLogMessageBytes, 'x');
			CHECK(errors.Add({ .Message = exact }).Message == exact);
			const ScriptError over = errors.Add({ .Message = exact + "y" });
			CHECK(over.Message.size() == MaxScriptLogMessageBytes);
			CHECK(over.Message.ends_with(std::format(" [truncated: {} bytes]", MaxScriptLogMessageBytes + 1)));
		}

		TEST_CASE("ScriptError: a retained traceback keeps its innermost frames and counts the ones left out")
		{
			ScriptError deep{ .Script = "Assets/Scripts/Recursive.luau", .Line = 3, .Message = "too deep" };
			for (uint32_t line = 1; line <= 100; ++line)
				deep.Traceback.push_back({ "Assets/Scripts/Recursive.luau", line, "Descend" });
			ScriptErrorStream errors;
			const ScriptError added = errors.Add(deep);
			REQUIRE(added.Traceback.size() == MaxScriptErrorTracebackFrames);
			for (size_t index = 0; index + 1 < MaxScriptErrorTracebackFrames; ++index)
				CHECK(added.Traceback[index].Line == index + 1);
			const ScriptTraceFrame& marker = added.Traceback.back();
			CHECK(marker.Script.empty());
			CHECK(marker.Line == 0);
			CHECK(marker.Function == std::format("[truncated: {} more frames]", 100 - (MaxScriptErrorTracebackFrames - 1)));
			ScriptError bounded = added;
			BoundScriptError(bounded);
			CHECK(ScriptErrorToJson(bounded) == ScriptErrorToJson(added));
			deep.Traceback.resize(MaxScriptErrorTracebackFrames);
			deep.Message = "exactly at the cap";
			const ScriptError whole = errors.Add(deep);
			REQUIRE(whole.Traceback.size() == MaxScriptErrorTracebackFrames);
			CHECK(whole.Traceback.back().Function == "Descend");
			CHECK(whole.Traceback.back().Line == MaxScriptErrorTracebackFrames);
		}

		TEST_CASE("ScriptError: the stream evicts the oldest distinct error and reports it to older cursors")
		{
			ScriptErrorStream errors;
			const auto distinct = [](size_t index)
			{
				return ScriptError{ .Script = "Assets/Scripts/Spam.luau", .Line = 7, .Message = std::format("fault {}", index) };
			};
			// A repeat moves its error behind newer ones without any eviction.
			static_cast<void>(errors.Add(distinct(0)));
			static_cast<void>(errors.Add(distinct(1)));
			static_cast<void>(errors.Add(distinct(0)));
			CHECK(errors.GetErrors().size() == 2);
			CHECK(errors.GetEvictedCount() == 0);
			CHECK_FALSE(errors.HasEvictedAfter(0));
			for (size_t index = 2; index < MaxScriptErrorEntries + 5; ++index)
				static_cast<void>(errors.Add(distinct(index)));
			// IDs 1 and 3 belong to fault 0 and ID 2 to fault 1; IDs 4 to 6 to faults 2 to 4. Fault 1 (ID 2) was the oldest.
			const uint64_t added = MaxScriptErrorEntries + 6;
			CHECK(errors.GetCursor() == added);
			REQUIRE(errors.GetErrors().size() == MaxScriptErrorEntries);
			CHECK(errors.GetEvictedCount() == 5);
			CHECK(errors.GetErrors().front().ID == 7);
			CHECK(errors.GetErrors().front().Message == "fault 5");
			CHECK(errors.GetErrors().back().ID == added);
			// The evicted errors' latest occurrences are IDs 2 to 6: a cursor before 6 missed one of them.
			CHECK(errors.HasEvictedAfter(0));
			CHECK(errors.HasEvictedAfter(5));
			CHECK_FALSE(errors.HasEvictedAfter(6));
			CHECK_FALSE(errors.HasEvictedAfter(added));
			const auto page = errors.Read(0, 1000);
			REQUIRE(page.size() == MaxScriptErrorEntries);
			CHECK(page.front().ID == 7);
			CHECK(errors.Read(6, 1000).size() == MaxScriptErrorEntries);
			// A repeat of a retained error evicts nothing; a repeat of an evicted one starts a new count and evicts the oldest.
			const ScriptError retainedRepeat = errors.Add(distinct(5));
			CHECK(retainedRepeat.Count == 2);
			CHECK(errors.GetEvictedCount() == 5);
			const ScriptError evictedRepeat = errors.Add(distinct(1));
			CHECK(evictedRepeat.Count == 1);
			CHECK(errors.GetEvictedCount() == 6);
			CHECK(errors.GetErrors().size() == MaxScriptErrorEntries);
			CHECK(errors.GetErrors().front().Message == "fault 7");
			// Fault 6 went this time; its latest occurrence was ID 8, so only a cursor before 8 missed it.
			CHECK(errors.HasEvictedAfter(7));
			CHECK_FALSE(errors.HasEvictedAfter(8));
			CHECK(errors.GetCursor() == evictedRepeat.ID);
		}
	}

}
