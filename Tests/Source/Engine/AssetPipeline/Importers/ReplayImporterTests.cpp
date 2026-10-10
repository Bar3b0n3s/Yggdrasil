#include "TestsPCH.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/AssetPipeline/AssetCache.h"
#include "Engine/AssetPipeline/Importers/ReplayImporter.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <latch>
#include <thread>

namespace Engine {

	namespace Test {

		static ReplayDocument ImportReplayDocument()
		{
			ReplayDocument document;
			document.Header.Scene = { AssetHandle(0x4002), "Assets/Old.scene" };
			document.Header.Parameters = VariantValue(Json::object({ { "Mode", "Replay" } }));
			document.Header.Seed = 1234;
			document.Header.FixedHz = 60;
			document.Header.EngineVersion = "1.0.0";
			document.Header.Config = "Debug";
			document.FinalTick = 5;
			document.FinalStateHash = "0123456789abcdef";
			return document;
		}

		static AssetMetadata ReplayMetadata()
		{
			return { .Handle = AssetHandle(0x4001), .Type = AssetType::Replay, .Importer = "Replay", .ImporterVersion = ReplayImporter::Version };
		}

		static Result<ImportResult> ImportReplayText(AssetTestFixture& fixture, std::string_view source,
			std::span<const ImportAssetLookupEntry> assets)
		{
			ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Probe.replay"), .SourceBytes = AsBytes(source), .Registry = &fixture.GetRegistry(), .Assets = assets });
			return ReplayImporter().Import(context, ReplayMetadata());
		}

		static std::array<ImportAssetLookupEntry, 1> ReplaySceneSnapshot()
		{
			return { ImportAssetLookupEntry{ .SourcePath = ParseVfsPath("project://Assets/Moved.scene"),
				.Handle = AssetHandle(0x4002),
				.Type = AssetType::Scene } };
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("ReplayImporter: refresh replaces cached handle lookup paths after a scene move")
		{
			Test::AssetTestFixture fixture;
			const auto sceneText = Test::ReadTestDataText("Formats/Scene/v0.upgraded.scene");
			REQUIRE(sceneText.has_value());
			fixture.WriteProjectText("Assets/Old.scene", *sceneText);
			fixture.OpenProject(false);
			auto& manager = fixture.GetManager();
			const AssetHandle scene = manager.Resolve("Assets/Old.scene").value_or(AssetHandle());
			REQUIRE(scene.IsValid());
			auto document = Test::ImportReplayDocument();
			document.Header.Scene.Handle = scene;
			const auto text = ReplayToText(document);
			REQUIRE(text.has_value());
			fixture.WriteProjectText("Assets/Run.replay", *text);
			REQUIRE(manager.Refresh().has_value());
			const AssetHandle replay = manager.Resolve("Assets/Run.replay").value_or(AssetHandle());
			REQUIRE(manager.Load(replay).has_value());
			CHECK(manager.GetDependencyGraph().GetDependencies(replay) == std::vector<AssetHandle>{ scene });
			CHECK(manager.GetPathDependents(scene).empty());
			const uint64_t key = AssetCache::ComputeKey(AsBytes(*text), ReplayImporter::Id, ReplayImporter::Version, Json(), EngineCookVersion);
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://"));
			REQUIRE(fixture.GetVfs().Move(fixture.ProjectPath("Assets/Old.scene"), fixture.ProjectPath("Assets/Moved.scene")).has_value());
			REQUIRE(fixture.GetVfs().Move(fixture.ProjectPath("Assets/Old.scene.meta"), fixture.ProjectPath("Assets/Moved.scene.meta")).has_value());
			const auto refreshed = manager.Refresh();
			REQUIRE(refreshed.has_value());
			CHECK(refreshed->Changed == std::vector<AssetHandle>{ replay });
			CHECK(manager.Resolve("Assets/Moved.scene").value_or(AssetHandle()) == scene);
			const auto cached = cache.Find(replay, key);
			REQUIRE(cached.has_value());
			REQUIRE(cached->has_value());
			REQUIRE((**cached).Lookups.size() == 1);
			REQUIRE((**cached).Lookups[0].Found.has_value());
			CHECK((**cached).Lookups[0].RequestedHandle == scene);
			CHECK((**cached).Lookups[0].Found->SourcePath == fixture.ProjectPath("Assets/Moved.scene"));
			manager.CloseProject();
			fixture.OpenProject(false);
			CHECK(manager.Load(replay).has_value());
		}

		TEST_CASE("ReplayImporter: scene handle resolves after the readable path becomes stale")
		{
			Test::AssetTestFixture fixture;
			const auto document = Test::ImportReplayDocument();
			const auto text = ReplayToText(document);
			REQUIRE(text);
			const auto assets = Test::ReplaySceneSnapshot();
			ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Probe.replay"), .SourceBytes = AsBytes(*text), .Registry = &fixture.GetRegistry(), .Assets = assets });
			const auto imported = ReplayImporter().Import(context, Test::ReplayMetadata());
			REQUIRE(imported);
			CHECK(imported->Dependencies == std::vector<AssetHandle>{ AssetHandle(0x4002) });
			const auto lookups = context.GetLookups();
			REQUIRE(lookups.size() == 1);
			CHECK(lookups[0].RequestedHandle == AssetHandle(0x4002));
			CHECK(lookups[0].Path.IsEmpty());
			REQUIRE(lookups[0].Found);
			CHECK(lookups[0].Found->SourcePath.GetPath() == "Assets/Moved.scene");
			CHECK(IsManifestCurrent(fixture.GetVfs(), context.GetDependencyReads(), lookups, assets));
			auto movedAgain = assets;
			movedAgain[0].SourcePath = fixture.ProjectPath("Assets/Again.scene");
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), context.GetDependencyReads(), lookups, movedAgain));
			const auto missing = Test::ImportReplayText(fixture, *text, {});
			REQUIRE_FALSE(missing);
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			auto wrong = assets;
			wrong[0].Type = AssetType::Script;
			const auto wrongType = Test::ImportReplayText(fixture, *text, wrong);
			REQUIRE_FALSE(wrongType);
			CHECK(wrongType.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("ReplayImporter: strict source validation reports precise pointers before cooking")
		{
			Test::AssetTestFixture fixture;
			const auto source = ReplayToJson(Test::ImportReplayDocument());
			REQUIRE(source);
			for (const auto member : { "Unexpected", "Version", "FixedHz", "FinalStateHash" })
			{
				Json invalid = *source;
				invalid[member] = member == std::string_view("Unexpected") ? Json(1) : Json("wrong");
				const auto text = JsonWriter::Write(invalid, JsonStyle::Minified);
				REQUIRE(text);
				const auto result = Test::ImportReplayText(fixture, *text, Test::ReplaySceneSnapshot());
				REQUIRE_FALSE(result);
				CHECK(result.error().GetLocation().File == "Assets/Probe.replay");
				CHECK(result.error().GetLocation().JsonPointer.has_value());
			}
			Json event = *source;
			event["Events"] = Json::array({ Json::object({ { "Tick", 5 }, { "Type", "MouseMove" }, { "Position", Json::array({ 0, 0 }) } }) });
			const auto eventText = JsonWriter::Write(event);
			REQUIRE(eventText);
			CHECK_FALSE(Test::ImportReplayText(fixture, *eventText, Test::ReplaySceneSnapshot()));
		}

		TEST_CASE("ReplayImporter: every expectation compiles through the shared compiler without execution")
		{
			Test::AssetTestFixture fixture;
			auto document = Test::ImportReplayDocument();
			document.Expect = { { 0, "Scene.GetEntityCount() >= 0" }, { 0, "error('must not execute during import')" }, { 5, "return true" } };
			const auto text = ReplayToText(document);
			REQUIRE(text);
			const auto imported = Test::ImportReplayText(fixture, *text, Test::ReplaySceneSnapshot());
			REQUIRE(imported);
			const auto loaded = LoadCookedReplay(imported->Artifacts[0].Cooked);
			REQUIRE(loaded);
			REQUIRE((*loaded)->Expect.size() == 3);
			for (size_t i = 0; i < document.Expect.size(); ++i)
			{
				const auto& expectation = (*loaded)->Expect[i];
				CHECK(expectation.Tick == document.Expect[i].Tick);
				CHECK(expectation.Script.Kind == ScriptKind::Module);
				CHECK_FALSE(expectation.Script.Bytecode.empty());
				CHECK(expectation.Script.SourceMap.JsonPointer == std::format("/Expect/{}/Luau", i));
				CHECK(expectation.Script.SourceMap.SourceHash == XXH64(document.Expect[i].Luau));
			}
		}

		TEST_CASE("ReplayImporter: expectation syntax errors map to the authored expression")
		{
			Test::AssetTestFixture fixture;
			auto document = Test::ImportReplayDocument();
			document.Expect = { { 1, "-- café\r\nreturn )" } };
			const auto text = ReplayToText(document);
			REQUIRE(text);
			const auto imported = Test::ImportReplayText(fixture, *text, Test::ReplaySceneSnapshot());
			REQUIRE_FALSE(imported);
			CHECK(imported.error().GetCode() == ErrorCode::CompileFailed);
			CHECK(imported.error().GetLocation().File == "Assets/Probe.replay");
			CHECK(imported.error().GetLocation().JsonPointer == "/Expect/0/Luau");
			CHECK(imported.error().GetLocation().Line == 2);
			CHECK(imported.error().GetLocation().Column > 0);
		}

		TEST_CASE("ReplayImporter: expression and complete chunk forms retain exact authored attribution")
		{
			Test::AssetTestFixture fixture;
			auto document = Test::ImportReplayDocument();
			document.Expect = { { 0, "1 < 2" }, { 0, "Scene.GetEntityCount()" }, { 1, "return true" },
				{ 2, "local x = 3\nreturn x == 3" }, { 5, "-- leading comment\ntrue" } };
			const auto text = ReplayToText(document);
			REQUIRE(text);
			const auto imported = Test::ImportReplayText(fixture, *text, Test::ReplaySceneSnapshot());
			REQUIRE(imported);
			const auto loaded = LoadCookedReplay(imported->Artifacts[0].Cooked);
			REQUIRE(loaded);
			REQUIRE((*loaded)->Expect.size() == document.Expect.size());
			for (size_t i = 0; i < document.Expect.size(); ++i)
			{
				const auto& map = (*loaded)->Expect[i].Script.SourceMap;
				CHECK(map.GeneratedPrefixLines == (i == 2 || i == 3 ? 0u : 1u));
				CHECK(map.Path == "Assets/Probe.replay");
				CHECK(map.ChunkName == std::format("=replay/0000000000004001/Expect/{}", i));
				CHECK(map.JsonPointer == std::format("/Expect/{}/Luau", i));
				CHECK(map.SourceByteCount == document.Expect[i].Luau.size());
			}
			document.Expect[0].Luau = "true false";
			const auto invalid = ReplayToText(document);
			REQUIRE(invalid);
			CHECK_FALSE(Test::ImportReplayText(fixture, *invalid, Test::ReplaySceneSnapshot()));
		}

		TEST_CASE("ReplayImporter: worker imports are deterministic and produce one Replay artifact")
		{
			Test::AssetTestFixture fixture;
			auto document = Test::ImportReplayDocument();
			document.Expect = { { 0, "true" }, { 5, "return true" } };
			const auto source = ReplayToText(document);
			REQUIRE(source);
			std::array<Buffer, 2> outputs;
			std::array<bool, 2> succeeded{};
			std::latch start(1);
			const auto run = [&fixture, &source, &outputs, &succeeded, &start](size_t index)
			{
				start.wait();
				const auto result = Test::ImportReplayText(fixture, *source, Test::ReplaySceneSnapshot());
				succeeded[index] = result.has_value() && result->Artifacts.size() == 1 && result->Artifacts[0].Handle == AssetHandle(0x4001) && result->Artifacts[0].SubAssetKey.empty();
				if (result)
					outputs[index] = result->Artifacts[0].Cooked;
			};
			std::jthread first(run, 0);
			std::jthread second(run, 1);
			start.count_down();
			first.join();
			second.join();
			REQUIRE(succeeded[0]);
			REQUIRE(succeeded[1]);
			CHECK(outputs[0] == outputs[1]);
			CHECK(LoadCookedReplay(outputs[0]).has_value());
		}
	}

}
