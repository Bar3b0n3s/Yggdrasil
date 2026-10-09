#include "TestsPCH.h"
#include "EditorCore/Automation/RecoveryMethods.h"

#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/ProjectLock.h"
#include "Engine/Scene/Entity.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

namespace Engine {

	namespace {

		struct RecoverySetup
		{
			Test::EditorTestFixture Fixture{ "RecoveryMethods" };
			Scope<Autosave> Service{};
			Scope<Test::AutomationTestClient> Client{};
			std::filesystem::path ProjectFile{};
			std::filesystem::path Payload{};
			std::vector<std::filesystem::path> PreservedPayloads{};
			AutosaveRecoveryInfo Recovery{};
			explicit RecoverySetup(bool untitled = false, bool withService = true)
			{
				Fixture.CreateAndOpenProject();
				Fixture.CreateAndOpenScene();
				EditorContext& editor = Fixture.GetEditor();
				ProjectFile = editor.GetProject().GetProjectFile();
				{
					Autosave service(editor);
					REQUIRE(service.Publish().has_value());
					SceneEdit edit(editor, "Recover");
					static_cast<void>(editor.GetScene().CreateEntity("RecoverMe"));
					REQUIRE(edit.Commit().has_value());
					const auto result = service.Save(AutosaveReason::BeforePlay);
					REQUIRE(result.has_value());
					Payload = Fixture.GetProjectRoot() / "Library/Autosave" / result->Generation / "Scene.json";
					if (untitled)
					{
						PreservedPayloads.push_back(Payload);
						editor.SetScene(editor.CreateScene("OtherUntitled"), std::nullopt, true);
						const auto other = service.Save(AutosaveReason::BeforePlay);
						REQUIRE(other.has_value());
						REQUIRE(other->Written);
						PreservedPayloads.push_back(Fixture.GetProjectRoot() / "Library/Autosave" / other->Generation / "Scene.json");
						editor.SetScene(editor.CreateScene("RecoveredUntitled"), std::nullopt, true);
						const auto last = service.Save(AutosaveReason::BeforePlay);
						REQUIRE(last.has_value());
						REQUIRE(last->Written);
						Payload = Fixture.GetProjectRoot() / "Library/Autosave" / last->Generation / "Scene.json";
					}
					const auto offer = service.FindRecovery();
					REQUIRE(offer.has_value());
					REQUIRE(offer->has_value());
					Recovery = **offer;
					REQUIRE(service.Reset());
				}
				REQUIRE(editor.CloseProject());
				Service = CreateScope<Autosave>(editor);
				auto specification = Test::MakeTestServerSpecification();
				specification.AutosaveService = withService ? Service.get() : nullptr;
				Client = CreateScope<Test::AutomationTestClient>(editor, specification);
			}
			Result<Json> Open(bool recover)
			{
				return Client->Call("project.open", Json{ { "path", FileSystem::PathToUtf8(ProjectFile) }, { "recover", recover } });
			}
		};

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("RecoveryMethods: project.open recover adopts a validated autosave")
		{
			RecoverySetup setup;
			const auto source = setup.Fixture.GetProjectRoot() / "Assets/Scenes/Main.scene";
			const auto before = FileSystem::ReadText(source);
			REQUIRE(before.has_value());
			const auto result = setup.Open(true);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK((*result)["recovered"] == true);
			CHECK((*result)["recoveryAvailable"] == true);
			CHECK(setup.Fixture.GetEditor().IsSceneDirty());
			CHECK(setup.Fixture.GetEditor().GetScene().GetEntityCount() == 1);
			CHECK_FALSE(setup.Fixture.GetEditor().GetHistory().CanUndo());
			CHECK(*FileSystem::ReadText(source) == *before);
		}

		TEST_CASE("RecoveryMethods: project.open without recover reports the offer and leaves source intact")
		{
			RecoverySetup setup;
			const auto result = setup.Open(false);
			REQUIRE(result.has_value());
			CHECK((*result)["recovered"] == false);
			CHECK((*result)["recoveryAvailable"] == true);
			CHECK(setup.Fixture.GetEditor().HasProject());
			CHECK_FALSE(setup.Fixture.GetEditor().HasScene());
			CHECK(*ProjectLock::IsHeld(setup.Fixture.GetProjectRoot() / "Library/Editor.lock"));
		}

		TEST_CASE("RecoveryMethods: failed recovery releases the project lock and stays in launcher")
		{
			RecoverySetup setup;
			REQUIRE(FileSystem::WriteFileAtomic(setup.Payload, {}, { .KeepBackup = false }).has_value());
			CHECK_FALSE(setup.Open(true).has_value());
			CHECK_FALSE(setup.Fixture.GetEditor().HasProject());
			CHECK(ProjectLock::Acquire(setup.Fixture.GetProjectRoot() / "Library/Editor.lock").has_value());
		}

		TEST_CASE("RecoveryMethods: a read-only editor refuses recovery")
		{
			RecoverySetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			auto project = ProjectManager::OpenProject(setup.ProjectFile,
				{ .ReadOnly = true, .ReadOnlyCacheDirectory = setup.Fixture.GetDirectory().GetPath() / "Cache" }, editor.GetTypeRegistry());
			REQUIRE(project.has_value());
			REQUIRE(editor.OpenProject(std::move(*project)).has_value());
			const auto result = setup.Open(true);
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::PermissionDenied);
			CHECK_FALSE(editor.HasScene());
		}

		TEST_CASE("RecoveryMethods: recovery choice never reopens an already-open project")
		{
			RecoverySetup setup;
			REQUIRE(setup.Open(false).has_value());
			CHECK(setup.Open(true).error().GetCode() == ErrorCode::InvalidState);
			Autosave service(setup.Fixture.GetEditor());
			const auto offer = service.FindRecovery();
			REQUIRE(offer.has_value());
			REQUIRE(offer->has_value());
			REQUIRE(service.Recover(**offer).has_value());
			CHECK(setup.Fixture.GetEditor().IsSceneDirty());
			CHECK(*ProjectLock::IsHeld(setup.Fixture.GetProjectRoot() / "Library/Editor.lock"));
		}

		TEST_CASE("RecoveryMethods: an RPC-recovered untitled scene retains its token through first save")
		{
			RecoverySetup setup(true);
			REQUIRE(setup.Service->Reset());
			std::vector<std::string> preserved;
			for (const auto& path : setup.PreservedPayloads)
			{
				const auto bytes = FileSystem::ReadText(path);
				REQUIRE(bytes.has_value());
				preserved.push_back(*bytes);
			}
			const auto opened = setup.Open(true);
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			REQUIRE((*opened)["recovered"] == true);
			CHECK_FALSE(setup.Fixture.GetEditor().GetScenePath().has_value());
			CHECK(setup.Fixture.GetEditor().GetScene().GetName() == "RecoveredUntitled");
			CHECK(setup.Service->WriteFatalSnapshot() == AutosaveFatalResult::Disabled);
			// The first normal safe point must retain the adopted token rather than inventing a new untitled identity.
			REQUIRE(setup.Service->Publish().has_value());
			const auto saved = setup.Client->Call("scene.save", Json{ { "path", "Assets/Scenes/FirstSave.scene" } });
			REQUIRE_MESSAGE(saved.has_value(), saved.error().ToString());
			// The host calls this immediately after a successful explicit save, before another publication.
			REQUIRE(setup.Service->DiscardSavedRecovery().has_value());
			CHECK_FALSE(FileSystem::Exists(setup.Payload));
			for (size_t index = 0; index < setup.PreservedPayloads.size(); ++index)
			{
				const auto bytes = FileSystem::ReadText(setup.PreservedPayloads[index]);
				REQUIRE(bytes.has_value());
				CHECK(*bytes == preserved[index]);
			}
			const auto remaining = setup.Service->FindRecovery();
			REQUIRE(remaining.has_value());
			REQUIRE(remaining->has_value());
			CHECK((**remaining).SceneName == "OtherUntitled");
		}

		TEST_CASE("RecoveryMethods: an available recovery requires the host service before opening mounts")
		{
			RecoverySetup setup(false, false);
			const auto result = setup.Open(true);
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Unsupported);
			CHECK_FALSE(setup.Fixture.GetEditor().HasProject());
			CHECK_FALSE(setup.Fixture.GetEditor().GetVfs().IsMounted("project"));
			CHECK_FALSE(setup.Fixture.GetEditor().GetVfs().IsMounted("cache"));
			CHECK(ProjectLock::Acquire(setup.Fixture.GetProjectRoot() / "Library/Editor.lock").has_value());
			CHECK(FileSystem::Exists(setup.Payload));
		}

		TEST_CASE("RecoveryMethods: a project without a recovery opens normally without a service")
		{
			RecoverySetup setup(false, false);
			REQUIRE(FileSystem::Remove(setup.Payload.parent_path().parent_path() / "Manifest.json").has_value());
			const auto result = setup.Open(true);
			REQUIRE(result.has_value());
			CHECK((*result)["recovered"] == false);
			CHECK((*result)["recoveryAvailable"] == false);
			CHECK(setup.Fixture.GetEditor().HasProject());
			CHECK_FALSE(setup.Fixture.GetEditor().HasScene());
		}

		TEST_CASE("RecoveryMethods: invalid launcher offers release the transferred lock without changing mounts")
		{
			RecoverySetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			auto offer = setup.Recovery;
			ErrorCode expected = ErrorCode::Validation;
			SUBCASE("escaping generation")
			{
				offer.Generation = "../Escape";
			}
			SUBCASE("changed scene identity")
			{
				offer.SceneName = "Forged";
			}
			SUBCASE("changed revision")
			{
				++offer.SceneRevision;
			}
			SUBCASE("changed source fingerprint")
			{
				const auto source = setup.Fixture.GetProjectRoot() / "Assets/Scenes/Main.scene";
				const auto before = FileSystem::ReadText(source);
				REQUIRE(before.has_value());
				const std::string changed = *before + "\n";
				REQUIRE(FileSystem::WriteFileAtomic(source, std::as_bytes(std::span(changed.data(), changed.size())), { .KeepBackup = false }).has_value());
				expected = ErrorCode::Conflict;
			}
			SUBCASE("malformed scene with consistent metadata")
			{
				const auto metadataPath = setup.Payload.parent_path() / "Metadata.json";
				const auto text = FileSystem::ReadText(metadataPath);
				REQUIRE(text.has_value());
				auto metadata = JsonReader::Parse(*text);
				REQUIRE(metadata.has_value());
				const std::string malformed = "{";
				(*metadata)["PayloadSize"] = malformed.size();
				(*metadata)["PayloadHash"] = XXH64(malformed);
				const auto updated = JsonWriter::Write(*metadata);
				REQUIRE(updated.has_value());
				REQUIRE(FileSystem::WriteFileAtomic(metadataPath, std::as_bytes(std::span(updated->data(), updated->size())), { .KeepBackup = false }).has_value());
				REQUIRE(FileSystem::WriteFileAtomic(setup.Payload, std::as_bytes(std::span(malformed.data(), malformed.size())), { .KeepBackup = false }).has_value());
				expected = ErrorCode::Parse;
			}
			auto project = ProjectManager::OpenProject(setup.ProjectFile, {}, editor.GetTypeRegistry());
			REQUIRE(project.has_value());
			const auto result = setup.Service->OpenRecoveredProject(std::move(*project), offer);
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == expected);
			CHECK_FALSE(editor.HasProject());
			CHECK_FALSE(editor.HasScene());
			CHECK_FALSE(editor.GetVfs().IsMounted("project"));
			CHECK_FALSE(editor.GetVfs().IsMounted("cache"));
			CHECK(ProjectLock::Acquire(setup.Fixture.GetProjectRoot() / "Library/Editor.lock").has_value());
			CHECK(setup.Service->WriteFatalSnapshot() == AutosaveFatalResult::Disabled);
			CHECK(setup.Service->Reset());
		}

		TEST_CASE("RecoveryMethods: a mount failure leaves the existing mount and recovery service intact")
		{
			RecoverySetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			VirtualFileSystem& vfs = editor.GetVfs();
			REQUIRE(vfs.Mount("cache", CreateScope<MemoryMount>()).has_value());
			const auto marker = VfsPath::Parse("cache://Marker.txt");
			REQUIRE(marker.has_value());
			const std::string before = "preserved";
			REQUIRE(vfs.WriteFileAtomic(*marker, std::as_bytes(std::span(before.data(), before.size()))).has_value());
			const auto result = setup.Open(true);
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::AlreadyExists);
			CHECK_FALSE(editor.HasProject());
			CHECK_FALSE(vfs.IsMounted("project"));
			const auto actual = vfs.ReadText(*marker);
			REQUIRE(actual.has_value());
			CHECK(*actual == before);
			CHECK(ProjectLock::Acquire(setup.Fixture.GetProjectRoot() / "Library/Editor.lock").has_value());
			CHECK(setup.Service->WriteFatalSnapshot() == AutosaveFatalResult::Disabled);
			REQUIRE(vfs.Unmount("cache").has_value());
			// A failed open did not bind this service to a stale epoch or leave its writer lease claimed.
			REQUIRE(setup.Open(true).has_value());
			REQUIRE(setup.Service->Publish().has_value());
			CHECK(setup.Service->WriteFatalSnapshot() == AutosaveFatalResult::Saved);
		}

		TEST_CASE("RecoveryMethods: launcher recovery requires a writable locked project")
		{
			RecoverySetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			SUBCASE("no transferred project")
			{
				const auto result = setup.Service->OpenRecoveredProject({}, setup.Recovery);
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::InvalidArgument);
			}
			SUBCASE("read-only project")
			{
				auto project = ProjectManager::OpenProject(setup.ProjectFile,
					{ .ReadOnly = true, .ReadOnlyCacheDirectory = setup.Fixture.GetDirectory().GetPath() / "PrivateCache" }, editor.GetTypeRegistry());
				REQUIRE(project.has_value());
				const auto result = setup.Service->OpenRecoveredProject(std::move(*project), setup.Recovery);
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::PermissionDenied);
			}
			CHECK_FALSE(editor.HasProject());
			CHECK_FALSE(editor.GetVfs().IsMounted("project"));
			CHECK_FALSE(editor.GetVfs().IsMounted("cache"));
			CHECK(setup.Service->WriteFatalSnapshot() == AutosaveFatalResult::Disabled);
			CHECK(ProjectLock::Acquire(setup.Fixture.GetProjectRoot() / "Library/Editor.lock").has_value());
		}

		TEST_CASE("RecoveryMethods: launcher recovery never replaces a currently open project")
		{
			RecoverySetup setup;
			RecoverySetup other;
			REQUIRE(setup.Open(true).has_value());
			REQUIRE(setup.Service->Publish().has_value());
			EditorContext& editor = setup.Fixture.GetEditor();
			const uint64_t revision = editor.GetRevision();
			auto project = ProjectManager::OpenProject(other.ProjectFile, {}, editor.GetTypeRegistry());
			REQUIRE(project.has_value());
			const auto result = setup.Service->OpenRecoveredProject(std::move(*project), other.Recovery);
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::InvalidState);
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetProject().GetProjectFile() == setup.ProjectFile);
			CHECK(*ProjectLock::IsHeld(setup.Fixture.GetProjectRoot() / "Library/Editor.lock"));
			CHECK(ProjectLock::Acquire(other.Fixture.GetProjectRoot() / "Library/Editor.lock").has_value());
			CHECK(setup.Service->WriteFatalSnapshot() == AutosaveFatalResult::Saved);
		}
	}

}
