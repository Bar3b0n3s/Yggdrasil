#include "TestsPCH.h"

#include "Engine/AssetPipeline/AssetCache.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/AssetPipeline/Importers/ScriptImporter.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Hash.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <string>

namespace Engine {

	namespace Test {

		class PipelineCheckProvider final : public IScriptDiagnosticsProvider
		{
		public:
			explicit PipelineCheckProvider(uint64_t fingerprint)
				: m_Fingerprint(fingerprint)
			{
			}

			std::atomic<uint32_t> Calls{ 0 };
			std::function<std::vector<ScriptDiagnostic>(const ScriptCheckRequest&)> Check{};
			[[nodiscard]] uint64_t GetEnvironmentHash() const override { return m_Fingerprint; }
			[[nodiscard]] std::vector<ScriptDiagnostic> CheckScript(const ScriptCheckRequest& request) override
			{
				++Calls;
				if (Check)
					return Check(request);
				return {
					{ .Severity = DiagnosticSeverity::Error, .Code = "SCRIPT_TYPE_ERROR", .File = std::string(request.Path.GetPath()), .Line = 2, .Column = 4, .Message = std::string(request.Source), .EndLine = 3, .EndColumn = 9 },
					{ .Severity = DiagnosticSeverity::Warning, .Code = "SCRIPT_LINT_WARNING", .File = "Assets/Shared/Other.luau", .Line = 8, .Column = 2, .Message = "module finding", .EndLine = 8, .EndColumn = 12 }
				};
			}
		private:
			uint64_t m_Fingerprint = 0;
		};

		static AssetHandle LoadCheckedScript(AssetTestFixture& fixture, std::string_view path)
		{
			const AssetHandle handle = fixture.GetManager().Resolve(path).value_or(AssetHandle());
			REQUIRE(handle.IsValid());
			REQUIRE(fixture.GetManager().Load(handle).has_value());
			return handle;
		}

		static ScriptImportCheck ReadImportCheck(EditorAssetManager& manager, AssetHandle handle)
		{
			const auto check = manager.GetScriptCheck(handle);
			REQUIRE(check.has_value());
			return *check;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("ScriptImport: cached checks preserve complete ranges and distinguish unchecked from clean")
		{
			Test::PipelineCheckProvider provider(0xfedcba9876543210ull);
			Test::PipelineCheckProvider clean(24);
			Test::AssetTestFixture fixture;
			auto& manager = fixture.GetManager();
			manager.SetScriptDiagnosticsProvider(&provider);
			const std::string source = "return { Value = 7 }\n";
			fixture.WriteProjectText("Assets/Main.luau", source);
			fixture.OpenProject(false);
			const AssetHandle handle = Test::LoadCheckedScript(fixture, "Assets/Main.luau");
			const ScriptImportCheck checked = Test::ReadImportCheck(manager, handle);
			CHECK(checked.Performed);
			CHECK(checked.EnvironmentHash == provider.GetEnvironmentHash());
			CHECK(checked.SourceHash == XXH64(source));
			REQUIRE(checked.Diagnostics.size() == 2);
			CHECK(checked.Diagnostics[0].EndLine == 3);
			CHECK(checked.Diagnostics[0].EndColumn == 9);
			CHECK(provider.Calls.load() == 1);

			manager.CloseProject();
			fixture.OpenProject(false);
			REQUIRE(manager.Load(handle).has_value());
			const ScriptImportCheck cached = Test::ReadImportCheck(manager, handle);
			CHECK(cached.Diagnostics == checked.Diagnostics);
			CHECK(cached.EnvironmentHash == checked.EnvironmentHash);
			CHECK(cached.SourceHash == checked.SourceHash);
			CHECK(provider.Calls.load() == 1);
			auto ownedCopy = Test::ReadImportCheck(manager, handle);
			ownedCopy.Diagnostics.clear();
			CHECK(Test::ReadImportCheck(manager, handle).Diagnostics == checked.Diagnostics);

			manager.SetScriptDiagnosticsProvider(nullptr);
			REQUIRE(manager.Refresh().has_value());
			const ScriptImportCheck unchecked = Test::ReadImportCheck(manager, handle);
			CHECK_FALSE(unchecked.Performed);
			CHECK(unchecked.Diagnostics.empty());
			CHECK(unchecked.SourceHash == XXH64(source));
			manager.CloseProject();
			fixture.OpenProject(false);
			REQUIRE(manager.Load(handle).has_value());
			CHECK_FALSE(Test::ReadImportCheck(manager, handle).Performed);

			clean.Check = [](const ScriptCheckRequest&)
			{
				return std::vector<ScriptDiagnostic>();
			};
			manager.SetScriptDiagnosticsProvider(&clean);
			REQUIRE(manager.Refresh().has_value());
			const ScriptImportCheck cleanCheck = Test::ReadImportCheck(manager, handle);
			CHECK(cleanCheck.Performed);
			CHECK(cleanCheck.Diagnostics.empty());
			CHECK(clean.Calls.load() == 1);
			// The borrowed provider must outlive the fixture's manager.
			manager.SetScriptDiagnosticsProvider(nullptr);
		}

		TEST_CASE("ScriptImport: changed checker configuration invalidates cached checks and failed attempts")
		{
			Test::PipelineCheckProvider first(1);
			Test::PipelineCheckProvider replacement(2);
			Test::PipelineCheckProvider equivalent(2);
			Test::AssetTestFixture fixture;
			auto& manager = fixture.GetManager();
			manager.SetScriptDiagnosticsProvider(&first);
			fixture.WriteProjectText("Assets/Main.luau", "return { Value = 1 }");
			fixture.OpenProject(false);
			const AssetHandle handle = Test::LoadCheckedScript(fixture, "Assets/Main.luau");
			const uint64_t version = manager.GetVersion(handle);
			manager.SetScriptDiagnosticsProvider(&replacement);
			CHECK_FALSE(manager.GetScriptCheck(handle).has_value());
			const auto refreshed = manager.Refresh();
			REQUIRE(refreshed.has_value());
			CHECK(refreshed->Changed == std::vector<AssetHandle>{ handle });
			CHECK(replacement.Calls.load() == 1);
			CHECK(Test::ReadImportCheck(manager, handle).EnvironmentHash == 2);
			CHECK(manager.GetVersion(handle) == version);

			// Replacing the object with the same immutable environment does not recheck.
			manager.SetScriptDiagnosticsProvider(&equivalent);
			REQUIRE(manager.Refresh().has_value());
			CHECK(equivalent.Calls.load() == 0);

			fixture.WriteProjectText("Assets/Main.luau", "return function(");
			{
				Test::ExpectLog expected(LogLevel::Error, "Main.luau");
				CHECK_FALSE(manager.Reimport(handle).has_value());
			}
			CHECK(Test::ReadImportCheck(manager, handle).EnvironmentHash == 2);
			const uint32_t failures = equivalent.Calls.load();
			REQUIRE(manager.Load(handle).has_value());
			CHECK(equivalent.Calls.load() == failures);
			// An unchanged failed source is retried when its environment changes.
			manager.SetScriptDiagnosticsProvider(&first);
			{
				Test::ExpectLog expected(LogLevel::Error, "Main.luau");
				REQUIRE(manager.Refresh().has_value());
			}
			CHECK(first.Calls.load() == 2);
			CHECK(Test::ReadImportCheck(manager, handle).EnvironmentHash == 1);
			manager.SetScriptDiagnosticsProvider(nullptr);
		}

		TEST_CASE("ScriptImport: a failed reimport publishes its diagnostics while retaining the last good artifact")
		{
			Test::PipelineCheckProvider provider(10);
			Test::AssetTestFixture fixture;
			auto& manager = fixture.GetManager();
			manager.SetScriptDiagnosticsProvider(&provider);
			const std::string good = "return { Value = 4 }";
			const std::string broken = "return function(";
			fixture.WriteProjectText("Assets/Main.luau", good);
			fixture.OpenProject(false);
			const AssetHandle handle = Test::LoadCheckedScript(fixture, "Assets/Main.luau");
			const auto original = manager.Load(handle);
			REQUIRE(original.has_value());
			const auto originalCheck = Test::ReadImportCheck(manager, handle);
			const uint64_t version = manager.GetVersion(handle);

			manager.BeginDryRun();
			fixture.WriteProjectText("Assets/Main.luau", broken);
			{
				Test::ExpectLog expected(LogLevel::Error, "Main.luau");
				CHECK_FALSE(manager.Reimport(handle).has_value());
			}
			const auto failed = Test::ReadImportCheck(manager, handle);
			CHECK(failed.SourceHash == XXH64(broken));
			CHECK(failed.Diagnostics[0].Message == broken);
			CHECK(manager.Load(handle).value_or(nullptr) == *original);
			CHECK(manager.GetVersion(handle) == version);
			fixture.WriteProjectText("Assets/Main.luau", good);
			manager.EndDryRun();
			CHECK(Test::ReadImportCheck(manager, handle).Diagnostics == originalCheck.Diagnostics);
			CHECK(Test::ReadImportCheck(manager, handle).SourceHash == originalCheck.SourceHash);
			CHECK(manager.Load(handle).value_or(nullptr) == *original);
			CHECK(manager.GetVersion(handle) == version);

			fixture.WriteProjectText("Assets/Main.luau", broken);
			{
				Test::ExpectLog expected(LogLevel::Error, "Main.luau");
				CHECK_FALSE(manager.Reimport(handle).has_value());
			}
			CHECK(Test::ReadImportCheck(manager, handle).SourceHash == XXH64(broken));
			CHECK(manager.Load(handle).value_or(nullptr) == *original);
			CHECK(manager.GetVersion(handle) == version);
		}

		TEST_CASE("ImportContext: handle lookups track moved scenes and invalidate when a missing handle appears")
		{
			Test::AssetTestFixture fixture;
			std::vector<ImportAssetLookupEntry> assets = {
				{ .SourcePath = fixture.ProjectPath("Assets/Scene.scene"), .Handle = AssetHandle(8), .Type = AssetType::Scene }
			};
			ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Run.replay"), .Registry = &fixture.GetRegistry(), .Assets = assets });
			CHECK_FALSE(context.FindAsset(AssetHandle()).has_value());
			CHECK_FALSE(context.FindAsset(AssetHandle(9)).has_value());
			REQUIRE(context.FindAsset(AssetHandle(8)).has_value());
			REQUIRE(context.FindAsset(assets[0].SourcePath).has_value());
			REQUIRE(context.FindAsset(AssetHandle(8)).has_value());
			const auto lookups = context.GetLookups();
			REQUIRE(lookups.size() == 3);
			CHECK_FALSE(lookups[0].RequestedHandle.IsValid());
			CHECK(lookups[1].RequestedHandle == AssetHandle(8));
			CHECK(lookups[2].RequestedHandle == AssetHandle(9));
			CHECK(lookups[1].Path.IsEmpty());
			CHECK(IsManifestCurrent(fixture.GetVfs(), {}, lookups, assets));

			// A real script import supplies the artifact for this manifest round trip.
			fixture.WriteProjectText("Assets/Main.luau", "return {}");
			fixture.OpenProject(false);
			const AssetHandle script = Test::LoadCheckedScript(fixture, "Assets/Main.luau");
			const Buffer source = fixture.ReadProjectFile("Assets/Main.luau");
			const uint64_t key = AssetCache::ComputeKey(source, "Script", ScriptImporter::Version, Json(), EngineCookVersion);
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://"));
			auto stored = cache.Find(script, key);
			REQUIRE(stored.has_value());
			REQUIRE(stored->has_value());
			(**stored).Lookups = lookups;
			REQUIRE(cache.Store(script, key, **stored).has_value());
			const auto restored = cache.Find(script, key);
			REQUIRE(restored.has_value());
			REQUIRE(restored->has_value());
			CHECK((**restored).Lookups == lookups);

			assets[0].SourcePath = fixture.ProjectPath("Assets/Moved.scene");
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), {}, lookups, assets));
			ImportContext moved({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Run.replay"), .Assets = assets });
			const auto scene = moved.FindAsset(AssetHandle(8));
			REQUIRE(scene.has_value());
			CHECK(scene->SourcePath == assets[0].SourcePath);
			CHECK(IsManifestCurrent(fixture.GetVfs(), {}, moved.GetLookups(), assets));
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), {}, moved.GetLookups(), {}));
			assets.push_back({ .SourcePath = fixture.ProjectPath("Assets/New.scene"), .Handle = AssetHandle(9), .Type = AssetType::Scene });
			const std::array missing = { lookups[2] };
			CHECK_FALSE(IsManifestCurrent(fixture.GetVfs(), {}, missing, assets));
		}

		TEST_CASE("ScriptImport: legacy or mismatched root check metadata cannot become a clean current check")
		{
			Test::PipelineCheckProvider provider(1);
			Test::AssetTestFixture fixture;
			auto& manager = fixture.GetManager();
			const std::string source = "return {}";
			fixture.WriteProjectText("Assets/Main.luau", source);
			fixture.OpenProject(false);
			const AssetHandle handle = Test::LoadCheckedScript(fixture, "Assets/Main.luau");
			manager.CloseProject();
			AssetCache cache(fixture.GetVfs(), Test::ParseVfsPath("cache://"));
			const uint64_t key = AssetCache::ComputeKey(AsBytes(source), "Script", ScriptImporter::Version, Json(), EngineCookVersion);
			auto cached = cache.Find(handle, key);
			REQUIRE(cached.has_value());
			REQUIRE(cached->has_value());
			bool checking = true;
			SUBCASE("a legacy manifest requires a new check when a provider is present")
			{
				(**cached).ScriptCheck.reset();
			}
			SUBCASE("a legacy manifest in a tool without a provider stays unchecked")
			{
				(**cached).ScriptCheck.reset();
				checking = false;
			}
			SUBCASE("matching environment alone cannot validate a different root")
			{
				(**cached).ScriptCheck = ScriptImportCheck{ .Performed = true, .EnvironmentHash = 1, .SourceHash = XXH64(source) ^ 1 };
			}
			REQUIRE(cache.Store(handle, key, **cached).has_value());
			manager.SetScriptDiagnosticsProvider(checking ? &provider : nullptr);
			fixture.OpenProject(false);
			REQUIRE(manager.Load(handle).has_value());
			const auto check = Test::ReadImportCheck(manager, handle);
			CHECK(check.Performed == checking);
			CHECK(check.SourceHash == XXH64(source));
			CHECK(provider.Calls.load() == (checking ? 1u : 0u));
		}

		TEST_CASE("ScriptImport: superseded attempts cannot replace current diagnostics or cache metadata")
		{
			Test::PipelineCheckProvider provider(4);
			Test::AssetTestFixture fixture;
			auto& manager = fixture.GetManager();
			manager.SetScriptDiagnosticsProvider(&provider);
			fixture.WriteProjectText("Assets/Main.luau", "return {}");
			fixture.OpenProject(false);
			const AssetHandle handle = Test::LoadCheckedScript(fixture, "Assets/Main.luau");
			const std::string latest = "return { Value = 3 }";
			fixture.WriteProjectText("Assets/Main.luau", "return function(");
			auto older = manager.ReimportAsync(handle);
			fixture.WriteProjectText("Assets/Main.luau", latest);
			auto newer = manager.ReimportAsync(handle);
			CHECK_FALSE(older.Take().has_value());
			REQUIRE(newer.Take().has_value());
			manager.WaitIdle();
			const auto checked = Test::ReadImportCheck(manager, handle);
			CHECK(checked.SourceHash == XXH64(latest));
			CHECK(checked.Diagnostics[0].Message == latest);
			CHECK(manager.GetVersion(handle) == 2);
			manager.CloseProject();
			fixture.OpenProject(false);
			REQUIRE(manager.Load(handle).has_value());
			CHECK(Test::ReadImportCheck(manager, handle).Diagnostics == checked.Diagnostics);
			CHECK(provider.Calls.load() == 3);
		}

		TEST_CASE("ScriptImport: rebinding drains admitted publications before invalidating the old environment")
		{
			Test::PipelineCheckProvider oldProvider(1);
			Test::PipelineCheckProvider newProvider(2);
			Test::AssetTestFixture fixture;
			auto& manager = fixture.GetManager();
			manager.SetScriptDiagnosticsProvider(&oldProvider);
			fixture.WriteProjectText("Assets/Main.luau", "return {}");
			fixture.OpenProject(false);
			const AssetHandle handle = Test::LoadCheckedScript(fixture, "Assets/Main.luau");
			fixture.WriteProjectText("Assets/Main.luau", "return { Changed = true }");
			auto admitted = manager.ReimportAsync(handle);
			REQUIRE(admitted.Take().has_value());
			REQUIRE(fixture.GetMainThreadQueue().GetPendingCount() > 0);
			manager.SetScriptDiagnosticsProvider(&newProvider);
			CHECK(fixture.GetMainThreadQueue().GetPendingCount() == 0);
			CHECK_FALSE(manager.GetScriptCheck(handle).has_value());
			REQUIRE(manager.Refresh().has_value());
			CHECK(Test::ReadImportCheck(manager, handle).EnvironmentHash == 2);
			CHECK(newProvider.Calls.load() == 1);
		}

		TEST_CASE("ScriptImport: a failed require outside the source directory is retried when its handle appears")
		{
			Test::PipelineCheckProvider provider(5);
			Test::AssetTestFixture fixture;
			auto& manager = fixture.GetManager();
			manager.SetScriptDiagnosticsProvider(&provider);
			fixture.WriteProjectText("Assets/Logic/Main.luau", "return require(\"../Shared/Later\")");
			fixture.OpenProject(false);
			const AssetHandle handle = manager.Resolve("Assets/Logic/Main.luau").value_or(AssetHandle());
			REQUIRE(handle.IsValid());
			{
				Test::ExpectLog expected(LogLevel::Error, "Main.luau");
				CHECK_FALSE(manager.Load(handle).has_value());
			}
			const auto failed = Test::ReadImportCheck(manager, handle);
			REQUIRE(failed.Performed);
			CHECK_FALSE(manager.Load(handle).has_value());
			CHECK(provider.Calls.load() == 1);
			fixture.WriteProjectText("Assets/Shared/Later.luau", "return { Value = 12 }");
			REQUIRE(manager.Refresh().has_value());
			CHECK(manager.Load(handle).has_value());
			CHECK(provider.Calls.load() == 3); // the new module and its previously failed dependent
			CHECK(manager.GetDependencyGraph().GetDependencies(handle).size() == 1);
		}
	}

}
