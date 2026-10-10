#include "TestsPCH.h"

#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/AssetCache.h"
#include "Engine/AssetPipeline/Importers/ScriptImporter.h"
#include "Engine/Core/Hash.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <functional>
#include <latch>
#include <thread>

namespace Engine {

	namespace Test {

		class ImportCheckProvider final : public IScriptDiagnosticsProvider
		{
		public:
			uint64_t Fingerprint = 17;
			std::function<std::vector<ScriptDiagnostic>(const ScriptCheckRequest&)> Check{};
			uint64_t GetEnvironmentHash() const override { return Fingerprint; }
			std::vector<ScriptDiagnostic> CheckScript(const ScriptCheckRequest& request) override
			{
				return Check ? Check(request) : std::vector<ScriptDiagnostic>{};
			}
		};

		static AssetMetadata ScriptMetadata()
		{
			return { .Handle = AssetHandle(0x4001), .Type = AssetType::Script, .Importer = "Script", .ImporterVersion = ScriptImporter::Version };
		}

		static ScriptData ImportScriptSource(AssetTestFixture& fixture, std::string_view source)
		{
			ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Probe.test.luau"), .SourceBytes = AsBytes(source), .Registry = &fixture.GetRegistry() });
			ScriptImporter importer;
			const auto imported = importer.Import(context, ScriptMetadata());
			REQUIRE(imported);
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts[0].Handle == ScriptMetadata().Handle);
			const auto loaded = LoadCookedScript(imported->Artifacts[0].Cooked);
			REQUIRE(loaded);
			const auto check = context.GetScriptCheck();
			REQUIRE(check);
			CHECK_FALSE(check->Performed);
			CHECK(check->SourceHash == XXH64(source));
			return **loaded;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("ScriptImporter: source bytes are authoritative and kinds are independent of filenames")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Probe.test.luau", "error('stale disk source')");
			CHECK(Test::ImportScriptSource(fixture, "return { Value = 42 }").Kind == ScriptKind::Module);
			CHECK(Test::ImportScriptSource(fixture, "local C = {}; return Script.Define('Probe', C)").Kind == ScriptKind::Behaviour);
			CHECK(Test::ImportScriptSource(fixture, "return Test.Suite('Suite', function() end)").Kind == ScriptKind::TestSuite);
			const auto fieldFixture = Test::ReadTestDataText("TypeChecker/Fields.luau");
			REQUIRE(fieldFixture);
			const auto allFields = Test::ImportScriptSource(fixture, *fieldFixture);
			REQUIRE(allFields.Fields.size() == 11);
			for (const auto& field : allFields.Fields)
			{
				switch (field.Type)
				{
					case FieldType::Float:
					case FieldType::Int32:     CHECK(field.DefaultValue.Get() == 0); break;
					case FieldType::Bool:      CHECK(field.DefaultValue.Get() == false); break;
					case FieldType::String:    CHECK(field.DefaultValue.Get() == ""); break;
					case FieldType::Vec3:      CHECK(field.DefaultValue.Get() == Json::array({ 0, 0, 0 })); break;
					case FieldType::Color4:    CHECK(field.DefaultValue.Get() == Json::array({ 1, 1, 1, 1 })); break;
					case FieldType::Quat:      CHECK(field.DefaultValue.Get() == Json::array({ 0, 0, 0, 1 })); break;
					case FieldType::EntityRef:
					case FieldType::AssetRef:  CHECK(field.DefaultValue.IsNull()); break;
					case FieldType::Enum:      CHECK(field.DefaultValue.Get() == "First"); break;
					case FieldType::Array:
						CHECK(field.DefaultValue.Get() == Json::array());
						REQUIRE(field.Element);
						REQUIRE(field.Element->Element);
						CHECK(field.Element->Element->Meta.Max == 1.0);
						break;
					default: CHECK(false); break;
				}
			}
			ScriptImporter importer;
			CHECK_FALSE(importer.RequiresMainThread());
			CHECK(importer.CanImport(".luau"));
			const auto files = importer.ListDependencyFiles(AsBytes("return {}"), fixture.ProjectPath("Assets/Probe.luau"));
			REQUIRE(files);
			CHECK(files->empty());
		}

		TEST_CASE("ScriptImporter: Board.luau change re-extracts Game.luau")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Leaf.luau", "return { Default = 1 }");
			fixture.WriteProjectText("Assets/Board.luau", "return require('./Leaf')");
			fixture.WriteProjectText("Assets/Game.luau",
				"local Board = require('./Board'); local C = { Fields = { Speed = Field.Number(Board.Default) } }; return Script.Define('Game', C)");
			fixture.OpenProject();
			const auto root = fixture.GetManager().Resolve("Assets/Game.luau");
			const auto board = fixture.GetManager().Resolve("Assets/Board.luau");
			REQUIRE(root);
			REQUIRE(board);
			const auto first = fixture.GetManager().Load(*root);
			REQUIRE(first);
			const auto firstScript = AssetCast<ScriptData>(*first);
			REQUIRE(firstScript);
			REQUIRE(firstScript->Fields.size() == 1);
			CHECK(firstScript->Fields[0].DefaultValue.Get() == 1);
			fixture.WriteProjectText("Assets/Leaf.luau", "return { Default = 2 }");
			REQUIRE(fixture.GetManager().Refresh());
			fixture.GetManager().WaitIdle();
			static_cast<void>(fixture.GetMainThreadQueue().Drain());
			const auto second = fixture.GetManager().Load(*root);
			REQUIRE(second);
			const auto secondScript = AssetCast<ScriptData>(*second);
			REQUIRE(secondScript);
			CHECK(secondScript->Fields[0].DefaultValue.Get() == 2);
			CHECK(firstScript->Fields[0].DefaultValue.Get() == 1);
			CHECK(fixture.GetManager().Resolve("Assets/Board.luau") == board);
			CHECK(fixture.GetManager().GetMetadata(*board)->Kind == AssetMetaKind::Asset);
			const auto warm = fixture.GetManager().Load(*root);
			REQUIRE(warm);
			CHECK(AssetCast<ScriptData>(*warm)->Fields[0].DefaultValue.Get() == 2);
		}

		TEST_CASE("ScriptImporter: require reads and asset edges share one immutable source view")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Board.luau", "return { Default = 1 }");
			fixture.WriteProjectText("Assets/Delayed.luau", "return {}");
			const std::array assets{
				ImportAssetLookupEntry{ .SourcePath = fixture.ProjectPath("Assets/Board.luau"), .Handle = AssetHandle(0x4002), .Type = AssetType::Script },
				ImportAssetLookupEntry{ .SourcePath = fixture.ProjectPath("Assets/Delayed.luau"), .Handle = AssetHandle(0x4003), .Type = AssetType::Script }
			};
			Test::ImportCheckProvider provider;
			provider.Check = [&fixture](const ScriptCheckRequest& request)
			{
				REQUIRE(request.Modules->ReadModule(fixture.ProjectPath("Assets/Board.luau")));
				REQUIRE(request.Modules->ReadModule(fixture.ProjectPath("Assets/Delayed.luau")));
				fixture.WriteProjectText("Assets/Board.luau", "return { Default = 999 }");
				const auto cached = request.Modules->ReadModule(fixture.ProjectPath("Assets/Board.luau"));
				REQUIRE(cached);
				CHECK(*cached == "return { Default = 1 }");
				return std::vector<ScriptDiagnostic>{};
			};
			const std::string source = "local M = require('./Board'); local C = { Fields = { Speed = Field.Number(M.Default) } }; return Script.Define('Game', C)";
			ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Game.luau"), .SourceBytes = AsBytes(source), .Registry = &fixture.GetRegistry(), .Assets = assets, .ScriptDiagnostics = &provider });
			const auto imported = ScriptImporter().Import(context, Test::ScriptMetadata());
			REQUIRE(imported);
			CHECK(imported->Dependencies == std::vector<AssetHandle>{ AssetHandle(0x4002), AssetHandle(0x4003) });
			CHECK(context.GetDependencyReads().size() == 2);
			CHECK(context.GetLookups().size() == 2);
			const auto loaded = LoadCookedScript(imported->Artifacts[0].Cooked);
			REQUIRE(loaded);
			CHECK((*loaded)->Fields[0].DefaultValue.Get() == 1);
			REQUIRE((*loaded)->Requires.size() == 1);
			CHECK((*loaded)->Requires[0].Handle == AssetHandle(0x4002));
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), context.GetDependencyReads(), context.GetLookups(), assets));
		}

		TEST_CASE("ScriptImporter: type errors cook but remain blocking findings and absence is unchecked")
		{
			Test::AssetTestFixture fixture;
			Test::ImportCheckProvider provider;
			provider.Check = [](const ScriptCheckRequest&)
			{
				return std::vector<ScriptDiagnostic>{
					{ .File = "Assets/Probe.luau", .Line = 1, .Column = 2, .Message = "type mismatch", .EndLine = 1, .EndColumn = 8 },
					{ .File = "Assets/Required.luau", .Line = 4, .Column = 3, .Message = "nested mismatch", .EndLine = 4, .EndColumn = 9 }
				};
			};
			const std::string source = "return {}";
			ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Probe.luau"), .SourceBytes = AsBytes(source), .Registry = &fixture.GetRegistry(), .ScriptDiagnostics = &provider });
			const auto imported = ScriptImporter().Import(context, Test::ScriptMetadata());
			REQUIRE(imported);
			const auto check = context.GetScriptCheck();
			REQUIRE(check);
			CHECK(check->Performed);
			REQUIRE(check->Diagnostics.size() == 2);
			CHECK(check->Diagnostics[0].EndColumn == 8);
			CHECK(check->EnvironmentHash == 17);
			CHECK(IsScriptCheckCurrent(check, &provider));
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://"));
			CachedImport entry{ .Import = *imported, .Reads = context.GetDependencyReads(), .Lookups = context.GetLookups(), .ScriptCheck = check };
			REQUIRE(cache.Store(Test::ScriptMetadata().Handle, 123, entry));
			const auto restored = cache.Find(Test::ScriptMetadata().Handle, 123);
			REQUIRE(restored);
			REQUIRE(restored->has_value());
			REQUIRE((**restored).ScriptCheck);
			CHECK((**restored).ScriptCheck->Diagnostics == check->Diagnostics);
			provider.Fingerprint = 18;
			CHECK_FALSE(IsScriptCheckCurrent(check, &provider));
			CHECK_FALSE(IsScriptCheckCurrent(std::nullopt, &provider));
			CHECK(IsScriptCheckCurrent(std::nullopt, nullptr));
		}

		TEST_CASE("ScriptImporter: failed extraction preserves the previous asset and reports located errors")
		{
			Test::AssetTestFixture fixture;
			Test::ImportCheckProvider provider;
			provider.Check = [](const ScriptCheckRequest& request)
			{
				return std::vector<ScriptDiagnostic>{ { .File = std::string(request.Path.GetPath()), .Line = 1, .Column = 1, .Message = "latest attempt" } };
			};
			for (const std::string source : { "return )", "return require('./Missing')", "return Input.GetAxis('Move')",
					 "local C={Fields={Bad=Field.Number(0/0)}}; return Script.Define('Bad', C)" })
			{
				ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Probe.luau"), .SourceBytes = AsBytes(source), .Registry = &fixture.GetRegistry(), .ScriptDiagnostics = &provider });
				const auto result = ScriptImporter().Import(context, Test::ScriptMetadata());
				REQUIRE_FALSE(result);
				CHECK_FALSE(result.error().GetLocation().File.empty());
				const auto check = context.GetScriptCheck();
				REQUIRE(check);
				CHECK(check->SourceHash == XXH64(source));
				REQUIRE(check->Diagnostics.size() == 1);
				CHECK(check->Diagnostics[0].Message == "latest attempt");
				if (source.find("Missing") != std::string::npos)
					CHECK_FALSE(context.GetLookups().empty());
			}
			fixture.WriteProjectText("Assets/Good.luau", "return {}");
			fixture.OpenProject();
			const auto handle = fixture.GetManager().Resolve("Assets/Good.luau");
			REQUIRE(handle);
			const auto good = fixture.GetManager().Load(*handle);
			REQUIRE(good);
			fixture.WriteProjectText("Assets/Good.luau", "return )");
			{
				Test::ExpectLog expected(LogLevel::Error, "Good.luau");
				CHECK_FALSE(fixture.GetManager().Reimport(*handle));
			}
			const auto retained = fixture.GetManager().Load(*handle);
			REQUIRE(retained);
			CHECK(AssetCast<ScriptData>(*retained)->Bytecode == AssetCast<ScriptData>(*good)->Bytecode);
		}

		TEST_CASE("ScriptImporter: independent worker imports produce identical artifacts")
		{
			Test::AssetTestFixture fixture;
			const std::string source = "local C={Fields={Speed=Field.Number(3)}}; return Script.Define('Parallel',C)";
			std::array<Buffer, 2> outputs;
			std::array<bool, 2> succeeded{};
			std::latch ready(2);
			std::latch start(1);
			const auto run = [&fixture, &source, &outputs, &succeeded, &ready, &start](size_t index)
			{
				ready.count_down();
				start.wait();
				ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = Test::ParseVfsPath("project://Assets/Parallel.luau"), .SourceBytes = AsBytes(source), .Registry = &fixture.GetRegistry() });
				const auto result = ScriptImporter().Import(context, Test::ScriptMetadata());
				succeeded[index] = result.has_value();
				if (result)
					outputs[index] = result->Artifacts[0].Cooked;
			};
			std::jthread first(run, 0);
			std::jthread second(run, 1);
			ready.wait();
			start.count_down();
			first.join();
			second.join();
			REQUIRE(succeeded[0]);
			REQUIRE(succeeded[1]);
			CHECK(outputs[0] == outputs[1]);
		}
	}

}
