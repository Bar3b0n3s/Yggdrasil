#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/ScriptImporter.h"

#include "Engine/Asset/ScriptData.h"
#include "Support/AssetTestFixture.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("ScriptImporter: source bytes are authoritative and kinds are independent of filenames" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Probe.test.luau", "error(\"stale disk source\")");
			const auto path = fixture.ProjectPath("Assets/Probe.test.luau");
			const std::string_view source = "return { Value = 42 }";
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = path,
				.SourceBytes = AsBytes(source),
				.Registry = &fixture.GetRegistry(),
			});
			const AssetMetadata metadata{
				.Handle = AssetHandle(0x4001),
				.Type = AssetType::Script,
				.Importer = "Script",
				.ImporterVersion = ScriptImporter::Version,
			};
			ScriptImporter importer;
			CHECK_FALSE(importer.RequiresMainThread());
			CHECK(importer.CanImport(".luau"));
			const auto dependencyFiles = importer.ListDependencyFiles(AsBytes(source), path);
			REQUIRE(dependencyFiles);
			CHECK(dependencyFiles->empty());
			const auto imported = importer.Import(context, metadata);
			REQUIRE(imported);
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts[0].Handle == metadata.Handle);
			CHECK(imported->Artifacts[0].SubAssetKey.empty());
			const auto cooked = LoadCookedScript(imported->Artifacts[0].Cooked);
			REQUIRE(cooked);
			CHECK((*cooked)->Kind == ScriptKind::Module);
			CHECK((*cooked)->Fields.empty());
			CHECK((*cooked)->SourceMap.SourceByteCount == source.size());
		}

		TEST_CASE("ScriptImporter: Board.luau change re-extracts Game.luau" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Import Game requiring Board; mutate Board's exported field default without changing Game; refresh through "
				 "EditorAssetManager and prove Game's extracted schema and cooked hash change, both handles stay stable and "
				 "Board keeps a standalone Script meta; repeat with a transitive require and warm cache");
		}

		TEST_CASE("ScriptImporter: require reads and asset edges share one immutable source view" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Record source hashes with ReadDependency and standalone handles with FindAsset for extraction and checker "
				 "reads; deduplicate/sort edges and memoize bytes; include checker-only delayed requires, transitive changes "
				 "and every observed call edge without creating dependency-file ownership or sub-assets");
		}

		TEST_CASE("ScriptImporter: type errors cook but remain blocking findings and absence is unchecked" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("A fake diagnostics provider reports a located range in a root and required module while cooking succeeds; "
				 "manager/cache preserve performed status, environment fingerprint and findings; no provider never means clean; "
				 "export/tests reject type errors, play applies BlockPlayOnTypeErrors, and config/definition changes recheck");
		}

		TEST_CASE("ScriptImporter: failed extraction preserves the previous asset and reports located errors" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Exercise syntax errors, missing/non-Script requires, cycles, sandbox escapes, invalid fields and 250 ms "
				 "whole-graph timeout; publish no partial artifact/schema, retain previous successful asset, and recheck when "
				 "a missing required module appears");
		}

		TEST_CASE("ScriptImporter: independent worker imports produce identical artifacts" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Initialize process flags on main before workers; overlap imports with separate contexts/readers using latches; "
				 "compare serial and parallel cooked bytes and dependencies; prove no shared VM/module cache or live scene access");
		}
	}

}
