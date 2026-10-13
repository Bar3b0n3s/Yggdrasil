#include "TestsPCH.h"
#include "Engine/Scripting/LoadTimeVm.h"

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Asset/ScriptData.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		class ImportModuleReader final : public IScriptModuleReader
		{
		public:
			[[nodiscard]] Result<std::string> ReadModule(const VfsPath& path) override
			{
				Reads.push_back(path.ToString());
				if (OnRead)
					OnRead();
				const auto found = Sources.find(path.ToString());
				if (found == Sources.end())
					return MakeError(ErrorCode::NotFound, "module '{}' was not supplied", path);
				return found->second;
			}
		public:
			std::map<std::string, std::string> Sources{};
			std::vector<std::string> Reads{};
			std::function<void()> OnRead{};
		};

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("LoadTimeVm: a multi-file require graph imports within one budget")
		{
			ImportModuleReader reader;
			reader.Sources.emplace("project://Assets/Board.luau", "return { Data = require('./Shared') }");
			reader.Sources.emplace("project://Assets/Shared.luau", "return table.freeze({ Value = 7 })");
			auto path = VfsPath::Parse("project://Assets/Game.luau");
			REQUIRE(path.has_value());
			const ScriptCheckRequest request{
				.Path = *path,
				.Source = "local a = require('./Board'); local b = require('./Shared'); assert(a.Data == b); return {}",
				.Modules = &reader,
			};
			double now = 0.0;
			reader.OnRead = [&now]
			{
				now += 0.10;
			};
			LoadTimeVmSpecification specification{};
			specification.ClockSeconds = [&now]
			{
				return now;
			};
			const auto imported = LoadTimeVm::Extract(request, specification);
			REQUIRE_MESSAGE(imported.has_value(), (imported ? "" : imported.error().ToString()));
			REQUIRE(*imported != nullptr);
			CHECK((*imported)->Kind == ScriptKind::Module);
			CHECK((*imported)->Requires.size() == 3);
			CHECK(reader.Reads.size() == 2);
			CHECK(now == doctest::Approx(0.20));

			reader.Reads.clear();
			now = 0.0;
			reader.OnRead = [&now]
			{
				now += 0.15;
			};
			const auto timeout = LoadTimeVm::Extract(request, specification);
			REQUIRE_FALSE(timeout.has_value());
			CHECK(timeout.error().GetCode() == ErrorCode::Timeout);
			// Each read fits individually; only the cumulative 250 ms deadline rejects the second graph.
		}

		TEST_CASE("LoadTimeVm: engine API at module top fails with a located message")
		{
			ImportModuleReader reader;
			reader.Sources.emplace("project://Assets/Board.luau", "local board = {}\nInput.GetAxis('MoveX')\nreturn board");
			auto path = VfsPath::Parse("project://Assets/Game.luau");
			REQUIRE(path.has_value());
			const auto result = LoadTimeVm::Extract({ .Path = *path, .Source = "return require('./Board')", .Modules = &reader });
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Script);
			CHECK(result.error().GetLocation().File == "Assets/Board.luau");
			CHECK(result.error().GetLocation().Line == 2);
			CHECK(result.error().GetMessageText().find("Input.GetAxis") != std::string::npos);
			CHECK(result.error().GetMessageText().find("is not available at load time") != std::string::npos);
		}

		TEST_CASE("LoadTimeVm: classifies authenticated registrations and preserves Asset field descriptors")
		{
			ImportModuleReader reader;
			auto path = VfsPath::Parse("project://Assets/Ball.luau");
			REQUIRE(path.has_value());
			const auto behaviour = LoadTimeVm::Extract({ .Path = *path,
				.Source = "local Ball = { Fields = { Speed = Field.Number(3, { Min = 0, Max = 9 }) } }; return Script.Define('Ball', Ball)",
				.Modules = &reader });
			REQUIRE_MESSAGE(behaviour.has_value(), (behaviour ? "" : behaviour.error().ToString()));
			REQUIRE(*behaviour != nullptr);
			CHECK((*behaviour)->Kind == ScriptKind::Behaviour);
			CHECK((*behaviour)->Name == "Ball");
			REQUIRE((*behaviour)->Fields.size() == 1);
			CHECK((*behaviour)->Fields.front().Name == "Speed");
			CHECK((*behaviour)->Fields.front().Type == FieldType::Float);
			CHECK_FALSE((*behaviour)->Bytecode.empty());
			const auto ordinary = LoadTimeVm::Extract({ .Path = *path,
				.Source = "return { Name = 'Ball', Fields = { Speed = 3 }, OnStart = function() end }",
				.Modules = &reader });
			REQUIRE_MESSAGE(ordinary.has_value(), (ordinary ? "" : ordinary.error().ToString()));
			REQUIRE(*ordinary != nullptr);
			CHECK((*ordinary)->Kind == ScriptKind::Module);
			CHECK((*ordinary)->Fields.empty());
			const auto suite = LoadTimeVm::Extract({ .Path = *path,
				.Source = "return Test.Suite('Pure', function() error('suite body must not run during import') end)",
				.Modules = &reader });
			REQUIRE_MESSAGE(suite.has_value(), (suite ? "" : suite.error().ToString()));
			REQUIRE(*suite != nullptr);
			CHECK((*suite)->Kind == ScriptKind::TestSuite);
			CHECK((*suite)->Name == "Pure");
			CHECK((*suite)->Fields.empty());
		}

		TEST_CASE("LoadTimeVm: pure library imports are isolated and deterministic across invocations")
		{
			ImportModuleReader reader;
			auto path = VfsPath::Parse("project://Assets/Pure.luau");
			REQUIRE(path.has_value());
			const ScriptCheckRequest request{
				.Path = *path,
				.Source = "assert(string and table and bit32 and utf8 and buffer and vector and coroutine); "
						  "local C = { Fields = { Value = Field.Number(math.random()) } }; return Script.Define('Pure', C)",
				.Modules = &reader,
			};
			const auto first = LoadTimeVm::Extract(request);
			const auto second = LoadTimeVm::Extract(request);
			REQUIRE_MESSAGE(first.has_value(), (first ? "" : first.error().ToString()));
			REQUIRE_MESSAGE(second.has_value(), (second ? "" : second.error().ToString()));
			REQUIRE(*first != nullptr);
			REQUIRE(*second != nullptr);
			const auto firstCooked = CookScript(**first, 1);
			const auto secondCooked = CookScript(**second, 1);
			REQUIRE(firstCooked.has_value());
			REQUIRE(secondCooked.has_value());
			CHECK(*firstCooked == *secondCooked);
		}

		TEST_CASE("LoadTimeVm: rejects require cycles and missing readers without retaining a failed import")
		{
			ImportModuleReader reader;
			// Both modules are readable, so B's require('./A') reaches cycle detection rather than a missing-module error.
			reader.Sources.emplace("project://Assets/A.luau", "return require('./B')");
			reader.Sources.emplace("project://Assets/B.luau", "return require('./A')");
			auto path = VfsPath::Parse("project://Assets/A.luau");
			REQUIRE(path.has_value());
			const auto cycle = LoadTimeVm::Extract({ .Path = *path, .Source = "return require('./B')", .Modules = &reader });
			REQUIRE_FALSE(cycle.has_value());
			CHECK(cycle.error().GetCode() == ErrorCode::Script);
			const std::string message = cycle.error().ToString();
			CAPTURE(message);
			CHECK(message.find("require cycle: Assets/A.luau -> Assets/B.luau -> Assets/A.luau") != std::string::npos);
			CHECK(message.find("was not supplied") == std::string::npos);
			CHECK(std::count(reader.Reads.begin(), reader.Reads.end(), std::string("project://Assets/A.luau")) == 1);
			CHECK(LoadTimeVm::Extract({ .Path = *path, .Source = "return {}", .Modules = &reader }).has_value());
			const auto missing = LoadTimeVm::Extract({ .Path = *path, .Source = "return {}" });
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
