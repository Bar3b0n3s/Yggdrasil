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
#include <format>
#include <latch>
#include <map>
#include <set>
#include <string>
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
			std::set<std::string> unlisted;
			for (const auto& file : *files)
			{
				const auto source = Test::ReadTestDataText(file);
				REQUIRE(source);
				const std::string relative = file.substr(std::string("TypeChecker/").size());
				scriptModuleSources.Sources.emplace(Test::ParseVfsPath("project://Assets/" + relative), *source);
				unlisted.insert(relative);
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
				unlisted.erase(*path);
				const auto vfsPath = Test::ParseVfsPath("project://Assets/" + *path);
				const auto source = scriptModuleSources.Sources.find(vfsPath);
				REQUIRE(source != scriptModuleSources.Sources.end());
				const auto findings = checker->CheckScript({ vfsPath, source->second, &scriptModuleSources });
				std::string actual;
				for (const auto& finding : findings)
					actual += std::format("{} {}:{}:{}-{}:{} {}\n", finding.Code, finding.File, finding.Line, finding.Column, finding.EndLine, finding.EndColumn, finding.Message);
				INFO(actual);
				CHECK(Test::HasCheckerErrors(findings) == !*clean);
				for (const auto& finding : findings)
				{
					CHECK(finding.File.starts_with("Assets/"));
					CHECK(finding.Line > 0);
					CHECK(finding.Column > 0);
					CHECK_FALSE(finding.Message.empty());
				}
				// Every seeded mistake pins its exact findings, so a fixture failing for an unrelated reason cannot pass.
				const auto expected = row->FindMember("Diagnostics");
				REQUIRE(expected.has_value() == !*clean);
				if (!expected)
					continue;
				const auto size = expected->GetArraySize();
				REQUIRE(size);
				REQUIRE(findings.size() == *size);
				for (size_t d = 0; d < *size; ++d)
				{
					const auto diagnostic = expected->GetElement(d);
					REQUIRE(diagnostic);
					CAPTURE(d);
					CHECK(diagnostic->ReadMember<std::string>("Code") == findings[d].Code);
					CHECK(diagnostic->ReadMember<std::string>("File") == findings[d].File);
					const auto range = diagnostic->FindMember("Range");
					REQUIRE(range);
					const std::array actualRange{ findings[d].Line, findings[d].Column, findings[d].EndLine, findings[d].EndColumn };
					for (size_t p = 0; p < actualRange.size(); ++p)
					{
						const auto coordinate = range->GetElement(p);
						REQUIRE(coordinate);
						CHECK(coordinate->ReadUInt32() == actualRange[p]);
					}
					const auto message = diagnostic->ReadMember<std::string>("MessageContains");
					REQUIRE(message);
					CHECK(findings[d].Message.find(*message) != std::string::npos);
				}
			}
			// A fixture missing from the manifest would never be checked.
			CHECK(unlisted.empty());
		}

		TEST_CASE("ScriptTypeChecker: syntax errors are compile findings, also in required modules")
		{
			const auto checker = Test::PrimitiveChecker();
			Test::CheckerModuleReader scriptModuleSources;
			const auto root = Test::ParseVfsPath("project://Assets/Root.luau");
			const auto findings = checker->CheckScript({ root, "-- first\nreturn )\n", &scriptModuleSources });
			REQUIRE(findings.size() == 1);
			CHECK(findings[0].Code == "SCRIPT_COMPILE_ERROR");
			CHECK(findings[0].File == "Assets/Root.luau");
			CHECK(findings[0].Line == 2);
			CHECK(findings[0].Column == 8);
			scriptModuleSources.Sources[Test::ParseVfsPath("project://Assets/Broken.luau")] = "return )";
			const auto required = checker->CheckScript({ root, "return require('./Broken')", &scriptModuleSources });
			CHECK(std::ranges::any_of(required, [](const ScriptDiagnostic& finding)
			{
				return finding.Code == "SCRIPT_COMPILE_ERROR" && finding.File == "Assets/Broken.luau";
			}));
			// Type errors keep their own code.
			const auto typed = checker->CheckScript({ root, "local value: number = 'wrong'\nreturn value", &scriptModuleSources });
			REQUIRE(typed.size() == 1);
			CHECK(typed[0].Code == "SCRIPT_TYPE_ERROR");
		}

		TEST_CASE("ScriptTypeChecker: nested configurations override their parents whatever the directory names")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/.luaurc", "{\"languageMode\":\"strict\"}");
			fixture.WriteProjectText("Assets/Probe.luau", "return {}");
			// '-', '(' and the like sort below '.', so these children's .luaurc precede their parent's in path order.
			const std::array directories{ std::string("-Prototypes"), std::string("(WIP)"), std::string("Nested") };
			for (const auto& directory : directories)
			{
				fixture.WriteProjectText("Assets/" + directory + "/.luaurc", "{\"languageMode\":\"nocheck\"}");
				fixture.WriteProjectText("Assets/" + directory + "/Probe.luau", "return {}");
			}
			const auto configuration = ScriptTypeChecker::CaptureConfiguration(fixture.GetVfs());
			REQUIRE(configuration);
			const auto checker = Test::PrimitiveChecker(*configuration);
			Test::CheckerModuleReader scriptModuleSources;
			const std::string source = "local n: number = 'wrong'\nreturn n";
			CHECK(Test::HasCheckerErrors(checker->CheckScript({ fixture.ProjectPath("Assets/Probe.luau"), source, &scriptModuleSources })));
			for (const auto& directory : directories)
			{
				CAPTURE(directory);
				CHECK_FALSE(Test::HasCheckerErrors(checker->CheckScript({ fixture.ProjectPath("Assets/" + directory + "/Probe.luau"), source, &scriptModuleSources })));
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

		// ADR 0022 "Integration": the checker used Luau's one-second wall-clock module limit, so a long script on a loaded
		// machine reported "script analysis exceeded its safety limit" as a type error and blocked test discovery. Analysis
		// now has no time limit: a module whose analysis takes several seconds in Debug (well over the old limit) reports
		// exactly its real finding, and the same findings every time.
		TEST_CASE("ScriptTypeChecker: findings do not depend on how long the analysis takes")
		{
			const auto checker = Test::PrimitiveChecker();
			constexpr int Functions = 600;
			std::string source = "--!strict\nlocal M = {}\ntype Point = { X: number, Y: number, Z: vector }\n";
			for (int i = 0; i < Functions; ++i)
			{
				source += std::format("function M.F{0}(a: Point, b: number): Point\n\tlocal t = {{ X = a.X + b, Y = a.Y * {0}, Z = a.Z + vector.create(b, {0}, 1) }}\n"
									  "\tif t.X > {0} then return M.F{1}(t, b - 1) end\n\treturn t\nend\n",
					i, i > 0 ? i - 1 : 0);
			}
			const uint32_t errorLine = 3 + 5 * Functions + 1;
			source += "local wrong: number = \"text\"\nM.Wrong = wrong\nreturn M\n";
			Test::CheckerModuleReader reader;
			const auto path = Test::ParseVfsPath("project://Assets/Long.luau");
			const auto findings = checker->CheckScript({ path, source, &reader });
			REQUIRE(findings.size() == 1);
			CHECK(findings[0].Code == "SCRIPT_TYPE_ERROR");
			CHECK(findings[0].Line == errorLine);
			CHECK(findings[0].Message.find("safety limit") == std::string::npos);
			CHECK(checker->CheckScript({ path, source, &reader }) == findings);
		}

		// Review finding M23 (ADR 0022 "Integration"): Luau evaluates a user-defined type function's body in its analysis VM,
		// whose only interrupt checks a time limit or a cancellation token, and the checker sets neither (findings must not
		// depend on machine load), so a looping body never returned and hung every import, script.check and test run. The
		// checker refuses type functions before analysis, at each declaration, in the root and in required modules. Without
		// the refusal the first check below never returns, and the case's timeout ends the run instead of a hang.
		TEST_CASE("ScriptTypeChecker: a user-defined type function is refused before analysis, so a looping one cannot hang a check"
			* doctest::timeout(120.0))
		{
			const auto checker = Test::PrimitiveChecker();
			Test::CheckerModuleReader reader;
			const auto root = Test::ParseVfsPath("project://Assets/Spin.luau");
			const auto looping = checker->CheckScript({ root,
				"--!strict\ntype function Spin(t)\n\twhile true do end\n\treturn t\nend\nlocal value: Spin<number> = 1\nreturn value\n", &reader });
			REQUIRE(looping.size() == 1);
			CHECK(looping[0].Severity == DiagnosticSeverity::Error);
			CHECK(looping[0].Code == "SCRIPT_TYPE_ERROR");
			CHECK(looping[0].File == "Assets/Spin.luau");
			CHECK(looping[0].Line == 2);
			CHECK(looping[0].Column == 15);
			CHECK(looping[0].EndColumn == 19);
			CHECK(looping[0].Message.contains("type function 'Spin' is not supported"));

			// A body that would raise when evaluated is refused without running, and syntax errors are still reported.
			const auto raising = checker->CheckScript({ root, "--!strict\ntype function Boom(t)\n\terror(\"evaluated\")\nend\nreturn )\n", &reader });
			REQUIRE(raising.size() == 2);
			CHECK(raising[0].Code == "SCRIPT_TYPE_ERROR");
			CHECK(raising[0].Line == 2);
			CHECK_FALSE(raising[0].Message.contains("evaluated"));
			CHECK(raising[1].Code == "SCRIPT_COMPILE_ERROR");
			CHECK(raising[1].Line == 5);

			// An exported type function in a required module refuses that module, so the frontend never evaluates it.
			const auto types = Test::ParseVfsPath("project://Assets/Types.luau");
			reader.Sources[types] = "--!strict\nexport type function Spin(t)\n\twhile true do end\n\treturn t\nend\nreturn {}\n";
			const auto required = checker->CheckScript({ root, "--!strict\nlocal Types = require(\"./Types\")\nreturn Types\n", &reader });
			CHECK(Test::HasCheckerErrors(required));
			CHECK(std::ranges::any_of(required, [](const ScriptDiagnostic& finding)
			{
				return finding.File == "Assets/Types.luau" && finding.Code == "SCRIPT_TYPE_ERROR" && finding.Line == 2
					&& finding.Message.contains("type function 'Spin' is not supported");
			}));

			// A plain type alias is unaffected.
			CHECK(checker->CheckScript({ root, "--!strict\ntype Pair = { number }\nlocal pair: Pair = { 1 }\nreturn pair\n", &reader }).empty());
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
