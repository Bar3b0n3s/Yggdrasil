#include "TestsPCH.h"

#include "Engine/AssetPipeline/AssetHotReloader.h"

#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/AssetWriter.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Hash.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

// The "HotReload:" acceptance tests of Roadmap M6 (§7.5 race rules). Most run the manager on an inline JobSystem, so a
// poll job and an import job run inside the call that submits them and their completions wait in the MainThreadQueue until
// Drain: tests choose the times and the drain points exactly. The stale-completion test needs two imports of one handle
// that really overlap, so it uses worker threads and holds one import with a gate (a condition variable, never a sleep).

namespace Engine {

	namespace {

		size_t CountEvents(const EventLog& log, EngineEventType type)
		{
			const EngineEventType types[] = { type };
			return log.Read(0, types, 1000).Events.size();
		}

		// Polls at `seconds`, then drains the main-thread queue (the start of the next frame).
		void RunFrame(Test::AssetTestFixture& fixture, double seconds)
		{
			fixture.GetManager().Update(seconds);
			static_cast<void>(fixture.GetMainThreadQueue().Drain());
		}

		// Updates `reloader` at `seconds`, then drains the main-thread queue, where its poll results arrive.
		void RunReloaderFrame(Test::AssetTestFixture& fixture, AssetHotReloader& reloader, double seconds)
		{
			reloader.Update(seconds);
			static_cast<void>(fixture.GetMainThreadQueue().Drain());
		}

		// Holds the import of one chosen content inside Import until the test releases it.
		struct ImportGate
		{
			std::mutex Mutex;
			std::condition_variable Changed;
			uint64_t HeldContentHash = 0; // XXH64 of the source whose import waits; 0: none
			bool Entered = false;         // that import is inside Import
			bool Released = false;        // the test let it finish

			void Release()
			{
				{
					std::scoped_lock lock(Mutex);
					Released = true;
				}
				Changed.notify_all();
			}
		};

		// Releases the gate however the test ends, so a failed check never leaves a worker waiting inside Import (which
		// would block the fixture's destruction). Declared after the fixture, so it runs first.
		class ImportGateRelease
		{
		public:
			explicit ImportGateRelease(ImportGate& gate)
				: m_Gate(&gate)
			{
			}

			~ImportGateRelease() { m_Gate->Release(); }

			ImportGateRelease(const ImportGateRelease&) = delete;
			ImportGateRelease& operator=(const ImportGateRelease&) = delete;
		private:
			ImportGate* m_Gate = nullptr; // documented back-reference: the gate outlives the guard
		};

		// ".latched" files are PNGs imported as textures; the import of the gate's held content waits for the gate.
		class LatchedTextureImporter final : public IAssetImporter
		{
		public:
			explicit LatchedTextureImporter(ImportGate& gate)
				: m_Gate(&gate)
			{
			}

			[[nodiscard]] std::string_view GetId() const override { return "LatchedTexture"; }
			[[nodiscard]] uint32_t GetVersion() const override { return 1; }
			[[nodiscard]] AssetType GetMainType() const override { return AssetType::Texture; }
			[[nodiscard]] std::span<const std::string_view> GetExtensions() const override
			{
				static constexpr std::string_view Extensions[] = { ".latched" };
				return Extensions;
			}
			[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

			[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override
			{
				const uint64_t contentHash = XXH64(context.GetSourceBytes());
				{
					std::unique_lock lock(m_Gate->Mutex);
					if (contentHash == m_Gate->HeldContentHash)
					{
						m_Gate->Entered = true;
						m_Gate->Changed.notify_all();
						m_Gate->Changed.wait(lock, [gate = m_Gate]()
						{
							return gate->Released;
						});
					}
				}
				ENGINE_TRY_ASSIGN(Buffer cooked, TextureImporter::ImportTextureFromMemory(context.GetSourceBytes(), { .Usage = TextureUsage::Color, .GenerateMips = true }, "Latched"));
				ImportResult result;
				result.Artifacts.push_back({ .Handle = metadata.Handle, .Type = AssetType::Texture, .SubAssetKey = {}, .Cooked = std::move(cooked) });
				return result;
			}
		private:
			ImportGate* m_Gate = nullptr; // documented back-reference: the test's gate outlives the fixture's importers
		};

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("HotReload: an editor write causes no echo reimport")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Red.material", R"({"Format": "Material", "Version": 1, "Roughness": 0.5})");
			fixture.OpenProject(true);
			const AssetHandle handle = fixture.GetManager().Resolve("Assets/Red.material").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(handle).has_value());
			REQUIRE(fixture.GetManager().GetVersion(handle) == 1);
			RunFrame(fixture, 0.0);

			// The editor writes through its AssetWriter: one reimport, scheduled by the write itself.
			const std::string edited = R"({"Format": "Material", "Version": 1, "Roughness": 0.9})";
			REQUIRE(fixture.GetManager().GetWriter().Write(fixture.ProjectPath("Assets/Red.material"), AsBytes(edited)).has_value());
			fixture.GetManager().WaitIdle();
			CHECK(fixture.GetManager().GetVersion(handle) == 2);
			const size_t reloads = CountEvents(fixture.GetEventLog(), EngineEventType::AssetReloaded);

			// The watcher polls past the interval and the debounce: it never reports the editor's own write.
			for (double seconds = 0.5; seconds <= 3.0; seconds += 0.5)
				RunFrame(fixture, seconds);
			fixture.GetManager().WaitIdle();
			CHECK(fixture.GetManager().GetVersion(handle) == 2);
			CHECK(CountEvents(fixture.GetEventLog(), EngineEventType::AssetReloaded) == reloads);
		}

		TEST_CASE("AssetHotReloader: each reimport ticket is newer than the handle's earlier tickets")
		{
			Test::AssetTestFixture fixture;
			AssetHotReloader reloader(fixture.GetVfs(), fixture.GetJobSystem(), { .Root = fixture.ProjectPath("Assets") });
			const AssetHandle handle(0xab);
			const AssetReimportTicket slow = reloader.BeginReimport(handle, 1);
			const AssetReimportTicket fast = reloader.BeginReimport(handle, 2);
			CHECK(fast.Generation > slow.Generation);
			// The older job completes last: its result must not overwrite the newer one (§7.5 race rule 2).
			CHECK(reloader.IsCurrent(fast));
			CHECK_FALSE(reloader.IsCurrent(slow));
			// Other handles have their own generations.
			const AssetReimportTicket other = reloader.BeginReimport(AssetHandle(0xcd), 3);
			CHECK(reloader.IsCurrent(other));
			CHECK(reloader.IsCurrent(fast));
			// Stopping keeps the generations.
			reloader.Stop();
			CHECK(reloader.BeginReimport(handle, 4).Generation == fast.Generation + 1);
		}

		TEST_CASE("AssetHotReloader: polls on a job at the poll interval and delivers debounced changes sorted by path")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/A.material", "a");
			AssetHotReloader reloader(fixture.GetVfs(), fixture.GetJobSystem(), { .Root = fixture.ProjectPath("Assets") });
			std::vector<std::vector<FileChange>> delivered;
			reloader.SetChangeListener([&delivered](std::span<const FileChange> changes)
			{
				delivered.emplace_back(changes.begin(), changes.end());
			});
			CHECK_FALSE(reloader.IsRunning());
			REQUIRE(reloader.Start().has_value());
			CHECK(reloader.IsRunning());
			RunReloaderFrame(fixture, reloader, 0.0);

			fixture.WriteProjectText("Assets/B.material", "b");
			fixture.WriteProjectText("Assets/A.material", "changed");
			RunReloaderFrame(fixture, reloader, 0.3); // before the poll interval: no poll
			RunReloaderFrame(fixture, reloader, 0.5); // sees both changes; their debounce starts
			RunReloaderFrame(fixture, reloader, 0.6); // the interval has not passed since 0.5
			CHECK(delivered.empty());
			RunReloaderFrame(fixture, reloader, 1.0);
			REQUIRE(delivered.size() == 1);
			const std::vector<FileChange> expected = {
				{ .Path = fixture.ProjectPath("Assets/A.material"), .Kind = FileChangeKind::Modified },
				{ .Path = fixture.ProjectPath("Assets/B.material"), .Kind = FileChangeKind::Created },
			};
			CHECK(delivered.front() == expected);

			// A stopped reloader polls nothing.
			reloader.Stop();
			CHECK_FALSE(reloader.IsRunning());
			fixture.WriteProjectText("Assets/A.material", "again");
			RunReloaderFrame(fixture, reloader, 5.0);
			RunReloaderFrame(fixture, reloader, 6.0);
			CHECK(delivered.size() == 1);
		}

		TEST_CASE("AssetHotReloader: writes through an AssetWriter on its watcher never reach the listener")
		{
			Test::AssetTestFixture fixture;
			AssetHotReloader reloader(fixture.GetVfs(), fixture.GetJobSystem(), { .Root = fixture.ProjectPath("Assets") });
			std::vector<FileChange> delivered;
			reloader.SetChangeListener([&delivered](std::span<const FileChange> changes)
			{
				delivered.insert(delivered.end(), changes.begin(), changes.end());
			});
			REQUIRE(reloader.Start().has_value());
			AssetWriter writer(fixture.GetVfs());
			writer.SetWatcher(&reloader.GetWatcher());
			RunReloaderFrame(fixture, reloader, 0.0);

			// The editor's writes (§7.5 race rule 1) and one external write at the same time: only the external one is reported.
			REQUIRE(writer.Write(fixture.ProjectPath("Assets/Editor.material"), AsBytes(std::string_view("e"))).has_value());
			REQUIRE(writer.Move(fixture.ProjectPath("Assets/Editor.material"), fixture.ProjectPath("Assets/Moved/Editor.material")).has_value());
			fixture.WriteProjectText("Assets/External.material", "x");
			for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
				RunReloaderFrame(fixture, reloader, seconds);
			const std::vector<FileChange> expected = { { .Path = fixture.ProjectPath("Assets/External.material"), .Kind = FileChangeKind::Created } };
			CHECK(delivered == expected);
		}

		TEST_CASE("AssetHotReloader: held changes are delivered merged per path when the deferral ends")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Gone.material", "g");
			AssetHotReloader reloader(fixture.GetVfs(), fixture.GetJobSystem(), { .Root = fixture.ProjectPath("Assets") });
			std::vector<std::vector<FileChange>> delivered;
			reloader.SetChangeListener([&delivered](std::span<const FileChange> changes)
			{
				delivered.emplace_back(changes.begin(), changes.end());
			});
			REQUIRE(reloader.Start().has_value());
			RunReloaderFrame(fixture, reloader, 0.0);

			reloader.SetDeferred(true);
			CHECK(reloader.IsDeferred());
			fixture.WriteProjectText("Assets/New.material", "n");
			fixture.WriteProjectText("Assets/Temp.material", "t");
			REQUIRE(fixture.GetVfs().Remove(fixture.ProjectPath("Assets/Gone.material")).has_value());
			RunReloaderFrame(fixture, reloader, 0.5);
			RunReloaderFrame(fixture, reloader, 1.0);
			// Created then modified stays Created; deleted then recreated is Modified; created then deleted is nothing.
			fixture.WriteProjectText("Assets/New.material", "n2");
			fixture.WriteProjectText("Assets/Gone.material", "g2");
			REQUIRE(fixture.GetVfs().Remove(fixture.ProjectPath("Assets/Temp.material")).has_value());
			RunReloaderFrame(fixture, reloader, 1.5);
			RunReloaderFrame(fixture, reloader, 2.0);
			CHECK(delivered.empty());

			const std::vector<FileChange> expected = {
				{ .Path = fixture.ProjectPath("Assets/Gone.material"), .Kind = FileChangeKind::Modified },
				{ .Path = fixture.ProjectPath("Assets/New.material"), .Kind = FileChangeKind::Created },
			};
			CHECK(reloader.GetDeferredChanges() == expected);
			reloader.SetDeferred(false);
			CHECK_FALSE(reloader.IsDeferred());
			REQUIRE(delivered.size() == 1);
			CHECK(delivered.front() == expected);
			CHECK(reloader.GetDeferredChanges().empty());
		}

		TEST_CASE("AssetHotReloader: a poll result that arrives after Stop or destruction is dropped")
		{
			Test::AssetTestFixture fixture;
			size_t deliveries = 0;
			{
				AssetHotReloader reloader(fixture.GetVfs(), fixture.GetJobSystem(), { .Root = fixture.ProjectPath("Assets") });
				reloader.SetChangeListener([&deliveries](std::span<const FileChange>)
				{
					++deliveries;
				});
				REQUIRE(reloader.Start().has_value());
				RunReloaderFrame(fixture, reloader, 0.0);
				fixture.WriteProjectText("Assets/Red.material", "r");
				RunReloaderFrame(fixture, reloader, 0.5);

				// The inline poll job has completed; its continuation waits in the main-thread queue when the reloader stops.
				reloader.Update(1.0);
				reloader.Stop();
				static_cast<void>(fixture.GetMainThreadQueue().Drain());
				CHECK(deliveries == 0);

				// Restarted: a new baseline, which already holds the file.
				REQUIRE(reloader.Start().has_value());
				fixture.WriteProjectText("Assets/Blue.material", "b");
				RunReloaderFrame(fixture, reloader, 2.0);
				reloader.Update(2.5);
			}
			// The reloader is gone while the continuation of its last poll is queued.
			static_cast<void>(fixture.GetMainThreadQueue().Drain());
			CHECK(deliveries == 0);
		}

		TEST_CASE("AssetHotReloader: a failed poll is logged once and retried at the next interval")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Red.material", "r");
			AssetHotReloader reloader(fixture.GetVfs(), fixture.GetJobSystem(), { .Root = fixture.ProjectPath("Assets") });
			std::vector<FileChange> delivered;
			reloader.SetChangeListener([&delivered](std::span<const FileChange> changes)
			{
				delivered.insert(delivered.end(), changes.begin(), changes.end());
			});
			REQUIRE(reloader.Start().has_value());
			REQUIRE(fixture.GetVfs().Remove(fixture.ProjectPath("Assets")).has_value());
			{
				const Test::ExpectLog failed(LogLevel::Warn, "Hot reload could not poll 'project://Assets'");
				RunReloaderFrame(fixture, reloader, 0.0);
				RunReloaderFrame(fixture, reloader, 0.5);
				RunReloaderFrame(fixture, reloader, 1.0);
				CHECK(failed.GetMatchCount() == 1);
			}
			// The folder comes back: polling resumes and reports the file's removal and return as nothing at all.
			fixture.WriteProjectText("Assets/Red.material", "r");
			fixture.WriteProjectText("Assets/Blue.material", "b");
			RunReloaderFrame(fixture, reloader, 1.5);
			RunReloaderFrame(fixture, reloader, 2.0);
			const std::vector<FileChange> expected = { { .Path = fixture.ProjectPath("Assets/Blue.material"), .Kind = FileChangeKind::Created } };
			CHECK(delivered == expected);
		}

		TEST_CASE("HotReload: a stale job completion is dropped")
		{
			// End to end, on two workers: an import of content A is held inside Import while a newer import of content B
			// completes and is published; A's completion arrives last and must be dropped (§7.5 race rule 2).
			ImportGate gate;
			Test::AssetTestFixture fixture(1, 2);
			const ImportGateRelease releaseOnExit(gate);
			fixture.GetImporters().Register(CreateScope<LatchedTextureImporter>(gate));
			const Buffer contentA = Test::MakeTestPng(4, 4, 1);
			const Buffer contentB = Test::MakeTestPng(4, 4, 2);
			fixture.WriteProjectFile("Assets/Wood.latched", contentA);
			fixture.OpenProject(false);
			const AssetHandle wood = fixture.GetManager().Resolve("Assets/Wood.latched").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(wood).has_value());
			REQUIRE(fixture.GetManager().GetVersion(wood) == 1);

			// The older reimport (content A) enters Import and waits there.
			{
				std::scoped_lock lock(gate.Mutex);
				gate.HeldContentHash = XXH64(contentA);
			}
			JobHandle<AssetImportOutcome> slow = fixture.GetManager().ReimportAsync(wood);
			{
				std::unique_lock lock(gate.Mutex);
				gate.Changed.wait(lock, [&gate]()
				{
					return gate.Entered;
				});
			}

			// The newer reimport (content B) completes and is published while the older one is still running. The loop
			// drains the main-thread queue until the newer operation has resolved (no sleep, no clock).
			fixture.WriteProjectFile("Assets/Wood.latched", contentB);
			JobHandle<AssetImportOutcome> fast = fixture.GetManager().ReimportAsync(wood);
			while (!fast.IsReady())
			{
				static_cast<void>(fixture.GetMainThreadQueue().Drain());
				std::this_thread::yield();
			}
			REQUIRE(fast.Wait().has_value());

			// The older import finishes last; its completion is dropped.
			gate.Release();
			fixture.GetManager().WaitIdle();
			REQUIRE(slow.IsReady());

			// The published texture is B's import, published once (version 1 -> 2), never A's.
			CHECK(fixture.GetManager().GetVersion(wood) == 2);
			Result<AssetRef<Asset>> published = fixture.GetManager().Load(wood);
			REQUIRE(published.has_value());
			Result<Buffer> newest = TextureImporter::ImportTextureFromMemory(contentB, { .Usage = TextureUsage::Color, .GenerateMips = true },
				"Latched");
			REQUIRE(newest.has_value());
			Result<AssetRef<TextureData>> expected = LoadCookedTexture(*newest);
			REQUIRE(expected.has_value());
			CHECK(AssetCast<TextureData>(*published)->Pixels == (*expected)->Pixels);
		}

		TEST_CASE("HotReload: changed texture swaps and bumps the version")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 1));
			fixture.OpenProject(true);
			const AssetHandle handle = fixture.GetManager().Resolve("Assets/Wood.png").value_or(AssetHandle());
			Result<AssetRef<Asset>> before = fixture.GetManager().Load(handle);
			REQUIRE_MESSAGE(before.has_value(), before.error().ToString());
			const AssetRef<TextureData> oldTexture = AssetCast<TextureData>(*before);
			REQUIRE(oldTexture != nullptr);
			CHECK(fixture.GetManager().GetVersion(handle) == 1);
			RunFrame(fixture, 0.0);

			// An external change: polled every 0.5 s, debounced 0.2 s, reimported on a job, swapped at the next drain.
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 2));
			RunFrame(fixture, 0.5);
			RunFrame(fixture, 1.0);
			RunFrame(fixture, 1.5);
			fixture.GetManager().WaitIdle();
			CHECK(fixture.GetManager().GetVersion(handle) == 2);
			Result<AssetRef<Asset>> after = fixture.GetManager().Load(handle);
			REQUIRE(after.has_value());
			const AssetRef<TextureData> newTexture = AssetCast<TextureData>(*after);
			REQUIRE(newTexture != nullptr);
			CHECK(newTexture->Pixels != oldTexture->Pixels);
			// The old reference stays valid until released (§7.2).
			CHECK(oldTexture->Width == 4);
			CHECK(CountEvents(fixture.GetEventLog(), EngineEventType::AssetReloaded) == 1);
		}

		TEST_CASE("HotReload: reloads are held while a deterministic session runs")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 1));
			fixture.OpenProject(true);
			const AssetHandle handle = fixture.GetManager().Resolve("Assets/Wood.png").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(handle).has_value());
			RunFrame(fixture, 0.0);

			fixture.GetManager().SetReloadsDeferred(true);
			CHECK(fixture.GetManager().AreReloadsDeferred());
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 2));
			for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
				RunFrame(fixture, seconds);
			fixture.GetManager().WaitIdle();
			CHECK(fixture.GetManager().GetVersion(handle) == 1);
			REQUIRE(fixture.GetManager().GetHotReloader() != nullptr);
			CHECK(fixture.GetManager().GetHotReloader()->GetDeferredChanges().size() == 1);

			// The session ends: the held change applies at once.
			fixture.GetManager().SetReloadsDeferred(false);
			fixture.GetManager().WaitIdle();
			CHECK(fixture.GetManager().GetVersion(handle) == 2);
		}

		TEST_CASE("HotReload: a failed reimport keeps the last good version and records a diagnostic")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 1));
			fixture.OpenProject(true);
			const AssetHandle handle = fixture.GetManager().Resolve("Assets/Wood.png").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(handle).has_value());
			RunFrame(fixture, 0.0);
			// The failed reimport is reported once, at Error (AssetManager::ReportDiagnostic).
			const Test::ExpectLog failure(LogLevel::Error, "ASSET_IMPORT_FAILED Assets/Wood.png");
			fixture.WriteProjectText("Assets/Wood.png", "no longer a png");
			for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
				RunFrame(fixture, seconds);
			fixture.GetManager().WaitIdle();
			CHECK(fixture.GetManager().GetVersion(handle) == 1);
			CHECK(fixture.GetManager().Load(handle).has_value());
			CHECK(failure.GetMatchCount() == 1);
			CHECK(std::ranges::any_of(fixture.GetManager().GetDiagnostics(), [handle](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Asset == handle && diagnostic.Code == AssetImportFailedCode;
			}));
			CHECK(CountEvents(fixture.GetEventLog(), EngineEventType::AssetImportFailed) == 1);
		}
	}

}
