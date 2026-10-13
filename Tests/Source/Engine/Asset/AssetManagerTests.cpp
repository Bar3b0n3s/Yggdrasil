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
#include <format>
#include <map>
#include <tuple>
#include <vector>

namespace Engine {

	namespace {

		// A manager that knows a fixed set of loaded assets and nothing else: the shared failure policy of the base class
		// (placeholders, diagnostics, logging once) is what these tests observe.
		class FixedAssetManager final : public AssetManager
		{
		public:
			// The dry-run and scan hooks, public for the tests.
			using AssetManager::ClearDiagnostics;
			using AssetManager::NotifyLoaded;
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

		// ADR 0022 decision 5: the observer mechanism of the base class; the managers' own notifications are tested with
		// EditorAssetManager and RuntimeAssetManager.
		TEST_CASE("AssetManager: the load observer receives each notified load until it is replaced or removed" * doctest::skip(true))
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			FixedAssetManager manager(jobs);
			const AssetHandle first(0x1111000011110000ull);
			const AssetHandle second(0x2222000022220000ull);
			std::vector<AssetHandle> seen;
			std::vector<AssetHandle> replaced;
			manager.SetLoadObserver([&seen](AssetHandle handle)
			{
				seen.push_back(handle);
			});
			manager.NotifyLoaded(first);
			manager.NotifyLoaded(first);
			manager.SetLoadObserver([&replaced](AssetHandle handle)
			{
				replaced.push_back(handle);
			});
			manager.NotifyLoaded(second);
			manager.SetLoadObserver({});
			manager.NotifyLoaded(second);
			CHECK(seen == std::vector<AssetHandle>{ first, first });
			CHECK(replaced == std::vector<AssetHandle>{ second });
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

		TEST_CASE("AssetManager: missing-asset diagnostics are kept for a bounded number of handles and one summary counts the other uses")
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			FixedAssetManager manager(jobs);
			const AssetRef<TextureData> placeholder = AssetCast<TextureData>(manager.GetPlaceholder(AssetType::Texture));
			constexpr uint64_t FirstHandle = 0x5a5a000000000000ull;
			const auto handleAt = [](uint64_t index)
			{
				return AssetHandle(FirstHandle + index);
			};
			const auto findSummary = [&manager]() -> const AssetDiagnostic*
			{
				const auto found = std::ranges::find_if(manager.GetDiagnostics(), [](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == AssetMissingCode && !diagnostic.Asset.IsValid();
				});
				return found == manager.GetDiagnostics().end() ? nullptr : &*found;
			};

			Test::ExpectLog recorded(LogLevel::Error, "is missing; the Texture placeholder stands in");
			Test::ExpectLog summarized(LogLevel::Error, "of missing assets");
			// The first MaxAssetMissingDiagnostics unknown handles behave as before: each is recorded and logged once.
			for (uint64_t index = 0; index < MaxAssetMissingDiagnostics; ++index)
				CHECK(manager.GetOrPlaceholder<TextureData>(handleAt(index)) == placeholder);
			static_cast<void>(manager.GetOrPlaceholder<TextureData>(handleAt(0)));
			CHECK(recorded.GetMatchCount() == MaxAssetMissingDiagnostics);
			CHECK(summarized.GetMatchCount() == 0);
			REQUIRE(manager.GetDiagnostics().size() == MaxAssetMissingDiagnostics);
			CHECK(findSummary() == nullptr);

			// Later unknown handles still get the placeholder, but add neither a diagnostic nor a log line: one summary,
			// logged once, counts their uses (a repeated use counts again; distinct handles would need unbounded memory).
			const AssetHandle beyond = handleAt(MaxAssetMissingDiagnostics);
			CHECK(manager.GetOrPlaceholder<TextureData>(beyond) == placeholder);
			for (uint64_t index = MaxAssetMissingDiagnostics; index < MaxAssetMissingDiagnostics + 40; ++index)
				static_cast<void>(manager.GetOrPlaceholder<TextureData>(handleAt(index)));
			CHECK(recorded.GetMatchCount() == MaxAssetMissingDiagnostics);
			CHECK(summarized.GetMatchCount() == 1);
			CHECK(CountDiagnostics(manager, AssetMissingCode, beyond) == 0);
			REQUIRE(manager.GetDiagnostics().size() == MaxAssetMissingDiagnostics + 1);
			const AssetDiagnostic* summary = findSummary();
			REQUIRE(summary != nullptr);
			CHECK(summary->Severity == DiagnosticSeverity::Error);
			CHECK(summary->Subject == AssetMissingSuppressedSubject);
			CHECK(summary->Path.empty());
			CHECK(summary->Message == std::format("41 more uses of missing assets were not recorded: diagnostics are kept for at most {} missing assets", MaxAssetMissingDiagnostics));
			// The summary sorts first (no path) and the recorded handles are the first ones, in handle order.
			CHECK(std::ranges::is_sorted(manager.GetDiagnostics(), {}, [](const AssetDiagnostic& diagnostic)
			{
				return std::tie(diagnostic.Path, diagnostic.Code, diagnostic.Subject);
			}));
			CHECK(&manager.GetDiagnostics().front() == summary);
			CHECK(manager.GetDiagnostics()[1].Asset == handleAt(0));
			CHECK(manager.GetDiagnostics().back().Asset == handleAt(MaxAssetMissingDiagnostics - 1));
			// Reference diagnostics, the summary included, never gate play or export.
			CHECK_FALSE(manager.HasErrorDiagnostics());

			// A recorded handle that loads frees its place: the next unknown handle is recorded and logged again.
			manager.Add(handleAt(0), CreateRef<TextureData>());
			CHECK(CountDiagnostics(manager, AssetMissingCode, handleAt(0)) == 0);
			const AssetHandle admitted(0x5b5b000000000000ull);
			static_cast<void>(manager.GetOrPlaceholder<TextureData>(admitted));
			CHECK(CountDiagnostics(manager, AssetMissingCode, admitted) == 1);
			CHECK(recorded.GetMatchCount() == MaxAssetMissingDiagnostics + 1);
			static_cast<void>(manager.GetOrPlaceholder<TextureData>(AssetHandle(0x5c5c000000000000ull)));
			CHECK(recorded.GetMatchCount() == MaxAssetMissingDiagnostics + 1);
			REQUIRE(findSummary() != nullptr);
			CHECK(findSummary()->Message.starts_with("42 more uses"));

			// A dry run's suppressed uses are undone with the rest of its diagnostics.
			manager.SaveSharedState();
			static_cast<void>(manager.GetOrPlaceholder<TextureData>(beyond));
			CHECK(findSummary()->Message.starts_with("43 more uses"));
			manager.RestoreSharedState();
			REQUIRE(findSummary() != nullptr);
			CHECK(findSummary()->Message.starts_with("42 more uses"));

			// A registry scan removes the summary and its count; a use that is still unrecorded starts a new one.
			manager.SetScanDiagnostics({});
			CHECK(findSummary() == nullptr);
			CHECK(manager.GetDiagnostics().size() == MaxAssetMissingDiagnostics);
			static_cast<void>(manager.GetOrPlaceholder<TextureData>(beyond));
			REQUIRE(findSummary() != nullptr);
			CHECK(findSummary()->Message.starts_with("1 more use of missing assets was not recorded"));
			CHECK(summarized.GetMatchCount() == 2);
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
