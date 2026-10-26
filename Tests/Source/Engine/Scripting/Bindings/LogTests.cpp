#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Test {

		void RunLogBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			const AssetHandle script(100);
			REQUIRE(fixture.AddScript(script, "Assets/Logger.luau", "return Script.Define(\"Logger\", {})").has_value());
			const Entity entity = fixture.GetScene().CreateEntity("Logger");
			entity.AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(script), .Fields = {}, .ExecutionOrder = 0 });
			fixture.Frame.Tick = 47;
			REQUIRE(fixture.Start().has_value());
			const auto path = VfsPath::Parse("project://Assets/LogProbe.luau");
			REQUIRE(path.has_value());
			const auto cursor = Log::GetRingBuffer().GetNextSeq();
			Test::ExpectLog expected(LogLevel::Error, "binding-log-error");
			const auto result = fixture.GetEngine()->Evaluate(
				"Log.Trace(\"binding-log-trace\")\n"
				"print(\"binding-log-print\", 7, true, nil)\n"
				"Log.Info(\"binding-log-info\", setmetatable({}, {__tostring = function() return \"custom\" end}))\n"
				"Log.Warn(\"binding-log-warn\")\n"
				"Log.Error(\"binding-log-error\")\n"
				"return true",
				*path, entity.GetUUID());
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(result->Prints == std::vector<std::string>{ "binding-log-print\t7\ttrue\tnil", "binding-log-info\tcustom" });
			const auto entries = Log::GetRingBuffer().Read({ .Cursor = cursor, .Channels = { LogChannel::Script }, .Contains = "binding-log-" });
			REQUIRE(entries.Entries.size() == 5);
			for (size_t index = 0; index < entries.Entries.size(); ++index)
			{
				const auto& entry = entries.Entries[index];
				CHECK(entry.EntityId == entity.GetUUID());
				CHECK(entry.Tick == 47);
				CHECK(entry.ScriptFile == "Assets/LogProbe.luau");
				CHECK(entry.ScriptLine == index + 1);
				CHECK(entry.File == "Assets/LogProbe.luau");
				CHECK(entry.Line == index + 1);
			}
			CHECK(entries.Entries[0].Level == LogLevel::Trace);
			CHECK(entries.Entries[1].Level == LogLevel::Info);
			CHECK(entries.Entries[3].Level == LogLevel::Warn);
			CHECK(entries.Entries[4].Level == LogLevel::Error);
			CHECK(fixture.Errors.empty());
			CHECK(fixture.ExternalMutations.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
				if (counter.Owner == "Log")
				{
					CAPTURE(counter.Member);
					CHECK(counter.Calls == (counter.Member == "Info" ? 2 : 1));
				}
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("LogBindings: levels print alias source context and coverage share one dispatcher")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunLogBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("LogBindings: read-only logging permits local formatting and rejects formatter faults atomically")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			REQUIRE(fixture.Start().has_value());
			const auto cursor = Log::GetRingBuffer().GetNextSeq();
			const auto result = fixture.Evaluate(R"(
Log.Info("binding-readonly", "ok")
local ok = pcall(Log.Info, "binding-partial", setmetatable({}, {__tostring = function() error("formatter fault") end}))
assert(not ok)
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(result->Prints == std::vector<std::string>{ "binding-readonly\tok" });
			CHECK(Log::GetRingBuffer().Read({ .Cursor = cursor, .Channels = {}, .Contains = "binding-partial" }).Entries.empty());
			CHECK(fixture.ExternalMutations.empty());
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("LogBindings: an entry keeps at most the message limit however many arguments alias one string")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			const auto cursor = Log::GetRingBuffer().GetNextSeq();
			// About 1 MiB of VM memory names a 7 GB message through 7,000 aliases of one string.
			const auto result = fixture.Evaluate("local s = string.rep('x', 2^20); local t = table.create(7000, s); print('binding-cap', table.unpack(t)); return true");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			const uint64_t total = std::string_view("binding-cap").size() + 7000ull * (1048576ull + 1ull);
			const std::string marker = std::format(" [truncated: {} bytes]", total);
			const std::string expected = "binding-cap\t" + std::string(MaxScriptLogMessageBytes - std::string_view("binding-cap\t").size() - marker.size(), 'x') + marker;
			CHECK(expected.size() == MaxScriptLogMessageBytes);
			REQUIRE(result->Prints.size() == 1);
			CHECK(result->Prints[0] == expected);
			const auto entries = Log::GetRingBuffer().Read({ .Cursor = cursor, .Channels = { LogChannel::Script }, .Contains = "binding-cap" });
			REQUIRE(entries.Entries.size() == 1);
			CHECK(entries.Entries[0].Message == expected);
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("LogBindings: the entry limit holds after private locations become authored ones")
		{
			// A script can learn its own private chunk identity from a caught error and repeat it: each short "engine/<n>:1:"
			// becomes a much longer authored location, so the cap must apply to the rewritten text.
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			const auto path = VfsPath::Parse("project://Assets/Scripts/Gameplay/Enemies/BossControllerStateMachine.luau");
			REQUIRE(path.has_value());
			const auto cursor = Log::GetRingBuffer().GetNextSeq();
			const auto result = fixture.GetEngine()->Evaluate(R"lua(
local ok, message = pcall(function() local t = nil; return t.x end)
local prefix = string.match(message, "^(engine/%d+:%d+:)")
assert(not ok and prefix ~= nil, message)
local count = math.floor(8184 / #prefix)
Log.Warn(string.rep(prefix, count))
print(string.rep(prefix, count))
return prefix
)lua",
				*path);
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			const auto prefix = JsonReader(result->Value.Get()).ReadString();
			REQUIRE(prefix.has_value());
			REQUIRE_FALSE(prefix->empty());
			const size_t written = 8184 / prefix->size() * prefix->size();
			CHECK(written <= MaxScriptLogMessageBytes);
			const std::string marker = std::format(" [truncated: {} bytes]", written);
			// print is Log.Info in a runtime VM, so the same text is a Warn and an Info entry.
			const auto entries = Log::GetRingBuffer().Read({ .Cursor = cursor, .Channels = { LogChannel::Script }, .Contains = "BossControllerStateMachine.luau:2:" });
			REQUIRE(entries.Entries.size() == 2);
			CHECK(entries.Entries[0].Level == LogLevel::Warn);
			CHECK(entries.Entries[1].Level == LogLevel::Info);
			CHECK(entries.Entries[1].Message == entries.Entries[0].Message);
			const std::string& entry = entries.Entries[0].Message;
			CHECK(entry.size() <= MaxScriptLogMessageBytes);
			CHECK(entry.starts_with("Assets/Scripts/Gameplay/Enemies/BossControllerStateMachine.luau:2:Assets/"));
			CHECK(entry.ends_with(marker));
			CHECK(entry.find("engine/") == std::string::npos);
			REQUIRE(result->Prints.size() == 1);
			CHECK(result->Prints[0] == entry);
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("LogBindings: entries beyond the per-frame limit are dropped counted and reported without affecting capture")
		{
			Test::ScriptTestFixture fixture;
			fixture.Frame.Tick = 3;
			fixture.Frame.Frame = 5;
			REQUIRE(fixture.Start().has_value());
			const std::string source = std::format("for i = 1, {} do print('binding-rate', i) end\nreturn true", MaxScriptLogEntriesPerFrame + 100);
			{
				Test::ExpectLog written(LogLevel::Info, "binding-rate");
				Test::ExpectLog limit(LogLevel::Warn, std::format("script log limit of {} entries per frame reached in tick 3 frame 5", MaxScriptLogEntriesPerFrame));
				const auto result = fixture.Evaluate(source);
				REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
				// Evaluation output keeps every line; only the log entries are limited.
				CHECK(result->Prints.size() == MaxScriptLogEntriesPerFrame + 100);
				CHECK(written.GetMatchCount() == MaxScriptLogEntriesPerFrame);
				CHECK(limit.GetMatchCount() == 1);
			}
			{
				// Still the same frame window: everything is dropped and counted.
				const auto cursor = Log::GetRingBuffer().GetNextSeq();
				const auto result = fixture.Evaluate("print('binding-rate', 'late'); return true");
				REQUIRE(result.has_value());
				REQUIRE(result->Prints.size() == 1);
				CHECK(Log::GetRingBuffer().Read({ .Cursor = cursor, .Channels = {}, .Contains = "binding-rate" }).Entries.empty());
			}
			// The next window reports the previous window's total before writing again.
			fixture.Frame.Frame = 6;
			Test::ExpectLog summary(LogLevel::Warn, "101 script log entries were dropped in tick 3 frame 5");
			Test::ExpectLog written(LogLevel::Info, "binding-rate fresh");
			const auto result = fixture.Evaluate("print('binding-rate fresh'); return true");
			REQUIRE(result.has_value());
			CHECK(summary.GetMatchCount() == 1);
			CHECK(written.GetMatchCount() == 1);
			CHECK(fixture.Errors.empty());
		}
	}

}
