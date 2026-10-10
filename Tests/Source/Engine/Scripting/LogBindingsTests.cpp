#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Core/Log.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <vector>

namespace Engine {

	namespace Test {

		void RunLogBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			const AssetHandle script(100);
			REQUIRE(fixture.AddScript(script, "Assets/Logger.luau", "return Script.Define(\"Logger\", {})").has_value());
			const Entity entity = fixture.GetScene().CreateEntity("Logger");
			entity.AddComponent<ScriptComponent>().Script = TypedAssetHandle<AssetType::Script>(script);
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
	}

}
