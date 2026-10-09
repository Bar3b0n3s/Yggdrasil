#include "TestsPCH.h"
#include "EditorCore/Autosave/Autosave.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Platform/ProjectLock.h"
#include "Engine/Platform/Process.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <semaphore>
#include <thread>

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
	}

}
