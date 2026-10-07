#include "TestsPCH.h"

#include "Engine/Asset/AssetManager.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/RingBufferSink.h"
#include "Support/ExpectLog.h"

#include <algorithm>
#include <map>

namespace Engine {

	namespace {

		// A manager that knows a fixed set of loaded assets and nothing else: the shared failure policy of the base class
		// (placeholders, diagnostics, logging once) is what these tests observe.
		class FixedAssetManager final : public AssetManager
		{
		public:
			// The dry-run and scan hooks, public for the tests.
			using AssetManager::ClearDiagnostics;
			using AssetManager::RestoreSharedState;
			using AssetManager::SaveSharedState;
			using AssetManager::SetScanDiagnostics;

			explicit FixedAssetManager(JobSystem& jobs)
				: m_Jobs(&jobs)
			{
			}

			void Add(AssetHandle handle, AssetRef<Asset> asset)
			{
				m_Assets[handle] = std::move(asset);
				BumpVersion(handle);
			}

			[[nodiscard]] Result<AssetRef<Asset>> Load(AssetHandle handle) override
			{
				if (Result<AssetRef<Asset>> builtin = GetProceduralBuiltin(handle))
					return builtin;
				const auto found = m_Assets.find(handle);
				if (found == m_Assets.end())
					return MakeError(ErrorCode::NotFound, "no asset {}", handle.ToString());
				return found->second;
			}

			[[nodiscard]] JobHandle<AssetRef<Asset>> LoadAsync(AssetHandle handle) override
			{
				Result<AssetRef<Asset>> loaded = Load(handle);
				return m_Jobs->Submit([loaded]() -> Result<AssetRef<Asset>>
				{
					return loaded;
				});
			}

			[[nodiscard]] AssetState GetState(AssetHandle handle) const override
			{
				return m_Assets.contains(handle) ? AssetState::Loaded : AssetState::Unloaded;
			}

			[[nodiscard]] const AssetMetadata* GetMetadata(AssetHandle /*handle*/) const override { return nullptr; }

			[[nodiscard]] AssetType GetAssetType(AssetHandle handle) const override
			{
				const auto found = m_Assets.find(handle);
				return found == m_Assets.end() ? AssetType::None : found->second->GetAssetType();
			}

			[[nodiscard]] std::optional<AssetHandle> Resolve(std::string_view /*reference*/) const override { return std::nullopt; }
			[[nodiscard]] std::string GetReferencePath(AssetHandle handle) const override { return handle.ToString(); }
			void WaitIdle() override {}
		private:
			JobSystem* m_Jobs = nullptr;
			std::map<AssetHandle, AssetRef<Asset>> m_Assets;
		};

		size_t CountDiagnostics(const AssetManager& manager, std::string_view code, AssetHandle asset)
		{
			return static_cast<size_t>(std::ranges::count_if(manager.GetDiagnostics(), [code, asset](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Code == code && diagnostic.Asset == asset;
			}));
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("AssetManager: asset states name themselves")
		{
			CHECK(AssetStateToString(AssetState::Unloaded) == "Unloaded");
			CHECK(AssetStateToString(AssetState::Loading) == "Loading");
			CHECK(AssetStateToString(AssetState::Loaded) == "Loaded");
			CHECK(AssetStateToString(AssetState::Failed) == "Failed");
		}

		TEST_CASE("AssetManager: missing texture yields the Missing placeholder and logs once")
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			FixedAssetManager manager(jobs);
			const AssetHandle missing(0x7777000077770000ull);

			Test::ExpectLog expected(LogLevel::Error, missing.ToString());
			const AssetRef<TextureData> first = manager.GetOrPlaceholder<TextureData>(missing);
			const AssetRef<TextureData> second = manager.GetOrPlaceholder<TextureData>(missing);
			REQUIRE(first != nullptr);
			CHECK(first == second);
			// The placeholder is the procedural Missing texture (§7.2).
			const AssetRef<TextureData> placeholder = AssetCast<TextureData>(manager.GetPlaceholder(AssetType::Texture));
			CHECK(first == placeholder);
			Result<AssetRef<Asset>> builtin = manager.Load(BuiltinAssetHandles::MissingTexture);
			REQUIRE(builtin.has_value());
			CHECK(first.get() == builtin->get());
			// Logged once, recorded once.
			CHECK(expected.GetMatchCount() == 1);
			CHECK(CountDiagnostics(manager, AssetMissingCode, missing) == 1);
			// A reference diagnostic reports a use, not an asset's state: it does not gate play or export by itself
			// (ProjectValidator checks the current references), and it goes once the handle loads.
			CHECK_FALSE(manager.HasErrorDiagnostics());
			manager.Add(missing, CreateRef<TextureData>());
			CHECK(manager.GetOrPlaceholder<TextureData>(missing) != placeholder);
			CHECK(CountDiagnostics(manager, AssetMissingCode, missing) == 0);
		}

		TEST_CASE("AssetManager: an asset of another type yields the placeholder and ASSET_TYPE_MISMATCH")
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			FixedAssetManager manager(jobs);
			const AssetHandle material(0x1234000012340000ull);
			manager.Add(material, CreateRef<MaterialData>());

			Test::ExpectLog expected(LogLevel::Error, material.ToString());
			const AssetRef<MeshData> mesh = manager.GetOrPlaceholder<MeshData>(material);
			REQUIRE(mesh != nullptr);
			CHECK(mesh.get() == AssetCast<MeshData>(manager.GetPlaceholder(AssetType::Mesh)).get());
			CHECK(CountDiagnostics(manager, AssetTypeMismatchCode, material) == 1);
			// The right type is served directly.
			CHECK(manager.GetOrPlaceholder<MaterialData>(material) != nullptr);
			CHECK(manager.GetVersion(material) == 1);
		}

		TEST_CASE("AssetManager: a null handle yields the placeholder without a diagnostic")
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			FixedAssetManager manager(jobs);
			CHECK(manager.GetOrPlaceholder<MaterialData>(AssetHandle()) != nullptr);
			CHECK(manager.GetDiagnostics().empty());
			CHECK(manager.GetVersion(AssetHandle(42)) == 0);
		}

		TEST_CASE("AssetManager: ReportDiagnostic logs each problem once and records it")
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			FixedAssetManager manager(jobs);
			const AssetDiagnostic diagnostic{
				.Severity = DiagnosticSeverity::Warning,
				.Code = std::string(AssetUnsupportedUvSetCode),
				.Asset = AssetHandle(0x99),
				.Path = "Assets/Track.glb",
				.Message = "texture 2 uses TEXCOORD_1 and is ignored",
				.Hint = {},
				.Subject = "texture:2",
				.AutoFixable = false,
			};
			Test::ExpectLog expected(LogLevel::Warn, "TEXCOORD_1");
			manager.ReportDiagnostic(diagnostic);
			manager.ReportDiagnostic(diagnostic);
			CHECK(expected.GetMatchCount() == 1);
			REQUIRE(manager.GetDiagnostics().size() == 1);
			CHECK(manager.GetDiagnostics().front() == diagnostic);
			CHECK_FALSE(manager.HasErrorDiagnostics());

			// An import failure describes the asset's state: it gates play and export.
			AssetDiagnostic failed = diagnostic;
			failed.Severity = DiagnosticSeverity::Error;
			failed.Code = std::string(AssetImportFailedCode);
			failed.Subject = {};
			Test::ExpectLog failure(LogLevel::Error, "Assets/Track.glb");
			manager.ReportDiagnostic(failed);
			CHECK(manager.HasErrorDiagnostics());
		}

		TEST_CASE("AssetManager: restoring the saved shared state undoes versions and diagnostics")
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			FixedAssetManager manager(jobs);
			const AssetHandle texture(0x4242000042420000ull);
			manager.Add(texture, CreateRef<TextureData>());
			REQUIRE(manager.GetVersion(texture) == 1);

			manager.SaveSharedState();
			manager.Add(texture, CreateRef<TextureData>());
			const AssetHandle missing(0x4343000043430000ull);
			{
				Test::ExpectLog expected(LogLevel::Error, missing.ToString());
				static_cast<void>(manager.GetOrPlaceholder<TextureData>(missing));
			}
			CHECK(manager.GetVersion(texture) == 2);
			CHECK(CountDiagnostics(manager, AssetMissingCode, missing) == 1);
			manager.RestoreSharedState();

			// A dry run leaves no trace: the version is back at 1 and the diagnostic is gone (and logs again when it recurs).
			CHECK(manager.GetVersion(texture) == 1);
			CHECK(manager.GetDiagnostics().empty());
			Test::ExpectLog again(LogLevel::Error, missing.ToString());
			static_cast<void>(manager.GetOrPlaceholder<TextureData>(missing));
			CHECK(again.GetMatchCount() == 1);
		}

		TEST_CASE("AssetManager: the Font placeholder is an empty font when the Default font cannot load")
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			FixedAssetManager manager(jobs);
			const AssetHandle missing(0x5151000051510000ull);
			Test::ExpectLog fontFailure(LogLevel::Error, BuiltinAssetHandles::DefaultFont.ToString());
			Test::ExpectLog referenceFailure(LogLevel::Error, missing.ToString());
			const AssetRef<FontData> first = manager.GetOrPlaceholder<FontData>(missing);
			const AssetRef<FontData> second = manager.GetOrPlaceholder<FontData>(missing);
			REQUIRE(first != nullptr);
			CHECK(first == second);
			CHECK(first->Glyphs.empty());
			// Each failure is logged once: the missing reference and the Default font.
			CHECK(fontFailure.GetMatchCount() == 1);
			CHECK(referenceFailure.GetMatchCount() == 1);
			CHECK(CountDiagnostics(manager, AssetImportFailedCode, BuiltinAssetHandles::DefaultFont) == 1);
		}

		TEST_CASE("AssetManager: a registry scan ends the reference type mismatches and its errors gate export")
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			FixedAssetManager manager(jobs);
			const AssetHandle material(0x6161000061610000ull);
			manager.Add(material, CreateRef<MaterialData>());
			{
				Test::ExpectLog expected(LogLevel::Error, material.ToString());
				static_cast<void>(manager.GetOrPlaceholder<MeshData>(material));
			}
			REQUIRE(CountDiagnostics(manager, AssetTypeMismatchCode, material) == 1);

			// The scan's own diagnostics replace the previous scan's; a reference mismatch goes with any scan.
			const AssetDiagnostic duplicate{
				.Severity = DiagnosticSeverity::Error,
				.Code = std::string(AssetDuplicateHandleCode),
				.Asset = AssetHandle(0x77),
				.Path = "Assets/B.png.meta",
				.Message = "duplicate handle 0000000000000077 (also in Assets/A.png.meta)",
				.Hint = {},
				.Subject = "Assets/A.png.meta",
				.AutoFixable = true,
			};
			manager.SetScanDiagnostics({ duplicate });
			CHECK(CountDiagnostics(manager, AssetTypeMismatchCode, material) == 0);
			REQUIRE(manager.GetDiagnostics().size() == 1);
			CHECK(manager.GetDiagnostics().front() == duplicate);
			CHECK(manager.HasErrorDiagnostics());
			manager.SetScanDiagnostics({});
			CHECK(manager.GetDiagnostics().empty());
			CHECK_FALSE(manager.HasErrorDiagnostics());
		}

		TEST_CASE("AssetManager: diagnostics are sorted by path, code and subject and cleared per handle")
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			FixedAssetManager manager(jobs);
			const auto makeWarning = [](std::string path, std::string_view code, std::string subject)
			{
				return AssetDiagnostic{
					.Severity = DiagnosticSeverity::Warning,
					.Code = std::string(code),
					.Asset = AssetHandle(0x99),
					.Path = std::move(path),
					.Message = "warning",
					.Hint = {},
					.Subject = std::move(subject),
					.AutoFixable = false,
				};
			};
			Test::ExpectLog warnings(LogLevel::Warn, "warning");
			manager.ReportDiagnostic(makeWarning("Assets/B.glb", AssetVertexColorsIgnoredCode, "mesh:0"));
			manager.ReportDiagnostic(makeWarning("Assets/A.glb", AssetUnsupportedUvSetCode, "texture:1"));
			manager.ReportDiagnostic(makeWarning("Assets/A.glb", AssetUnsupportedUvSetCode, "texture:0"));
			const std::span<const AssetDiagnostic> diagnostics = manager.GetDiagnostics();
			REQUIRE(diagnostics.size() == 3);
			CHECK(diagnostics[0].Subject == "texture:0");
			CHECK(diagnostics[1].Subject == "texture:1");
			CHECK(diagnostics[2].Path == "Assets/B.glb");

			manager.ClearDiagnostics(AssetHandle(0x99), std::array<std::string_view, 1>{ AssetUnsupportedUvSetCode });
			REQUIRE(manager.GetDiagnostics().size() == 1);
			CHECK(manager.GetDiagnostics().front().Code == AssetVertexColorsIgnoredCode);
			CHECK(warnings.GetMatchCount() == 3);
		}
	}

}
