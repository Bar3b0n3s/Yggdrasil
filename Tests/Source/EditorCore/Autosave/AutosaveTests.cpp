#include "TestsPCH.h"
#include "EditorCore/Autosave/Autosave.h"

#include "EditorCore/Autosave/Private/AutosaveRecoveryData.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/ProjectLock.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/Fuzz.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <semaphore>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Engine {

	namespace {

		std::string AutosaveRead(const std::filesystem::path& path)
		{
			const auto text = FileSystem::ReadText(path);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			return *text;
		}
		void AutosaveWrite(const std::filesystem::path& path, std::string_view text)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size())), { .KeepBackup = false }).has_value());
		}
		Json AutosaveReadJson(const std::filesystem::path& path)
		{
			const auto json = JsonReader::Parse(AutosaveRead(path));
			REQUIRE(json.has_value());
			return *json;
		}
		void AutosaveWriteJson(const std::filesystem::path& path, const Json& json)
		{
			const auto text = JsonWriter::Write(json);
			REQUIRE(text.has_value());
			AutosaveWrite(path, *text);
		}
		// Moves a file's write time ten seconds on, as a later rewrite of the same bytes would: the change is then
		// observable however coarse the file system's timestamps are.
		void AutosaveTouch(const std::filesystem::path& path)
		{
			std::error_code error;
			const auto time = std::filesystem::last_write_time(path, error);
			REQUIRE_MESSAGE(!error, error.message());
			std::filesystem::last_write_time(path, time + std::chrono::seconds(10), error);
			REQUIRE_MESSAGE(!error, error.message());
		}
		struct AutosaveSetup
		{
			Test::EditorTestFixture Fixture{ "Autosave" };
			Autosave Service;
			explicit AutosaveSetup(const AutosaveSpecification& specification = {})
				: Service(Fixture.GetEditor(), specification)
			{
				Fixture.CreateAndOpenProject();
				Fixture.CreateAndOpenScene();
				REQUIRE(Service.Publish().has_value());
			}
			void Dirty(std::string name = "Dirty")
			{
				EditorContext& editor = Fixture.GetEditor();
				SceneEdit edit(editor, "Add entity");
				static_cast<void>(editor.GetScene().CreateEntity(std::move(name)));
				REQUIRE(edit.Commit().has_value());
			}
			std::filesystem::path Root() const { return Fixture.GetProjectRoot() / "Library" / "Autosave"; }
			std::filesystem::path Source() const { return Fixture.GetProjectRoot() / "Assets" / "Scenes" / "Main.scene"; }
			AutosaveWriteResult Save()
			{
				const auto result = Service.Save(AutosaveReason::Periodic);
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				REQUIRE(result->Written);
				return *result;
			}
			AutosaveRecoveryInfo Offer()
			{
				const auto result = Service.FindRecovery();
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				REQUIRE(result->has_value());
				return **result;
			}
		};

		// The bounded wait represents native I/O held by the test, not synchronization inside Autosave.
		struct AutosaveWriteBarrier
		{
			std::binary_semaphore Entered{ 0 };
			std::binary_semaphore Resume{ 0 };
			std::atomic<bool> Armed{ false };
			std::atomic<bool> TimedOut{ false };
			bool Fail = false;
			AutosaveSpecification Specification()
			{
				AutosaveSpecification result;
				result.WriteFile = [this](const std::filesystem::path& path, std::span<const std::byte> bytes) -> Status
				{
					if (Armed.exchange(false))
					{
						Entered.release();
						if (!Resume.try_acquire_for(std::chrono::seconds(20)))
						{
							TimedOut.store(true);
							return MakeError(ErrorCode::Io, "autosave test writer was not resumed");
						}
						if (Fail)
							return MakeError(ErrorCode::Io, "injected autosave write failure");
					}
					return FileSystem::WriteFileAtomic(path, bytes, { .KeepBackup = false });
				};
				return result;
			}
		};

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("Autosave: an injected clock saves a dirty scene every two minutes")
		{
			AutosaveSetup setup;
			setup.Dirty();
			CHECK_FALSE(setup.Service.Update(0)->Written);
			CHECK_FALSE(setup.Service.Update(119)->Written);
			REQUIRE(setup.Service.Update(120)->Written);
			CHECK_FALSE(setup.Service.Update(239)->Written);
			REQUIRE(setup.Service.Update(500)->Written);
			CHECK_FALSE(setup.Service.Update(501)->Written);
			CHECK_FALSE(setup.Service.Update(500).has_value());
			CHECK_FALSE(setup.Service.Update(std::numeric_limits<double>::infinity()).has_value());
			CHECK(setup.Fixture.GetEditor().IsSceneDirty());
		}

		TEST_CASE("Autosave: entering play saves the current committed edit scene")
		{
			AutosaveSetup setup;
			setup.Dirty("BeforePlay");
			const auto result = setup.Service.Save(AutosaveReason::BeforePlay);
			REQUIRE(result.has_value());
			CHECK(result->Written);
			CHECK(AutosaveRead(setup.Root() / result->Generation / "Scene.json").contains("BeforePlay"));
			CHECK_FALSE(AutosaveRead(setup.Source()).contains("BeforePlay"));
		}

		TEST_CASE("Autosave: clean read-only and dry-run editors write nothing")
		{
			AutosaveSetup setup;
			CHECK_FALSE(setup.Service.Update(120)->Written);
			CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::NothingDirty);
			{
				const auto dry = EditorDryRunScope::Begin(setup.Fixture.GetEditor());
				REQUIRE(dry.has_value());
				setup.Dirty();
				CHECK_FALSE(setup.Service.Update(240)->Written);
				CHECK_FALSE(setup.Service.Save(AutosaveReason::BeforePlay).has_value());
				const auto deferred = setup.Service.Update(240);
				REQUIRE(deferred.has_value());
				CHECK_FALSE(deferred->Written);
				CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::Disabled);
			}
			REQUIRE(setup.Service.Reset());
			EditorContext& editor = setup.Fixture.GetEditor();
			const auto projectFile = editor.GetProject().GetProjectFile();
			REQUIRE(editor.CloseProject());
			const ProjectOpenOptions options{ .ReadOnly = true, .ReadOnlyCacheDirectory = setup.Fixture.GetDirectory().GetPath() / "Readonly" };
			auto project = ProjectManager::OpenProject(projectFile, options, editor.GetTypeRegistry());
			REQUIRE(project.has_value());
			REQUIRE(editor.OpenProject(std::move(*project)).has_value());
			CHECK_FALSE(setup.Service.Update(120)->Written);
			CHECK(setup.Service.Save(AutosaveReason::Periodic).error().GetCode() == ErrorCode::PermissionDenied);
			CHECK_FALSE(FileSystem::Exists(setup.Root()));
		}

		TEST_CASE("Autosave: native assets and settings remain on their write-through path")
		{
			AutosaveSetup setup;
			const auto projectFile = setup.Fixture.GetEditor().GetProject().GetProjectFile();
			const std::string projectBefore = AutosaveRead(projectFile);
			const std::string sceneBefore = AutosaveRead(setup.Source());
			setup.Dirty();
			const auto saved = setup.Save();
			CHECK(saved.Files == std::vector<std::string>{ "Assets/Scenes/Main.scene" });
			CHECK(AutosaveRead(projectFile) == projectBefore);
			CHECK(AutosaveRead(setup.Source()) == sceneBefore);
			const auto files = FileSystem::ListDirectory(setup.Root() / saved.Generation, false);
			REQUIRE(files.has_value());
			CHECK(files->size() == 2);
		}

		TEST_CASE("Autosave: unfinished UI previews are excluded from recovery")
		{
			AutosaveSetup setup;
			setup.Dirty("Committed");
			REQUIRE(setup.Service.Publish().has_value());
			{
				SceneEdit preview(setup.Fixture.GetEditor(), "Preview");
				static_cast<void>(setup.Fixture.GetEditor().GetScene().CreateEntity("Uncommitted"));
				REQUIRE(setup.Service.Publish().has_value());
				CHECK_FALSE(setup.Service.Save(AutosaveReason::BeforePlay).has_value());
				const auto deferred = setup.Service.Update(120);
				REQUIRE(deferred.has_value());
				CHECK_FALSE(deferred->Written);
				CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::Saved);
			}
			const std::string bytes = AutosaveRead(setup.Root() / setup.Offer().Generation / "Scene.json");
			CHECK(bytes.contains("Committed"));
			CHECK_FALSE(bytes.contains("Uncommitted"));
		}

		TEST_CASE("Autosave: a failed write leaves the previous complete generation recoverable")
		{
			bool fail = false;
			AutosaveSpecification specification;
			specification.WriteFile = [&fail](const std::filesystem::path& path, std::span<const std::byte> bytes) -> Status
			{
				if (fail && path.filename() == "Manifest.json")
					return MakeError(ErrorCode::Io, "injected manifest failure");
				return FileSystem::WriteFileAtomic(path, bytes, { .KeepBackup = false });
			};
			AutosaveSetup setup(specification);
			setup.Dirty("First");
			const auto first = setup.Save();
			setup.Dirty("Second");
			fail = true;
			CHECK_FALSE(setup.Service.Save(AutosaveReason::Periodic).has_value());
			CHECK(setup.Offer().Generation == first.Generation);
			CHECK_FALSE(AutosaveRead(setup.Root() / first.Generation / "Scene.json").contains("Second"));
		}

		TEST_CASE("Autosave: an incomplete generation is ignored until its manifest is committed")
		{
			AutosaveSetup setup;
			AutosaveWrite(setup.Root() / "g-ffffffffffffffff" / "Scene.json", "incomplete");
			const auto missing = setup.Service.FindRecovery();
			REQUIRE(missing.has_value());
			CHECK_FALSE(missing->has_value());
			setup.Dirty();
			const auto saved = setup.Save();
			CHECK(setup.Offer().Generation == saved.Generation);
		}

		TEST_CASE("Autosave: recovery installs a dirty scene without overwriting its source")
		{
			AutosaveSetup setup;
			const std::string source = AutosaveRead(setup.Source());
			setup.Dirty("Recovered");
			setup.Save();
			const auto offer = setup.Offer();
			setup.Dirty("Discarded");
			REQUIRE(setup.Service.Recover(offer).has_value());
			CHECK(setup.Fixture.GetEditor().GetScene().GetEntityCount() == 1);
			CHECK(setup.Fixture.GetEditor().IsSceneDirty());
			CHECK_FALSE(setup.Fixture.GetEditor().GetHistory().CanUndo());
			CHECK(AutosaveRead(setup.Source()) == source);
		}

		TEST_CASE("Autosave: corrupt or stale recovery leaves the current scene unchanged")
		{
			AutosaveSetup setup;
			setup.Dirty();
			const auto saved = setup.Save();
			const auto offer = setup.Offer();
			const uint64_t revision = setup.Fixture.GetEditor().GetRevision();
			AutosaveWrite(setup.Root() / saved.Generation / "Scene.json", "{}");
			CHECK_FALSE(setup.Service.Recover(offer).has_value());
			CHECK(setup.Fixture.GetEditor().GetRevision() == revision);
			CHECK(setup.Fixture.GetEditor().GetScene().GetEntityCount() == 1);
		}

		TEST_CASE("Autosave: escaping symlinked and cross-project recovery paths are refused")
		{
			AutosaveSetup setup;
			setup.Dirty();
			const auto saved = setup.Save();
			const auto path = setup.Root() / saved.Generation / "Metadata.json";
			Json metadata = AutosaveReadJson(path);
			SUBCASE("escaping")
			{
				metadata["ScenePath"] = "../Other.scene";
			}
			SUBCASE("absolute")
			{
				metadata["ScenePath"] = FileSystem::PathToUtf8(setup.Source());
			}
			SUBCASE("other project")
			{
				metadata["ProjectFile"] = "Another.eproj";
			}
			SUBCASE("linked source")
			{
				const auto other = setup.Fixture.GetDirectory().GetPath() / "Outside";
				AutosaveWrite(other / "Main.scene", AutosaveRead(setup.Source()));
				const auto link = setup.Fixture.GetProjectRoot() / "Assets" / "Linked";
#if defined(ENGINE_PLATFORM_WINDOWS)
				// Junction creation needs no developer-mode privilege; both endpoints remain in this test's directory.
				size_t shellSize = 0;
				REQUIRE(getenv_s(&shellSize, nullptr, 0, "ComSpec") == 0);
				REQUIRE(shellSize != 0);
				std::string shell(shellSize, '\0');
				REQUIRE(getenv_s(&shellSize, shell.data(), shell.size(), "ComSpec") == 0);
				shell.resize(shellSize - 1);
				std::string linkPath = FileSystem::PathToUtf8(link);
				std::string otherPath = FileSystem::PathToUtf8(other);
				std::replace(linkPath.begin(), linkPath.end(), '/', '\\');
				std::replace(otherPath.begin(), otherPath.end(), '/', '\\');
				const auto linked = Process::Run({ .Executable = FileSystem::PathFromUtf8(shell), .Arguments = { "/c", "mklink", "/J", linkPath, otherPath } }, std::chrono::seconds(30));
				REQUIRE_MESSAGE(linked.has_value(), linked.error().ToString());
				REQUIRE_MESSAGE(linked->ExitCode == 0, linked->StandardError);
#else
				std::error_code error;
				std::filesystem::create_directory_symlink(other, link, error);
				REQUIRE_MESSAGE(!error, error.message());
#endif
				metadata["ScenePath"] = "Assets/Linked/Main.scene";
			}
			AutosaveWriteJson(path, metadata);
			CHECK_FALSE(setup.Service.FindRecovery().has_value());
		}

		TEST_CASE("Autosave: source replacement and newer files are never silently recovered")
		{
			AutosaveSetup setup;
			setup.Dirty();
			setup.Save();
			const auto offer = setup.Offer();
			AutosaveWrite(setup.Source(), AutosaveRead(setup.Source()) + "\n");
			CHECK(setup.Service.Recover(offer).error().GetCode() == ErrorCode::Conflict);
			CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::IoFailure);
		}

		TEST_CASE("Autosave: 1,000 seeded mutations of the documents of an autosave never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seeds: the manifest and the two generations a real autosave wrote, read once. Every iteration mutates one of
			// the five documents and reads them through ParseAutosaveRecovery, ReadAutosaveRecovery's checks in memory (the
			// files' own checks are covered by the cases above), so no iteration touches a file.
			AutosaveSetup setup;
			setup.Dirty("First");
			const AutosaveWriteResult first = setup.Save();
			setup.Dirty("Second");
			const AutosaveWriteResult second = setup.Save();
			Utils::AutosaveRecoveryDocuments documents;
			documents.Manifest = AutosaveRead(setup.Root() / "Manifest.json");
			for (const std::string& generation : { first.Generation, second.Generation })
			{
				documents.Generations[generation] = Utils::AutosaveGenerationDocuments{ .Metadata = AutosaveRead(setup.Root() / generation / "Metadata.json"),
					.Scene = AutosaveRead(setup.Root() / generation / "Scene.json") };
			}
			const Result<std::string> identity = JsonReader(AutosaveReadJson(setup.Root() / second.Generation / "Metadata.json")).ReadMember<std::string>("ProjectFile");
			REQUIRE(identity.has_value());
			const std::filesystem::path projectFile = FileSystem::PathFromUtf8(*identity);
			// Unmutated, the documents in memory give the offer the files give.
			const auto fromFiles = Utils::ReadAutosaveRecovery(setup.Fixture.GetEditor().GetProject());
			const auto fromMemory = Utils::ParseAutosaveRecovery(projectFile, documents);
			REQUIRE_MESSAGE(fromFiles.has_value(), fromFiles.error().ToString());
			REQUIRE_MESSAGE(fromMemory.has_value(), fromMemory.error().ToString());
			REQUIRE(fromFiles->has_value());
			REQUIRE(fromMemory->has_value());
			CHECK((*fromMemory)->Info.Generation == (*fromFiles)->Info.Generation);
			CHECK((*fromMemory)->SceneText == (*fromFiles)->SceneText);

			const std::array<std::string, 2> generations = { first.Generation, second.Generation };
			const Test::FuzzRun run = Test::RunFuzzCase(0xA5A7E, 1000, [&documents, &projectFile, &generations](Random& random, uint64_t /*index*/)
			{
				Utils::AutosaveRecoveryDocuments mutated = documents;
				const int64_t target = random.RangeInt(0, 4);
				if (target == 0)
				{
					Test::MutateText(*mutated.Manifest, random);
				}
				else
				{
					Utils::AutosaveGenerationDocuments& generation = mutated.Generations[generations[static_cast<size_t>((target - 1) / 2)]];
					Test::MutateText(target % 2 == 1 ? generation.Metadata : generation.Scene, random);
				}
				const std::string_view only = random.NextBool() ? std::string_view() : std::string_view(generations[static_cast<size_t>(random.RangeInt(0, 1))]);
				const auto recovered = Utils::ParseAutosaveRecovery(projectFile, mutated, only);
				if (!recovered.has_value())
					return false;
				if (recovered->has_value())
				{
					CHECK(std::ranges::find(generations, (*recovered)->Info.Generation) != generations.end());
					CHECK((*recovered)->Info.NewerThanSource);
				}
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}

		TEST_CASE("Autosave: an untitled scene is recoverable")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			editor.SetScene(editor.CreateScene("Untitled"), std::nullopt, true);
			setup.Dirty();
			setup.Save();
			const auto offer = setup.Offer();
			CHECK(offer.ScenePath.empty());
			REQUIRE(setup.Service.Recover(offer).has_value());
			CHECK_FALSE(editor.GetScenePath().has_value());
			CHECK(editor.IsSceneDirty());
		}

		TEST_CASE("Autosave: a worker fatal write uses only the last published owned snapshot")
		{
			AutosaveSetup setup;
			setup.Dirty("Published");
			REQUIRE(setup.Service.Publish().has_value());
			setup.Dirty("NotPublished");
			AutosaveFatalResult result{};
			std::jthread worker([&setup, &result]()
			{
				result = setup.Service.WriteFatalSnapshot();
			});
			worker.join();
			CHECK(result == AutosaveFatalResult::Saved);
			const std::string bytes = AutosaveRead(setup.Root() / setup.Offer().Generation / "Scene.json");
			CHECK(bytes.contains("Published"));
			CHECK_FALSE(bytes.contains("NotPublished"));
		}

		TEST_CASE("Autosave: a fatal write interrupted during publication never waits on the main thread")
		{
			AutosaveWriteBarrier barrier;
			AutosaveSetup setup(barrier.Specification());
			setup.Dirty("Claimed");
			REQUIRE(setup.Service.Publish().has_value());
			barrier.Armed.store(true);
			AutosaveFatalResult result{};
			std::jthread worker([&setup, &result]()
			{
				result = setup.Service.WriteFatalSnapshot();
			});
			const bool entered = barrier.Entered.try_acquire_for(std::chrono::seconds(20));
			if (entered)
			{
				for (int index = 0; index < 10; ++index)
				{
					setup.Dirty("Later");
					CHECK(setup.Service.Publish().has_value());
				}
				CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::Busy);
			}
			barrier.Resume.release();
			worker.join();
			REQUIRE(entered);
			CHECK_FALSE(barrier.TimedOut.load());
			CHECK(result == AutosaveFatalResult::Saved);
			CHECK_FALSE(AutosaveRead(setup.Root() / setup.Offer().Generation / "Scene.json").contains("Later"));
			// A writer may finish after its publication was replaced many times. Its stale slot must be reclaimable,
			// and releasing it must not withdraw the newer publication or prevent another worker from claiming it.
			for (int index = 0; index < 10; ++index)
			{
				const std::string name = std::format("Reclaimed{}", index);
				setup.Dirty(name);
				REQUIRE(setup.Service.Publish().has_value());
				std::jthread next([&setup, &result]()
				{
					result = setup.Service.WriteFatalSnapshot();
				});
				next.join();
				REQUIRE(result == AutosaveFatalResult::Saved);
				CHECK(AutosaveRead(setup.Root() / setup.Offer().Generation / "Scene.json").contains(name));
			}
		}

		TEST_CASE("Autosave: an explicit fatal-reason save captures the latest dirty revision")
		{
			AutosaveSetup setup;
			setup.Dirty("Old");
			REQUIRE(setup.Service.Publish().has_value());
			setup.Dirty("Latest");
			const auto result = setup.Service.Save(AutosaveReason::FatalError);
			REQUIRE(result.has_value());
			CHECK(AutosaveRead(setup.Root() / result->Generation / "Scene.json").contains("Latest"));
		}

		TEST_CASE("Autosave: close and explicit save withdraw only the matching recovery")
		{
			AutosaveSetup setup;
			setup.Dirty("First");
			const auto first = setup.Save();
			setup.Fixture.CreateAndOpenScene("Assets/Scenes/Other.scene");
			REQUIRE(setup.Service.Publish().has_value());
			setup.Dirty("Other");
			const auto other = setup.Save();
			EditorContext& editor = setup.Fixture.GetEditor();
			const auto bytes = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(bytes.has_value());
			AutosaveWrite(setup.Fixture.GetProjectRoot() / "Assets/Scenes/Other.scene", *bytes);
			REQUIRE(editor.MarkSceneSaved(*editor.GetScenePath()));
			REQUIRE(setup.Service.DiscardSavedRecovery().has_value());
			CHECK(setup.Offer().Generation == first.Generation);
			CHECK_FALSE(FileSystem::Exists(setup.Root() / other.Generation));
			REQUIRE(setup.Service.Reset());
			CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::Disabled);
			CHECK(FileSystem::Exists(setup.Root() / first.Generation));
		}

		TEST_CASE("Autosave: reset retains the project lock until an already-claimed fatal writer finishes")
		{
			AutosaveWriteBarrier barrier;
			AutosaveSetup setup(barrier.Specification());
			setup.Dirty();
			REQUIRE(setup.Service.Publish().has_value());
			barrier.Armed.store(true);
			AutosaveFatalResult result{};
			std::jthread worker([&setup, &result]()
			{
				result = setup.Service.WriteFatalSnapshot();
			});
			const bool entered = barrier.Entered.try_acquire_for(std::chrono::seconds(20));
			if (entered)
			{
				CHECK_FALSE(setup.Service.Reset());
				CHECK_FALSE(setup.Service.Publish().has_value());
				CHECK_FALSE(ProjectLock::Acquire(setup.Fixture.GetProjectRoot() / "Library" / "Editor.lock").has_value());
				CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::Disabled);
			}
			barrier.Resume.release();
			worker.join();
			REQUIRE(entered);
			CHECK(result == AutosaveFatalResult::Saved);
			CHECK(setup.Service.Reset());
			REQUIRE(setup.Fixture.GetEditor().CloseProject());
			CHECK(ProjectLock::Acquire(setup.Fixture.GetProjectRoot() / "Library" / "Editor.lock").has_value());
		}

		TEST_CASE("Autosave: a fatal claim after reset is disabled without waiting")
		{
			AutosaveSetup setup;
			setup.Dirty();
			REQUIRE(setup.Service.Publish().has_value());
			REQUIRE(setup.Service.Reset());
			CHECK(setup.Service.Reset());
			CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::Disabled);
			CHECK_FALSE(FileSystem::Exists(setup.Root()));
			REQUIRE(setup.Service.Publish().has_value());
			CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::Saved);
		}

		TEST_CASE("Autosave: a failed fatal write releases the closing project claim")
		{
			AutosaveWriteBarrier barrier;
			AutosaveSetup setup(barrier.Specification());
			setup.Dirty();
			const auto first = setup.Save();
			// A newer published revision, so the fatal claim has a generation to write (it never duplicates the newest).
			setup.Dirty("AfterFirstGeneration");
			REQUIRE(setup.Service.Publish().has_value());
			barrier.Fail = true;
			barrier.Armed.store(true);
			AutosaveFatalResult result{};
			std::jthread worker([&setup, &result]()
			{
				result = setup.Service.WriteFatalSnapshot();
			});
			const bool entered = barrier.Entered.try_acquire_for(std::chrono::seconds(20));
			if (entered)
				CHECK_FALSE(setup.Service.Reset());
			barrier.Resume.release();
			worker.join();
			REQUIRE(entered);
			CHECK(result == AutosaveFatalResult::IoFailure);
			CHECK(setup.Service.Reset());
			CHECK(setup.Offer().Generation == first.Generation);
		}

		TEST_CASE("Autosave: fatal and periodic writes never publish manifests concurrently")
		{
			Autosave* service = nullptr;
			AutosaveFatalResult competing{};
			AutosaveSpecification specification;
			specification.WriteFile = [&service, &competing](const std::filesystem::path& path, std::span<const std::byte> bytes) -> Status
			{
				std::jthread worker([&service, &competing]()
				{
					competing = service->WriteFatalSnapshot();
				});
				worker.join();
				return FileSystem::WriteFileAtomic(path, bytes, { .KeepBackup = false });
			};
			AutosaveSetup setup(specification);
			service = &setup.Service;
			setup.Dirty();
			const auto saved = setup.Save();
			CHECK(competing == AutosaveFatalResult::Busy);
			CHECK(setup.Offer().Generation == saved.Generation);
		}

		TEST_CASE("Autosave: equal opaque timestamps still allow a dirty derived revision")
		{
			AutosaveSetup setup;
			const auto before = FileSystem::GetInfo(setup.Source());
			REQUIRE(before.has_value());
			setup.Dirty();
			setup.Save();
			CHECK(setup.Offer().NewerThanSource);
			CHECK(FileSystem::GetInfo(setup.Source())->ModificationTime == before->ModificationTime);
		}

		TEST_CASE("Autosave: timestamp numeric order never determines recovery freshness")
		{
			AutosaveSetup setup;
			setup.Dirty();
			const auto saved = setup.Save();
			const auto metadataPath = setup.Root() / saved.Generation / "Metadata.json";
			const Json original = AutosaveReadJson(metadataPath);
			for (uint64_t token : { uint64_t{ 0 }, std::numeric_limits<uint64_t>::max() })
			{
				Json metadata = original;
				metadata["SourceFingerprint"]["ModificationTime"] = token;
				AutosaveWriteJson(metadataPath, metadata);
				CHECK(setup.Service.FindRecovery().error().GetCode() == ErrorCode::Conflict);
			}
			AutosaveWriteJson(metadataPath, original);
			CHECK(setup.Offer().NewerThanSource);
		}

		TEST_CASE("Autosave: persisted generation order survives restart without directory ordering")
		{
			AutosaveSetup setup;
			setup.Dirty("First");
			setup.Save();
			setup.Dirty("Last");
			const auto last = setup.Save();
			const auto path = setup.Root() / "Manifest.json";
			Json manifest = AutosaveReadJson(path);
			std::reverse(manifest["Generations"].begin(), manifest["Generations"].end());
			AutosaveWriteJson(path, manifest);
			REQUIRE(setup.Service.Reset());
			Autosave reopened(setup.Fixture.GetEditor());
			const auto offer = reopened.FindRecovery();
			REQUIRE(offer.has_value());
			REQUIRE(offer->has_value());
			CHECK((**offer).Generation == last.Generation);
		}

		TEST_CASE("Autosave: identical payloads and changed base fingerprints are never offered")
		{
			AutosaveSetup setup;
			SUBCASE("identical bytes")
			{
				const auto path = setup.Fixture.GetEditor().GetScenePath();
				const std::string bytes = AutosaveRead(setup.Source());
				auto scene = setup.Fixture.GetEditor().CreateScene("Same");
				LoadReport report;
				REQUIRE(SceneSerializer::LoadFromString(*scene, bytes, {}, report).has_value());
				setup.Fixture.GetEditor().SetScene(std::move(scene), path, true);
				CHECK_FALSE(setup.Service.Save(AutosaveReason::Periodic)->Written);
				CHECK_FALSE(setup.Service.FindRecovery()->has_value());
			}
			SUBCASE("same size and write time but changed source bytes")
			{
				setup.Dirty();
				setup.Save();
				std::error_code error;
				const auto timestamp = std::filesystem::last_write_time(setup.Source(), error);
				REQUIRE(!error);
				std::string changed = AutosaveRead(setup.Source());
				const auto position = changed.find("Main");
				REQUIRE(position != std::string::npos);
				changed.replace(position, 4, "Else");
				AutosaveWrite(setup.Source(), changed);
				std::filesystem::last_write_time(setup.Source(), timestamp, error);
				REQUIRE(!error);
				CHECK(setup.Service.FindRecovery().error().GetCode() == ErrorCode::Conflict);
			}
		}
		TEST_CASE("Autosave: retention bounds complete generations without reusing their sequence")
		{
			AutosaveSetup setup({ .RetainedGenerations = 2 });
			setup.Dirty();
			const auto first = setup.Save();
			const auto second = setup.Save();
			const auto third = setup.Save();
			CHECK_FALSE(FileSystem::Exists(setup.Root() / first.Generation));
			CHECK(FileSystem::Exists(setup.Root() / second.Generation));
			CHECK(FileSystem::Exists(setup.Root() / third.Generation));
			const Json manifest = AutosaveReadJson(setup.Root() / "Manifest.json");
			CHECK(manifest["Generations"].size() == 2);
			CHECK(manifest["Sequence"] == 3);
		}

		TEST_CASE("Autosave: an untitled recovery keeps its token until the first explicit save")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			editor.SetScene(editor.CreateScene("Untitled"), std::nullopt, true);
			setup.Dirty();
			const auto saved = setup.Save();
			REQUIRE(setup.Service.Recover(setup.Offer()).has_value());
			const auto bytes = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(bytes.has_value());
			const auto path = VfsPath::Create("project", "Assets/Scenes/Named.scene");
			REQUIRE(path.has_value());
			AutosaveWrite(setup.Fixture.GetProjectRoot() / "Assets/Scenes/Named.scene", *bytes);
			REQUIRE(editor.MarkSceneSaved(*path));
			REQUIRE(setup.Service.DiscardSavedRecovery().has_value());
			CHECK_FALSE(FileSystem::Exists(setup.Root() / saved.Generation));
			CHECK_FALSE(setup.Service.FindRecovery()->has_value());
			const Json manifest = AutosaveReadJson(setup.Root() / "Manifest.json");
			CHECK(manifest["Sequence"] == 1);
			CHECK(manifest["Generations"].empty());
		}

		TEST_CASE("Autosave: forged offers and oversized manifests are rejected before installing a scene")
		{
			AutosaveSetup setup;
			setup.Dirty();
			setup.Save();
			auto offer = setup.Offer();
			const uint64_t revision = setup.Fixture.GetEditor().GetRevision();
			offer.Files = { "Assets/Other.scene" };
			CHECK(setup.Service.Recover(offer).error().GetCode() == ErrorCode::Validation);
			CHECK(setup.Fixture.GetEditor().GetRevision() == revision);
			AutosaveWrite(setup.Root() / "Manifest.json", std::string(1024 * 1024 + 1, ' '));
			CHECK(setup.Service.FindRecovery().error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("Autosave: changing the source during payload write prevents manifest publication")
		{
			std::filesystem::path source;
			bool modify = false;
			AutosaveSpecification specification;
			specification.WriteFile = [&source, &modify](const std::filesystem::path& path, std::span<const std::byte> bytes) -> Status
			{
				ENGINE_TRY(FileSystem::WriteFileAtomic(path, bytes, { .KeepBackup = false }));
				if (modify && path.filename() == "Scene.json")
				{
					ENGINE_TRY_ASSIGN(std::string text, FileSystem::ReadText(source));
					text += "\n";
					ENGINE_TRY(FileSystem::WriteFileAtomic(source, std::as_bytes(std::span(text.data(), text.size())), { .KeepBackup = false }));
				}
				return {};
			};
			AutosaveSetup setup(specification);
			source = setup.Source();
			setup.Dirty();
			setup.Save();
			const std::string before = AutosaveRead(setup.Root() / "Manifest.json");
			modify = true;
			const auto result = setup.Service.Save(AutosaveReason::Periodic);
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Conflict);
			CHECK(AutosaveRead(setup.Root() / "Manifest.json") == before);
		}
		TEST_CASE("Autosave: a clean scene retains its loaded fingerprint across external source changes")
		{
			AutosaveSetup setup;
			AutosaveWrite(setup.Source(), AutosaveRead(setup.Source()) + "\n");
			REQUIRE(setup.Service.Publish().has_value());
			setup.Dirty();
			const auto result = setup.Service.Save(AutosaveReason::Periodic);
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Conflict);
			CHECK_FALSE(FileSystem::Exists(setup.Root() / "Manifest.json"));
		}

		TEST_CASE("Autosave: a changed project fingerprint is not adopted at a later publication")
		{
			AutosaveSetup setup;
			const auto projectFile = setup.Fixture.GetEditor().GetProject().GetProjectFile();
			AutosaveWrite(projectFile, AutosaveRead(projectFile) + "\n");
			const auto clean = setup.Service.Publish();
			REQUIRE_FALSE(clean.has_value());
			CHECK(clean.error().GetCode() == ErrorCode::Conflict);
			setup.Dirty();
			const auto dirty = setup.Service.Save(AutosaveReason::BeforePlay);
			REQUIRE_FALSE(dirty.has_value());
			CHECK(dirty.error().GetCode() == ErrorCode::Conflict);
			CHECK_FALSE(FileSystem::Exists(setup.Root() / "Manifest.json"));
		}

		TEST_CASE("Autosave: a committed settings write advances the captured project fingerprint")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			auto command = ProjectSettingsCommand::CreateFromPatch(editor, Json{ { "Window", { { "Title", "Changed" } } } }, "Title");
			REQUIRE(command.has_value());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
			REQUIRE(setup.Service.Publish().has_value());
			setup.Dirty();
			const auto saved = setup.Save();
			CHECK(setup.Offer().Generation == saved.Generation);
			auto changed = ProjectSettingsCommand::CreateFromPatch(editor, Json{ { "Window", { { "Title", "Changed again" } } } }, "Title again");
			REQUIRE(changed.has_value());
			REQUIRE(editor.Execute(std::move(*changed)).has_value());
			const auto updated = setup.Save();
			CHECK(setup.Offer().Generation == updated.Generation);
			CHECK(AutosaveRead(setup.Root() / updated.Generation / "Metadata.json") != AutosaveRead(setup.Root() / saved.Generation / "Metadata.json"));
		}

		TEST_CASE("Autosave: identical settings patches and their undo redo preserve the admitted file")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			const auto projectFile = editor.GetProject().GetProjectFile();
			const auto bytes = AutosaveRead(projectFile);
			const auto before = FileSystem::GetInfo(projectFile);
			REQUIRE(before.has_value());
			auto command = ProjectSettingsCommand::CreateFromPatch(editor,
				Json{ { "Scripting", { { "BlockPlayOnTypeErrors", editor.GetProject().GetSettings().Scripting.BlockPlayOnTypeErrors } } } }, "Keep script policy");
			REQUIRE(command.has_value());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
			REQUIRE(setup.Service.Publish().has_value());
			REQUIRE(editor.GetHistory().Undo(editor) == 1);
			REQUIRE(setup.Service.Publish().has_value());
			REQUIRE(editor.GetHistory().Redo(editor) == 1);
			REQUIRE(setup.Service.Publish().has_value());
			const auto after = FileSystem::GetInfo(projectFile);
			REQUIRE(after.has_value());
			CHECK(after->ModificationTime == before->ModificationTime);
			CHECK(AutosaveRead(projectFile) == bytes);
			setup.Dirty();
			const auto saved = setup.Service.Save(AutosaveReason::BeforePlay);
			REQUIRE(saved.has_value());
			CHECK(saved->Written);
		}

		TEST_CASE("Autosave: the editor's own project rewrites keep autosave and play available")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			const auto projectFile = editor.GetProject().GetProjectFile();
			const std::string canonical = AutosaveRead(projectFile);
			SUBCASE("a settings change undone before the next publication")
			{
				// An edit.batch rollback or a multi-step undo: A, X and A again, each a real write.
				auto command = ProjectSettingsCommand::CreateFromPatch(editor, Json{ { "Window", { { "Title", "Interim" } } } }, "Interim");
				REQUIRE(command.has_value());
				REQUIRE(editor.Execute(std::move(*command)).has_value());
				REQUIRE(editor.GetHistory().Undo(editor) == 1);
			}
			SUBCASE("a rewrite of the unchanged canonical settings")
			{
				// What project.upgrade writes for settings that were already loaded.
				const auto path = VfsPath::Create("project", FileSystem::PathToUtf8(projectFile.filename()));
				REQUIRE(path.has_value());
				const auto text = ProjectSerializer::SaveToString(editor.GetProject().GetSettings(), editor.GetTypeRegistry());
				REQUIRE(text.has_value());
				REQUIRE(editor.WriteProjectFile(*path, std::as_bytes(std::span(text->data(), text->size()))).has_value());
			}
			AutosaveTouch(projectFile);
			REQUIRE(AutosaveRead(projectFile) == canonical);
			const Status published = setup.Service.Publish();
			REQUIRE_MESSAGE(published.has_value(), published.error().ToString());
			setup.Dirty("AfterRewrite");
			const auto saved = setup.Service.Save(AutosaveReason::BeforePlay);
			REQUIRE_MESSAGE(saved.has_value(), saved.error().ToString());
			CHECK(saved->Written);
			CHECK(setup.Offer().Generation == saved->Generation);
		}

		TEST_CASE("Autosave: a settings change keeps the loaded baseline of an externally changed scene")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			setup.Dirty("DerivedFromTheLoadedFile");
			REQUIRE(setup.Service.Publish().has_value());
			// A pull rewrites the source under the dirty scene: its edits no longer derive from the file on disk.
			AutosaveWrite(setup.Source(), AutosaveRead(setup.Source()) + "\n");
			const Status conflict = setup.Service.Publish();
			REQUIRE_FALSE(conflict.has_value());
			CHECK(conflict.error().GetCode() == ErrorCode::Conflict);
			auto command = ProjectSettingsCommand::CreateFromPatch(editor, Json{ { "Window", { { "Title", "After the pull" } } } }, "Title");
			REQUIRE(command.has_value());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
			const auto saved = setup.Service.Save(AutosaveReason::Periodic);
			REQUIRE_FALSE(saved.has_value());
			CHECK(saved.error().GetCode() == ErrorCode::Conflict);
			CHECK_FALSE(FileSystem::Exists(setup.Root() / "Manifest.json"));
		}

		TEST_CASE("Autosave: a scene that changed on disk before its first publication never becomes its baseline")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			setup.Fixture.CreateAndOpenScene("Assets/Scenes/Other.scene");
			const auto other = setup.Fixture.GetProjectRoot() / "Assets/Scenes/Other.scene";
			editor.Update(0.0);
			AutosaveWrite(other, AutosaveRead(other) + "\n\n");
			for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
			{
				editor.Update(seconds);
				static_cast<void>(editor.GetEngine().GetMainThreadQueue().Drain());
			}
			REQUIRE(editor.IsSceneChangedOnDisk());
			setup.Dirty("DerivedFromTheLoadedFile");
			const auto refused = setup.Service.Save(AutosaveReason::Periodic);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Conflict);
			CHECK_FALSE(FileSystem::Exists(setup.Root() / "Manifest.json"));
			// Reloading adopts the file on disk as the document's baseline again.
			const auto path = editor.GetScenePath();
			auto reloaded = editor.CreateScene("Other");
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(*reloaded, AutosaveRead(other), {}, report).has_value());
			editor.SetScene(std::move(reloaded), path, false);
			setup.Dirty("AfterReload");
			CHECK(setup.Save().Files == std::vector<std::string>{ "Assets/Scenes/Other.scene" });
		}

		TEST_CASE("Autosave: unchanged publications read no file bytes and a write verifies its sources once")
		{
			std::map<std::string, uint32_t> reads;
			AutosaveSpecification specification;
			specification.ReadObserver = [&reads](const std::filesystem::path& path)
			{
				++reads[FileSystem::PathToUtf8(path.filename())];
			};
			AutosaveSetup setup(specification);
			const std::string project = FileSystem::PathToUtf8(setup.Fixture.GetEditor().GetProject().GetProjectFile().filename());
			reads.clear();
			// Edits and the safe points of the frames between them: no source or project bytes are read.
			setup.Dirty("First");
			for (int frame = 0; frame < 4; ++frame)
				REQUIRE(setup.Service.Publish().has_value());
			setup.Dirty("Second");
			REQUIRE(setup.Service.Publish().has_value());
			CHECK(reads.empty());
			setup.Save();
			CHECK(reads["Main.scene"] == 1);
			CHECK(reads[project] == 1);
			reads.clear();
			REQUIRE(setup.Service.Publish().has_value());
			CHECK(reads.empty());
			// An external change shows in the source's size and write time: one read of the changed file, after which the
			// conflict is kept without reading anything again.
			AutosaveWrite(setup.Source(), AutosaveRead(setup.Source()) + "\n");
			for (int frame = 0; frame < 3; ++frame)
				CHECK(setup.Service.Publish().error().GetCode() == ErrorCode::Conflict);
			CHECK(reads == std::map<std::string, uint32_t>{ { "Main.scene", 1 } });
		}

		TEST_CASE("Autosave: explicit save cleanup keeps foreign and unreadable generations")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			setup.Dirty("Foreign");
			const auto foreign = setup.Save();
			// A generation of the same files under another canonical project path: the project was moved or renamed.
			Json metadata = AutosaveReadJson(setup.Root() / foreign.Generation / "Metadata.json");
			metadata["ProjectFile"] = FileSystem::PathToUtf8(setup.Fixture.GetDirectory().GetPath() / "Moved" / "Autosave.eproj");
			AutosaveWriteJson(setup.Root() / foreign.Generation / "Metadata.json", metadata);
			setup.Dirty("Corrupt");
			const auto corrupt = setup.Save();
			AutosaveWrite(setup.Root() / corrupt.Generation / "Scene.json", "{}");
			setup.Dirty("Matching");
			const auto matching = setup.Save();
			const auto bytes = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(bytes.has_value());
			AutosaveWrite(setup.Source(), *bytes);
			REQUIRE(editor.MarkSceneSaved(*editor.GetScenePath()).has_value());
			const Status discarded = setup.Service.DiscardSavedRecovery();
			REQUIRE_MESSAGE(discarded.has_value(), discarded.error().ToString());
			CHECK_FALSE(FileSystem::Exists(setup.Root() / matching.Generation));
			CHECK(FileSystem::Exists(setup.Root() / foreign.Generation));
			CHECK(FileSystem::Exists(setup.Root() / corrupt.Generation));
			const Json manifest = AutosaveReadJson(setup.Root() / "Manifest.json");
			REQUIRE(manifest["Generations"].size() == 2);
			CHECK(manifest["Generations"][0]["Generation"] == corrupt.Generation);
			CHECK(manifest["Generations"][1]["Generation"] == foreign.Generation);
		}

		TEST_CASE("Autosave: a retired generation that cannot be removed never fails the write and is swept later")
		{
			SUBCASE("already removed")
			{
				AutosaveSetup setup({ .RetainedGenerations = 1 });
				setup.Dirty("First");
				const auto first = setup.Save();
				REQUIRE(FileSystem::Remove(setup.Root() / first.Generation).has_value());
				setup.Dirty("Second");
				const auto second = setup.Save();
				CHECK(setup.Offer().Generation == second.Generation);
			}
			SUBCASE("left behind after an earlier publication")
			{
				AutosaveSetup setup;
				setup.Dirty("First");
				const auto first = setup.Save();
				setup.Dirty("Second");
				const auto second = setup.Save();
				// The manifest retired `first`, but its directory could not be removed then.
				Json manifest = AutosaveReadJson(setup.Root() / "Manifest.json");
				REQUIRE(manifest["Generations"][0]["Generation"] == second.Generation);
				manifest["Generations"] = Json::array({ manifest["Generations"][0] });
				AutosaveWriteJson(setup.Root() / "Manifest.json", manifest);
				setup.Dirty("Third");
				const auto third = setup.Save();
				CHECK_FALSE(FileSystem::Exists(setup.Root() / first.Generation));
				CHECK(FileSystem::Exists(setup.Root() / second.Generation));
				CHECK(FileSystem::Exists(setup.Root() / third.Generation));
			}
		}

		TEST_CASE("Autosave: a final write never duplicates the newest generation of the same snapshot")
		{
			AutosaveSetup setup;
			setup.Dirty("Durable");
			// A main-thread GPU failure's refresh, then the fatal hook on the same published revision.
			const auto refreshed = setup.Service.Save(AutosaveReason::FatalError);
			REQUIRE(refreshed.has_value());
			REQUIRE(refreshed->Written);
			CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::Saved);
			const auto exit = setup.Service.Save(AutosaveReason::Exit);
			REQUIRE(exit.has_value());
			CHECK_FALSE(exit->Written);
			Json manifest = AutosaveReadJson(setup.Root() / "Manifest.json");
			CHECK(manifest["Generations"].size() == 1);
			CHECK(manifest["Sequence"] == 1);
			// A newer committed revision is written.
			setup.Dirty("Newer");
			REQUIRE(setup.Service.Publish().has_value());
			CHECK(setup.Service.WriteFatalSnapshot() == AutosaveFatalResult::Saved);
			manifest = AutosaveReadJson(setup.Root() / "Manifest.json");
			CHECK(manifest["Generations"].size() == 2);
			CHECK(AutosaveRead(setup.Root() / setup.Offer().Generation / "Scene.json").contains("Newer"));
		}

		// Docs/Decisions/0031-m20-contract.md decision 15 (E-SCENES; Roadmap M20 acceptance "autosave is isolated per open
		// scene"): a save writes one generation per dirty open scene, in tab order; each scene is offered and recovered on its
		// own; an explicit save of one discards only that scene's generations.
		TEST_CASE("Autosave: each dirty open scene is written and recovered on its own")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			setup.Dirty("InMain");
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(main != NoOpenScene);
			const Result<VfsPath> secondPath = VfsPath::Parse("project://Assets/Scenes/Second.scene");
			REQUIRE(secondPath.has_value());
			const Result<OpenSceneId> second = editor.AddScene(editor.CreateScene("Second"), *secondPath);
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			setup.Dirty("InSecond");
			REQUIRE(setup.Service.Publish().has_value());

			const AutosaveWriteResult written = setup.Save();
			CHECK(written.Files == std::vector<std::string>{ "Assets/Scenes/Main.scene", "Assets/Scenes/Second.scene" });
			const auto offers = setup.Service.FindRecoveries();
			REQUIRE_MESSAGE(offers.has_value(), offers.error().ToString());
			REQUIRE(offers->size() == 2);
			CHECK((*offers)[0].ScenePath == "Assets/Scenes/Main.scene");
			CHECK((*offers)[1].ScenePath == "Assets/Scenes/Second.scene");
			CHECK((*offers)[0].Generation != (*offers)[1].Generation);
			CHECK(AutosaveRead(setup.Root() / (*offers)[0].Generation / "Scene.json").contains("InMain"));
			CHECK_FALSE(AutosaveRead(setup.Root() / (*offers)[0].Generation / "Scene.json").contains("InSecond"));
			CHECK(AutosaveRead(setup.Root() / (*offers)[1].Generation / "Scene.json").contains("InSecond"));

			// Saving the second scene discards its recovery only.
			const auto bytes = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(bytes.has_value());
			AutosaveWrite(setup.Fixture.GetProjectRoot() / "Assets/Scenes/Second.scene", *bytes);
			REQUIRE(editor.MarkSceneSaved(*secondPath).has_value());
			REQUIRE(setup.Service.DiscardSavedRecovery().has_value());
			const auto remaining = setup.Service.FindRecoveries();
			REQUIRE(remaining.has_value());
			REQUIRE(remaining->size() == 1);
			CHECK((*remaining)[0].ScenePath == "Assets/Scenes/Main.scene");

			// Recovering the first scene replaces that open scene, dirty with an empty history; the second is untouched.
			REQUIRE(setup.Service.Recover((*remaining)[0]).has_value());
			CHECK(editor.GetOpenScenes().size() == 2);
			CHECK(editor.GetScene().GetName() == "Main");
			CHECK(editor.IsSceneDirty());
			CHECK_FALSE(editor.GetHistory().CanUndo());
			const Result<OpenSceneInfo> secondInfo = editor.DescribeOpenScene(*second);
			REQUIRE(secondInfo.has_value());
			CHECK_FALSE(secondInfo->Dirty);
		}

		// Decision 15 (Roadmap M20 acceptance "an injected device loss with two open scenes autosaves both"): the fatal path
		// writes every published dirty open scene from its own fixed slot.
		TEST_CASE("Autosave: a fatal snapshot writes every dirty open scene")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			setup.Dirty("FatalMain");
			const Result<VfsPath> secondPath = VfsPath::Parse("project://Assets/Scenes/Second.scene");
			REQUIRE(secondPath.has_value());
			const Result<OpenSceneId> second = editor.AddScene(editor.CreateScene("Second"), *secondPath);
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			setup.Dirty("FatalSecond");
			REQUIRE(setup.Service.Publish().has_value());

			AutosaveFatalResult result{};
			std::jthread worker([&setup, &result]()
			{
				result = setup.Service.WriteFatalSnapshot();
			});
			worker.join();
			CHECK(result == AutosaveFatalResult::Saved);
			const auto offers = setup.Service.FindRecoveries();
			REQUIRE_MESSAGE(offers.has_value(), offers.error().ToString());
			REQUIRE(offers->size() == 2);
			CHECK(AutosaveRead(setup.Root() / (*offers)[0].Generation / "Scene.json").contains("FatalMain"));
			CHECK(AutosaveRead(setup.Root() / (*offers)[1].Generation / "Scene.json").contains("FatalSecond"));
			// The manifest was published once, after both generations.
			const Json manifest = AutosaveReadJson(setup.Root() / "Manifest.json");
			CHECK(manifest["Generations"].size() == 2);
		}

		// Decision 15 (isolation): a scene that cannot be autosaved (its source changed under its edits) reports its conflict,
		// and the other open scenes are written all the same.
		TEST_CASE("Autosave: a scene whose source conflicts does not keep the other open scenes from being written")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			setup.Dirty("InMain");
			const Result<VfsPath> secondPath = VfsPath::Parse("project://Assets/Scenes/Second.scene");
			REQUIRE(secondPath.has_value());
			REQUIRE(editor.AddScene(editor.CreateScene("Second"), *secondPath).has_value());
			setup.Dirty("InSecond");
			REQUIRE(setup.Service.Publish().has_value());
			AutosaveWrite(setup.Source(), AutosaveRead(setup.Source()) + "\n");

			const auto saved = setup.Service.Save(AutosaveReason::Periodic);
			REQUIRE_FALSE(saved.has_value());
			CHECK(saved.error().GetCode() == ErrorCode::Conflict);
			const auto offers = setup.Service.FindRecoveries();
			REQUIRE_MESSAGE(offers.has_value(), offers.error().ToString());
			REQUIRE(offers->size() == 1);
			CHECK((*offers)[0].ScenePath == "Assets/Scenes/Second.scene");
			CHECK(AutosaveRead(setup.Root() / (*offers)[0].Generation / "Scene.json").contains("InSecond"));
		}

		// Decision 15: retention keeps RetainedGenerations of each scene, so another scene's writes never retire one.
		TEST_CASE("Autosave: retention keeps the newest generations of each scene")
		{
			AutosaveSetup setup({ .RetainedGenerations = 1 });
			EditorContext& editor = setup.Fixture.GetEditor();
			setup.Dirty("MainFirst");
			const auto first = setup.Save();
			setup.Dirty("MainSecond");
			const auto second = setup.Save();
			CHECK_FALSE(FileSystem::Exists(setup.Root() / first.Generation));
			const Result<VfsPath> secondPath = VfsPath::Parse("project://Assets/Scenes/Second.scene");
			REQUIRE(secondPath.has_value());
			REQUIRE(editor.AddScene(editor.CreateScene("Second"), *secondPath).has_value());
			setup.Dirty("InSecond");
			const auto both = setup.Save();
			CHECK(both.Files == std::vector<std::string>{ "Assets/Scenes/Main.scene", "Assets/Scenes/Second.scene" });
			CHECK_FALSE(FileSystem::Exists(setup.Root() / second.Generation));
			const Json manifest = AutosaveReadJson(setup.Root() / "Manifest.json");
			CHECK(manifest["Generations"].size() == 2);
			const auto offers = setup.Service.FindRecoveries();
			REQUIRE(offers.has_value());
			REQUIRE(offers->size() == 2);
			CHECK(AutosaveRead(setup.Root() / (*offers)[0].Generation / "Scene.json").contains("MainSecond"));
			CHECK(AutosaveRead(setup.Root() / (*offers)[1].Generation / "Scene.json").contains("InSecond"));
		}

		// Decision 15: a recovery of a scene that is not open opens it as an additional open scene, the active one.
		TEST_CASE("Autosave: recovering a scene that is not open adds it as the active open scene")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			setup.Dirty("RecoverMain");
			setup.Save();
			const auto offer = setup.Offer();
			setup.Fixture.CreateAndOpenScene("Assets/Scenes/Other.scene");
			const OpenSceneId other = editor.GetActiveSceneId();
			REQUIRE(setup.Service.Recover(offer).has_value());
			REQUIRE(editor.GetOpenScenes().size() == 2);
			CHECK(editor.GetOpenScenes().front() == other);
			CHECK(editor.GetActiveSceneId() == editor.GetOpenScenes().back());
			CHECK(editor.GetScene().GetName() == "Main");
			CHECK(editor.IsSceneDirty());
			CHECK(editor.GetScene().FindEntityByPath("/RecoverMain").IsValid());
			CHECK_FALSE(editor.DescribeOpenScene(other)->Dirty);
		}

		// Decision 15: launcher recovery of several scenes preflights every offer, then opens them all dirty with empty
		// histories, the first active; an empty list and two offers of one scene are refused before anything opens.
		TEST_CASE("Autosave: launcher recovery opens every offered scene with the first active")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			setup.Dirty("RecoverMain");
			const Result<VfsPath> secondPath = VfsPath::Parse("project://Assets/Scenes/Second.scene");
			REQUIRE(secondPath.has_value());
			REQUIRE(editor.AddScene(editor.CreateScene("Second"), *secondPath).has_value());
			setup.Dirty("RecoverSecond");
			setup.Save();
			const auto offers = setup.Service.FindRecoveries();
			REQUIRE(offers.has_value());
			REQUIRE(offers->size() == 2);
			const auto projectFile = editor.GetProject().GetProjectFile();
			REQUIRE(setup.Service.Reset());
			REQUIRE(editor.CloseProject().has_value());
			const auto open = [&editor, &projectFile]()
			{
				auto project = ProjectManager::OpenProject(projectFile, {}, editor.GetTypeRegistry());
				REQUIRE_MESSAGE(project.has_value(), project.error().ToString());
				return std::move(*project);
			};

			CHECK(setup.Service.OpenRecoveredProject(open(), std::span<const AutosaveRecoveryInfo>()).error().GetCode() == ErrorCode::InvalidArgument);
			const std::vector<AutosaveRecoveryInfo> repeated{ (*offers)[0], (*offers)[0] };
			CHECK(setup.Service.OpenRecoveredProject(open(), repeated).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(editor.HasProject());

			REQUIRE(setup.Service.OpenRecoveredProject(open(), *offers).has_value());
			const std::vector<OpenSceneId> scenes = editor.GetOpenScenes();
			REQUIRE(scenes.size() == 2);
			CHECK(editor.GetActiveSceneId() == scenes[0]);
			CHECK(editor.GetScene().GetName() == "Main");
			CHECK(editor.GetScene().FindEntityByPath("/RecoverMain").IsValid());
			CHECK(editor.IsSceneDirty());
			CHECK_FALSE(editor.GetHistory().CanUndo());
			const Result<OpenSceneInfo> second = editor.DescribeOpenScene(scenes[1]);
			REQUIRE(second.has_value());
			CHECK(second->Dirty);
			CHECK(second->Path == *secondPath);
			REQUIRE(editor.FindScene(scenes[1]) != nullptr);
			CHECK(editor.FindScene(scenes[1])->FindEntityByPath("/RecoverSecond").IsValid());
		}

		// Decision 15: FindRecoveries reads each scene's generations newest first until that scene has its offer; an older
		// generation of a scene with an offer is read for its metadata only, and one whose metadata cannot be read fails the
		// inspection, as an unreadable generation fails FindRecovery.
		TEST_CASE("Autosave: FindRecoveries reads an older generation of an offered scene for its metadata only")
		{
			AutosaveSetup setup;
			setup.Dirty("Older");
			const auto older = setup.Save();
			setup.Dirty("Newer");
			const auto newer = setup.Save();
			AutosaveWrite(setup.Root() / older.Generation / "Scene.json", "{}");
			const auto offers = setup.Service.FindRecoveries();
			REQUIRE_MESSAGE(offers.has_value(), offers.error().ToString());
			REQUIRE(offers->size() == 1);
			CHECK((*offers)[0].Generation == newer.Generation);
			AutosaveWrite(setup.Root() / older.Generation / "Metadata.json", "{");
			const auto broken = setup.Service.FindRecoveries();
			REQUIRE_FALSE(broken.has_value());
			CHECK(broken.error().GetCode() == ErrorCode::Parse);
		}

		// Decision 15: a scene whose newest generation went stale (its tab closed with its changes discarded, then the project
		// or its file changed) has no offer, and the other scenes are still offered and recovered; the conflict is the
		// answer only when no scene has an offer, as it is FindRecovery's.
		TEST_CASE("Autosave: a stale generation of one scene leaves the other scenes recoverable")
		{
			AutosaveSetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			const Result<VfsPath> secondPath = VfsPath::Parse("project://Assets/Scenes/Second.scene");
			REQUIRE(secondPath.has_value());
			const Result<OpenSceneId> second = editor.AddScene(editor.CreateScene("Second"), *secondPath);
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			setup.Dirty("InSecond");
			REQUIRE(setup.Service.Publish().has_value());
			const AutosaveWriteResult stale = setup.Save();
			REQUIRE(editor.CloseScene(*second).has_value());
			REQUIRE(setup.Service.Publish().has_value());
			SUBCASE("a settings write after its generation")
			{
				auto command = ProjectSettingsCommand::CreateFromPatch(editor, Json{ { "Window", { { "Title", "Changed" } } } }, "Title");
				REQUIRE(command.has_value());
				REQUIRE(editor.Execute(std::move(*command)).has_value());
				REQUIRE(setup.Service.Publish().has_value());
			}
			SUBCASE("a changed source")
			{
				AutosaveWrite(setup.Fixture.GetProjectRoot() / "Assets/Scenes/Second.scene", "{}");
			}
			{
				Test::ExpectLog warned(LogLevel::Warn, "Assets/Scenes/Second.scene");
				const auto alone = setup.Service.FindRecoveries();
				REQUIRE_FALSE(alone.has_value());
				CHECK(alone.error().GetCode() == ErrorCode::Conflict);
				CHECK(warned.GetMatchCount() == 1);
			}

			setup.Dirty("InMain");
			REQUIRE(setup.Service.Publish().has_value());
			const AutosaveWriteResult valid = setup.Save();
			CHECK(valid.Files == std::vector<std::string>{ "Assets/Scenes/Main.scene" });
			Test::ExpectLog warned(LogLevel::Warn, "Assets/Scenes/Second.scene");
			const auto offers = setup.Service.FindRecoveries();
			REQUIRE_MESSAGE(offers.has_value(), offers.error().ToString());
			REQUIRE(offers->size() == 1);
			CHECK((*offers)[0].ScenePath == "Assets/Scenes/Main.scene");
			CHECK((*offers)[0].Generation == valid.Generation);
			CHECK(warned.GetMatchCount() == 1);
			CHECK(FileSystem::Exists(setup.Root() / stale.Generation));

			const auto projectFile = editor.GetProject().GetProjectFile();
			REQUIRE(setup.Service.Reset());
			REQUIRE(editor.CloseProject().has_value());
			auto project = ProjectManager::OpenProject(projectFile, {}, editor.GetTypeRegistry());
			REQUIRE_MESSAGE(project.has_value(), project.error().ToString());
			const Status opened = setup.Service.OpenRecoveredProject(std::move(*project), *offers);
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			REQUIRE(editor.GetOpenScenes().size() == 1);
			CHECK(editor.GetScene().GetName() == "Main");
			CHECK(editor.GetScene().FindEntityByPath("/InMain").IsValid());
			CHECK(editor.IsSceneDirty());
		}
	}

}
