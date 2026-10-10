#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Core/Log.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/TaskScheduler.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptEngine: each published error logs its context and traceback once before host copies")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle script(0x213001);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/LogOwner.luau", "return Script.Define('LogOwner', {})"));
			const Entity entity = fixture.GetScene().CreateEntity("Log owner");
			ScriptComponent component{};
			component.Script.SetHandle(script);
			entity.AddComponent<ScriptComponent>(component);
			REQUIRE(fixture.Start());
			fixture.Frame.Tick = 27;
			const auto path = VfsPath::Parse("project://Assets/Scripts/LogFault.luau");
			REQUIRE(path);
			const LogContext previous = LogContextScope::GetCurrent();
			const uint64_t cursor = Log::GetRingBuffer().GetNextSeq();
			Test::ExpectLog expected(LogLevel::Error, "published log fault");
			ScriptErrorStream copiedHistory;
			for (uint32_t occurrence = 0; occurrence < 2; ++occurrence)
			{
				const auto result = fixture.GetEngine()->Evaluate("local function fail() error('published log fault') end\nfail()", *path, entity.GetUUID());
				REQUIRE_FALSE(result);
				REQUIRE(fixture.Errors.size() == occurrence + 1);
				static_cast<void>(copiedHistory.Add(fixture.Errors.back()));
			}
			CHECK(expected.GetMatchCount() == 2);
			const auto entries = Log::GetRingBuffer().Read({ .Cursor = cursor, .MinimumLevel = LogLevel::Error, .Channels = { LogChannel::Script }, .Contains = {} });
			REQUIRE(entries.Entries.size() == 2);
			for (const auto& entry : entries.Entries)
			{
				CHECK(entry.EntityId == entity.GetUUID());
				CHECK(entry.Tick == std::optional<uint64_t>{ 27 });
				CHECK(entry.ScriptFile == "Assets/Scripts/LogFault.luau");
				CHECK(entry.ScriptLine == 1);
				CHECK(entry.File == entry.ScriptFile);
				CHECK(entry.Line == entry.ScriptLine);
				CHECK(entry.Message.find("Callback: Evaluate") != std::string::npos);
				CHECK(entry.Message.find("Entity: Log owner") != std::string::npos);
				CHECK(entry.Message.find("at Assets/Scripts/LogFault.luau:1") != std::string::npos);
			}
			REQUIRE(copiedHistory.GetErrors().size() == 1);
			CHECK(copiedHistory.GetErrors()[0].Count == 2);
			CHECK(LogContextScope::GetCurrent().Entity == previous.Entity);
			CHECK(LogContextScope::GetCurrent().Tick == previous.Tick);
			CHECK(LogContextScope::GetCurrent().ScriptFile == previous.ScriptFile);
			CHECK(LogContextScope::GetCurrent().ScriptLine == previous.ScriptLine);
		}

		TEST_CASE("ScriptEngine: independent engines keep tasks and VM identities isolated")
		{
			Test::ScriptTestFixture first;
			Test::ScriptTestFixture second;
			REQUIRE(first.Start());
			REQUIRE(second.Start());
			CHECK(first.GetSceneGeneration() == second.GetSceneGeneration());
			CHECK(first.GetEngine()->GetGeneration() != second.GetEngine()->GetGeneration());
			const auto firstTask = first.Evaluate("Task.Delay(0, function() Scene.CreateEntity('First') end); return nil");
			REQUIRE_MESSAGE(firstTask, (firstTask ? "" : firstTask.error().ToString()));
			const auto secondTask = second.Evaluate("Task.Delay(0, function() Scene.CreateEntity('Second') end); return nil");
			REQUIRE_MESSAGE(secondTask, (secondTask ? "" : secondTask.error().ToString()));
			CHECK(first.GetEngine()->GetTaskScheduler()->GetCount() == 1);
			CHECK(second.GetEngine()->GetTaskScheduler()->GetCount() == 1);
			first.GetEngine()->Stop();
			CHECK(first.GetEngine()->IsStopped());
			CHECK_FALSE(first.Evaluate("return 1"));
			second.Frame.Tick = 1;
			second.GetEngine()->ResumeTasks();
			CHECK(first.GetScene().GetEntityCount() == 0);
			CHECK(second.GetScene().GetEntityCount() == 1);
			CHECK(second.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			const auto result = second.Evaluate("return Scene.FindByName('Second') ~= nil");
			REQUIRE(result);
			CHECK(result->Value.Get() == Json(true));
			CHECK(first.Errors.empty());
			CHECK(second.Errors.empty());
		}
	}

}
