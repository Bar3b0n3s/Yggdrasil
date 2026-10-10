#include "TestsPCH.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <latch>
#include <map>
#include <thread>

namespace Engine {

	namespace Test {

		class CheckerModuleReader final : public IScriptModuleReader
		{
		public:
			std::map<VfsPath, std::string> Sources{};
			std::vector<VfsPath> Reads{};
			std::latch* Arrived = nullptr;
			std::latch* Proceed = nullptr;

			Result<std::string> ReadModule(const VfsPath& path) override
			{
				Reads.push_back(path);
				if (Arrived != nullptr)
				{
					Arrived->count_down();
					Proceed->wait();
					Arrived = nullptr;
				}
				if (const auto found = Sources.find(path); found != Sources.end())
					return found->second;
				return MakeError(ErrorCode::NotFound, "fixture module '{}' is missing", path);
			}
		};

		static Scope<ScriptTypeChecker> PrimitiveChecker(ScriptTypeCheckerConfiguration configuration = {})
		{
			TypeRegistry types;
			types.Freeze();
			ScriptApiRegistry api;
			REQUIRE(api.Freeze(types));
			auto checker = ScriptTypeChecker::Create(api, std::move(configuration));
			INFO((checker ? "checker initialized" : checker.error().GetMessageText()));
			REQUIRE(checker);
			return std::move(*checker);
		}

		static Scope<ScriptTypeChecker> EngineChecker(const TypeRegistry& types)
		{
			ScriptApiRegistry api;
			REQUIRE(RegisterBindings(api, types));
			REQUIRE(api.Freeze(types));
			auto checker = ScriptTypeChecker::Create(api);
			INFO((checker ? "checker initialized" : checker.error().GetMessageText()));
			REQUIRE(checker);
			return std::move(*checker);
		}

		static bool HasCheckerErrors(const std::vector<ScriptDiagnostic>& findings)
		{
			return std::any_of(findings.begin(), findings.end(), [](const auto& finding)
			{
				return finding.Severity == DiagnosticSeverity::Error;
			});
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptTypeChecker: strict findings have exclusive source ranges")
		{
			const auto checker = Test::PrimitiveChecker();
			Test::CheckerModuleReader scriptModuleSources;
			const auto findings = checker->CheckScript({ .Path = Test::ParseVfsPath("project://Assets/Bad.luau"),
				.Source = "local value: number = \"bad\"\nreturn value\n",
				.Modules = &scriptModuleSources });
			REQUIRE(findings.size() == 1);
			CHECK(findings[0].Severity == DiagnosticSeverity::Error);
			CHECK(findings[0].Code == "SCRIPT_TYPE_ERROR");
			CHECK(findings[0].File == "Assets/Bad.luau");
			CHECK(findings[0].Line == 1);
			CHECK(findings[0].Column == 23);
			CHECK(findings[0].EndLine == 1);
			CHECK(findings[0].EndColumn == 28);
		}

		TEST_CASE("TypeChecker: fixtures report expected diagnostics")
		{
			Test::AssetTestFixture fixture;
			const auto checker = Test::EngineChecker(fixture.GetRegistry());
			Test::CheckerModuleReader scriptModuleSources;
			const auto files = Test::ListTestDataFiles("TypeChecker", { ".luau" });
			REQUIRE(files);
			for (const auto& file : *files)
			{
				const auto source = Test::ReadTestDataText(file);
				REQUIRE(source);
				scriptModuleSources.Sources.emplace(Test::ParseVfsPath("project://Assets/" + file.substr(std::string("TypeChecker/").size())), *source);
			}
			const auto manifest = Test::ReadTestDataText("TypeChecker/Expected.json");
			REQUIRE(manifest);
			const auto parsed = JsonReader::Parse(*manifest);
			REQUIRE(parsed);
			const JsonReader root(*parsed);
			const auto count = root.GetArraySize();
			REQUIRE(count);
			for (size_t i = 0; i < *count; ++i)
			{
				const auto row = root.GetElement(i);
				REQUIRE(row);
				const auto path = row->ReadMember<std::string>("Path");
				const auto clean = row->ReadMember<bool>("Clean");
				REQUIRE(path);
				REQUIRE(clean);
				CAPTURE(*path);
				const auto vfsPath = Test::ParseVfsPath("project://Assets/" + *path);
				const auto source = scriptModuleSources.Sources.find(vfsPath);
				REQUIRE(source != scriptModuleSources.Sources.end());
				const auto findings = checker->CheckScript({ vfsPath, source->second, &scriptModuleSources });
				CHECK(Test::HasCheckerErrors(findings) == !*clean);
				for (const auto& finding : findings)
				{
					CHECK(finding.File.starts_with("Assets/"));
					CHECK(finding.Line > 0);
					CHECK(finding.Column > 0);
					CHECK_FALSE(finding.Message.empty());
				}
				if (const auto ranges = row->FindMember("Ranges"))
				{
					const auto size = ranges->GetArraySize();
					REQUIRE(size);
					REQUIRE(findings.size() == *size);
					for (size_t r = 0; r < *size; ++r)
					{
						const auto expected = ranges->GetElement(r);
						REQUIRE(expected);
						const std::array actual{ findings[r].Line, findings[r].Column, findings[r].EndLine, findings[r].EndColumn };
						for (size_t p = 0; p < actual.size(); ++p)
						{
							const auto coordinate = expected->GetElement(p);
							REQUIRE(coordinate);
							CHECK(coordinate->ReadUInt32() == actual[p]);
						}
					}
				}
			}
		}

		TEST_CASE("ScriptTypeChecker: bounds APIs generate optional multiple returns accepted by the New solver")
		{
			Test::AssetTestFixture fixture;
			const auto checker = Test::EngineChecker(fixture.GetRegistry());
			Test::CheckerModuleReader scriptModuleSources;
			const auto source = Test::ReadTestDataText("TypeChecker/Bounds.luau");
			REQUIRE(source);
			CHECK_FALSE(Test::HasCheckerErrors(checker->CheckScript({ fixture.ProjectPath("Assets/Bounds.luau"), *source, &scriptModuleSources })));
			for (const std::string call : { "entity:GetWorldBounds()", "Physics.GetBodyBounds(entity)", "Physics.GetColliderBounds(entity)" })
			{
				const std::string invalid = "local function bad(entity: Entity)\nlocal a, b = " + call + "\nreturn b - a\nend\nreturn bad";
				const auto findings = checker->CheckScript({ fixture.ProjectPath("Assets/Bad.luau"), invalid, &scriptModuleSources });
				CHECK(Test::HasCheckerErrors(findings));
				CHECK(std::any_of(findings.begin(), findings.end(), [](const auto& finding)
				{
					return finding.Line == 3;
				}));
			}
		}

		TEST_CASE("ScriptTypeChecker: generated definitions are copied from a frozen registry")
		{
			ScriptApiRegistry unfrozen;
			const auto rejected = ScriptTypeChecker::Create(unfrozen);
			REQUIRE_FALSE(rejected);
			CHECK(rejected.error().GetCode() == ErrorCode::InvalidState);
			// Both temporary registries die inside PrimitiveChecker; definitions/configuration must be owned.
			const auto checker = Test::PrimitiveChecker();
			Test::CheckerModuleReader scriptModuleSources;
			CHECK_FALSE(Test::HasCheckerErrors(checker->CheckScript({ Test::ParseVfsPath("project://Assets/Good.luau"),
				"local value: number = 42\nreturn value", &scriptModuleSources })));
			CHECK(checker->GetEnvironmentHash() == Test::PrimitiveChecker()->GetEnvironmentHash());
		}

		TEST_CASE("ScriptTypeChecker: configuration snapshots distinguish missing present and inherited configs")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Nested/Probe.luau", "return {}");
			const auto absent = ScriptTypeChecker::CaptureConfiguration(fixture.GetVfs());
			REQUIRE(absent);
			REQUIRE(absent->Files.size() == 3);
			const auto first = Test::PrimitiveChecker(*absent);
			fixture.WriteProjectText(".luaurc", "{}");
			const auto empty = ScriptTypeChecker::CaptureConfiguration(fixture.GetVfs());
			REQUIRE(empty);
			const auto second = Test::PrimitiveChecker(*empty);
			CHECK(first->GetEnvironmentHash() != second->GetEnvironmentHash());
			fixture.WriteProjectText(".luaurc", "{\"languageMode\":\"strict\"}");
			fixture.WriteProjectText("Assets/Nested/.luaurc", "{\"languageMode\":\"nocheck\"}");
			const auto inherited = ScriptTypeChecker::CaptureConfiguration(fixture.GetVfs());
			REQUIRE(inherited);
			const auto checker = Test::PrimitiveChecker(*inherited);
			Test::CheckerModuleReader scriptModuleSources;
			const std::string source = "local n: number = 'wrong'\nreturn n";
			CHECK(Test::HasCheckerErrors(checker->CheckScript({ fixture.ProjectPath("Assets/Probe.luau"), source, &scriptModuleSources })));
			CHECK_FALSE(Test::HasCheckerErrors(checker->CheckScript({ fixture.ProjectPath("Assets/Nested/Probe.luau"), source, &scriptModuleSources })));
			CHECK(Test::HasCheckerErrors(checker->CheckScript({ fixture.ProjectPath("Assets/Nested/Probe.luau"), "--!strict\n" + source, &scriptModuleSources })));
			fixture.WriteProjectText("Assets/Nested/.luaurc", "{bad config");
			const auto broken = ScriptTypeChecker::CaptureConfiguration(fixture.GetVfs());
			REQUIRE(broken);
			const auto malformed = Test::PrimitiveChecker(*broken);
			const auto findings = malformed->CheckScript({ fixture.ProjectPath("Assets/Nested/Probe.luau"), "return {}", &scriptModuleSources });
			REQUIRE(Test::HasCheckerErrors(findings));
			CHECK(std::any_of(findings.begin(), findings.end(), [](const auto& finding)
			{
				return finding.File == "Assets/Nested/.luaurc";
			}));
			const auto again = ScriptTypeChecker::CaptureConfiguration(fixture.GetVfs());
			REQUIRE(again);
			CHECK(malformed->GetEnvironmentHash() == Test::PrimitiveChecker(*again)->GetEnvironmentHash());
			CHECK(first->GetEnvironmentHash() == Test::PrimitiveChecker(*absent)->GetEnvironmentHash());
			REQUIRE(fixture.GetVfs().Remove(fixture.ProjectPath("Assets/Nested/.luaurc")));
			const auto removed = ScriptTypeChecker::CaptureConfiguration(fixture.GetVfs());
			REQUIRE(removed);
			CHECK(malformed->GetEnvironmentHash() != Test::PrimitiveChecker(*removed)->GetEnvironmentHash());
			REQUIRE(fixture.GetVfs().CreateDirectories(fixture.ProjectPath("Assets/NewDirectory")));
			const auto newDirectory = ScriptTypeChecker::CaptureConfiguration(fixture.GetVfs());
			REQUIRE(newDirectory);
			CHECK(Test::PrimitiveChecker(*removed)->GetEnvironmentHash() != Test::PrimitiveChecker(*newDirectory)->GetEnvironmentHash());
		}

		TEST_CASE("ScriptTypeChecker: unsaved source and required modules use the supplied reader only")
		{
			const auto checker = Test::PrimitiveChecker();
			Test::CheckerModuleReader scriptModuleSources;
			const auto root = Test::ParseVfsPath("project://Assets/Root.luau");
			const auto dependency = Test::ParseVfsPath("project://Assets/Dependency.luau");
			scriptModuleSources.Sources[root] = "local stale: number = 'wrong'; return stale";
			scriptModuleSources.Sources[dependency] = "local bad: number = 'wrong'; return bad";
			const auto errors = checker->CheckScript({ root, "return require('./Dependency')", &scriptModuleSources });
			REQUIRE(Test::HasCheckerErrors(errors));
			CHECK(std::any_of(errors.begin(), errors.end(), [](const auto& finding)
			{
				return finding.File == "Assets/Dependency.luau";
			}));
			CHECK(scriptModuleSources.Reads == std::vector<VfsPath>{ dependency });
			scriptModuleSources.Sources[dependency] = "return 42";
			scriptModuleSources.Reads.clear();
			CHECK_FALSE(Test::HasCheckerErrors(checker->CheckScript({ root, "return require('./Dependency')", &scriptModuleSources })));
			CHECK(scriptModuleSources.Reads == std::vector<VfsPath>{ dependency });
			scriptModuleSources.Sources[dependency] = "return require('./Root')";
			CHECK(Test::HasCheckerErrors(checker->CheckScript({ root, "return require('./Dependency')", &scriptModuleSources })));
			for (const std::string request : { "'./Missing'", "'../../Escape'", "'@alias/File'" })
			{
				scriptModuleSources.Reads.clear();
				CHECK(Test::HasCheckerErrors(checker->CheckScript({ root, "return require(" + request + ")", &scriptModuleSources })));
				if (request != "'./Missing'")
					CHECK(scriptModuleSources.Reads.empty());
			}
		}

		TEST_CASE("ScriptTypeChecker: simultaneous checks are deterministic and failures never appear clean")
		{
			const auto checker = Test::PrimitiveChecker();
			const auto root = Test::ParseVfsPath("project://Assets/Root.luau");
			const auto dependency = Test::ParseVfsPath("project://Assets/Dependency.luau");
			std::array<Test::CheckerModuleReader, 2> scriptModuleSources;
			std::array<std::vector<ScriptDiagnostic>, 2> results;
			std::latch arrived(2);
			std::latch proceed(1);
			for (auto& reader : scriptModuleSources)
			{
				reader.Sources[dependency] = "local value: number = 'bad'; return value";
				reader.Arrived = &arrived;
				reader.Proceed = &proceed;
			}
			const auto check = [&checker, &root, &scriptModuleSources, &results](size_t index)
			{
				results[index] = checker->CheckScript({ root, "return require('./Dependency')", &scriptModuleSources[index] });
				// An early analysis failure must release the coordinating thread too; Reads below proves the overlap.
				if (scriptModuleSources[index].Arrived != nullptr)
				{
					scriptModuleSources[index].Arrived->count_down();
					scriptModuleSources[index].Arrived = nullptr;
				}
			};
			std::jthread first(check, 0);
			std::jthread second(check, 1);
			arrived.wait();
			proceed.count_down();
			first.join();
			second.join();
			CHECK(scriptModuleSources[0].Reads.size() == 1);
			CHECK(scriptModuleSources[1].Reads.size() == 1);
			CHECK(Test::HasCheckerErrors(results[0]));
			CHECK(results[0] == results[1]);
			CHECK(results[0] == checker->CheckScript({ root, "return require('./Dependency')", &scriptModuleSources[0] }));
			CHECK(Test::HasCheckerErrors(checker->CheckScript({ root, "return {}", nullptr })));
			CHECK(Test::HasCheckerErrors(checker->CheckScript({ root, "return )", &scriptModuleSources[0] })));
			CHECK_FALSE(Test::HasCheckerErrors(checker->CheckScript({ root, "return 1", &scriptModuleSources[0] })));
		}
	}

}
