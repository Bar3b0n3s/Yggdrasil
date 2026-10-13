#include "TestsPCH.h"

#include "Editor/Private/EditorHostRecovery.h"
#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <chrono>

namespace Engine {

	namespace {

		struct HostRecoverySetup
		{
			Test::EditorTestFixture Fixture{ "HostRecovery" };
			Autosave Saves{ Fixture.GetEditor() };
			EditorHostRecovery Host{ Fixture.GetEditor(), Saves };
			std::string Original{};
			HostRecoverySetup()
			{
				Fixture.CreateAndOpenProject();
				Fixture.CreateAndOpenScene();
				EditorContext& editor = Fixture.GetEditor();
				const auto before = SceneSerializer::SaveToString(editor.GetScene());
				REQUIRE(before.has_value());
				Original = *before;
				{
					SceneEdit edit(editor, "Recover this edit");
					(void)editor.GetScene().CreateEntity("RecoverThisEntity");
					REQUIRE(edit.Commit().has_value());
				}
				const auto saved = Saves.Save(AutosaveReason::BeforePlay);
				REQUIRE(saved.has_value());
				REQUIRE(saved->Written);
				REQUIRE(Saves.Reset());
				auto clean = editor.CreateScene("Main");
				LoadReport report;
				REQUIRE(SceneSerializer::LoadFromString(*clean, Original, {}, report).has_value());
				editor.SetScene(std::move(clean), editor.GetScenePath());
				REQUIRE(Host.Inspect(1).has_value());
				REQUIRE(Host.GetOffer().has_value());
			}
		};

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorHostRecovery: queued acceptance uses the owned offer on the existing project")
		{
			HostRecoverySetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			const LoadedProject* project = &editor.GetProject();
			auto callerOffer = *setup.Host.GetOffer();
			callerOffer.Recovery.Generation = "../Forged";
			REQUIRE(setup.Host.QueueDecision(callerOffer, EditorRecoveryDecision::Accept).has_value());
			CHECK(setup.Host.GetOffer()->DecisionPending);
			CHECK(editor.GetScene().GetEntityCount() == 0);
			CHECK_FALSE(editor.IsSceneDirty());
			REQUIRE(setup.Host.Pump().has_value());
			CHECK(&editor.GetProject() == project);
			CHECK(editor.GetScene().GetEntityCount() == 1);
			CHECK(editor.IsSceneDirty());
			CHECK_FALSE(setup.Host.GetOffer());
			CHECK(FileSystem::ReadText(setup.Fixture.GetProjectRoot() / "Assets/Scenes/Main.scene").value_or("") == setup.Original);
		}

		TEST_CASE("EditorHostRecovery: decline preserves scene history and recovery bytes without reoffering in the same epoch")
		{
			HostRecoverySetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			const auto offer = *setup.Host.GetOffer();
			const auto directory = setup.Fixture.GetProjectRoot() / "Library/Autosave";
			const auto manifest = FileSystem::ReadText(directory / "Manifest.json");
			const auto payload = FileSystem::ReadText(directory / offer.Recovery.Generation / "Scene.json");
			REQUIRE(manifest.has_value());
			REQUIRE(payload.has_value());
			const auto revision = editor.GetRevision();
			const auto undo = editor.GetHistory().CanUndo();
			REQUIRE(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Decline).has_value());
			REQUIRE(setup.Host.Pump().has_value());
			REQUIRE(setup.Host.Inspect(1).has_value());
			CHECK_FALSE(setup.Host.GetOffer());
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().CanUndo() == undo);
			CHECK(SceneSerializer::SaveToString(editor.GetScene()).value_or("") == setup.Original);
			CHECK(FileSystem::ReadText(directory / "Manifest.json").value_or("") == *manifest);
			CHECK(FileSystem::ReadText(directory / offer.Recovery.Generation / "Scene.json").value_or("") == *payload);
		}

		TEST_CASE("EditorHostRecovery: an edit after enqueue refuses recovery and leaves the failure observable")
		{
			HostRecoverySetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			const auto offer = *setup.Host.GetOffer();
			REQUIRE(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Accept).has_value());
			{
				SceneEdit edit(editor, "Later edit");
				(void)editor.GetScene().CreateEntity("KeepNewEdit");
				REQUIRE(edit.Commit().has_value());
			}
			const auto expected = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(expected.has_value());
			const auto applied = setup.Host.Pump();
			REQUIRE_FALSE(applied.has_value());
			CHECK(applied.error().GetCode() == ErrorCode::Conflict);
			REQUIRE(setup.Host.GetOffer().has_value());
			CHECK(setup.Host.GetOffer()->Id == offer.Id);
			CHECK_FALSE(setup.Host.GetOffer()->DecisionPending);
			REQUIRE(setup.Host.GetOffer()->Failure.has_value());
			CHECK(setup.Host.GetOffer()->Failure->GetCode() == ErrorCode::Conflict);
			REQUIRE(setup.Host.Pump().has_value());
			REQUIRE(setup.Host.Inspect(1).has_value());
			CHECK(setup.Host.GetOffer()->Failure.has_value());
			CHECK(SceneSerializer::SaveToString(editor.GetScene()).value_or("") == *expected);
		}

		TEST_CASE("EditorHostRecovery: Reset cancels pending work and never reuses an offer ID")
		{
			HostRecoverySetup setup;
			const auto first = *setup.Host.GetOffer();
			REQUIRE(setup.Host.QueueDecision(first, EditorRecoveryDecision::Accept).has_value());
			setup.Host.Reset();
			REQUIRE(setup.Host.Pump().has_value());
			CHECK_FALSE(setup.Host.GetOffer());
			REQUIRE(setup.Host.Inspect(2).has_value());
			const auto second = setup.Host.GetOffer();
			REQUIRE(second.has_value());
			CHECK(second->Id > first.Id);
			CHECK_FALSE(second->DecisionPending);
			CHECK(setup.Host.QueueDecision(first, EditorRecoveryDecision::Accept).error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.Fixture.GetEditor().GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("EditorHostRecovery: changed source bytes are revalidated after enqueue")
		{
			HostRecoverySetup setup;
			const auto offer = *setup.Host.GetOffer();
			REQUIRE(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Accept).has_value());
			const auto source = setup.Fixture.GetProjectRoot() / "Assets/Scenes/Main.scene";
			const std::string replaced = setup.Original + "\n";
			REQUIRE(FileSystem::WriteFileAtomic(source, std::as_bytes(std::span(replaced.data(), replaced.size()))).has_value());
			CHECK(setup.Host.Pump().error().GetCode() == ErrorCode::Conflict);
			REQUIRE(setup.Host.GetOffer()->Failure.has_value());
			CHECK(FileSystem::ReadText(source).value_or("") == replaced);
			CHECK(setup.Fixture.GetEditor().GetScene().GetEntityCount() == 0);
			CHECK_FALSE(setup.Fixture.GetEditor().IsSceneDirty());
		}

		TEST_CASE("EditorHostRecovery: invalid requests cannot replace a queued decision")
		{
			HostRecoverySetup setup;
			const auto offer = *setup.Host.GetOffer();
			REQUIRE(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Decline).has_value());
			CHECK(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Accept).error().GetCode() == ErrorCode::InvalidState);
			auto invalid = offer;
			invalid.Id = 0;
			CHECK(setup.Host.QueueDecision(invalid, EditorRecoveryDecision::Accept).error().GetCode() == ErrorCode::InvalidArgument);
			invalid.Id = offer.Id + 1;
			CHECK(setup.Host.QueueDecision(invalid, EditorRecoveryDecision::Accept).error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.Host.QueueDecision(offer, static_cast<EditorRecoveryDecision>(255)).error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(setup.Host.GetOffer().has_value());
			CHECK(setup.Host.GetOffer()->Id == offer.Id);
			CHECK(setup.Host.GetOffer()->DecisionPending);
			CHECK_FALSE(setup.Host.GetOffer()->Failure);
			REQUIRE(setup.Host.Pump().has_value());
			CHECK_FALSE(setup.Host.GetOffer());
			CHECK(setup.Fixture.GetEditor().GetScene().GetEntityCount() == 0);
			CHECK_FALSE(setup.Fixture.GetEditor().IsSceneDirty());
		}

		TEST_CASE("EditorHostRecovery: matching decline survives binding changes before enqueue or pump")
		{
			for (const bool closeProject : { false, true })
			{
				for (const bool queuedBeforeChange : { false, true })
				{
					CAPTURE(closeProject);
					CAPTURE(queuedBeforeChange);
					HostRecoverySetup setup;
					EditorContext& editor = setup.Fixture.GetEditor();
					const auto offer = *setup.Host.GetOffer();
					const auto source = setup.Fixture.GetProjectRoot() / "Assets/Scenes/Main.scene";
					const auto directory = setup.Fixture.GetProjectRoot() / "Library/Autosave";
					const auto manifest = FileSystem::ReadText(directory / "Manifest.json");
					const auto payload = FileSystem::ReadText(directory / offer.Recovery.Generation / "Scene.json");
					REQUIRE(manifest);
					REQUIRE(payload);
					if (queuedBeforeChange)
						REQUIRE(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Decline));
					if (closeProject)
					{
						REQUIRE(setup.Saves.Reset());
						REQUIRE(editor.CloseProject());
					}
					else
					{
						SceneEdit edit(editor, "Preserved edit before dismissal");
						(void)editor.GetScene().CreateEntity("KeepNewEdit");
						REQUIRE(edit.Commit());
					}
					const auto revision = editor.GetRevision();
					const bool dirty = editor.IsSceneDirty();
					const bool undo = editor.GetHistory().CanUndo();
					std::string scene;
					if (!closeProject)
					{
						const auto serialized = SceneSerializer::SaveToString(editor.GetScene());
						REQUIRE(serialized);
						scene = *serialized;
					}
					if (!queuedBeforeChange)
					{
						CHECK(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Accept).error().GetCode() == ErrorCode::Conflict);
						REQUIRE(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Decline));
					}
					CHECK(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Decline).error().GetCode() == ErrorCode::InvalidState);
					REQUIRE(setup.Host.Pump());
					CHECK_FALSE(setup.Host.GetOffer());
					CHECK(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Decline).error().GetCode() == ErrorCode::NotFound);
					CHECK(editor.HasProject() == !closeProject);
					CHECK(editor.GetRevision() == revision);
					CHECK(editor.IsSceneDirty() == dirty);
					CHECK(editor.GetHistory().CanUndo() == undo);
					if (!closeProject)
						CHECK(SceneSerializer::SaveToString(editor.GetScene()).value_or("") == scene);
					CHECK(FileSystem::ReadText(source).value_or("") == setup.Original);
					CHECK(FileSystem::ReadText(directory / "Manifest.json").value_or("") == *manifest);
					CHECK(FileSystem::ReadText(directory / offer.Recovery.Generation / "Scene.json").value_or("") == *payload);
				}
			}
		}

		TEST_CASE("EditorHostRecovery: a stale queue refusal stays visible without changing the live scene")
		{
			HostRecoverySetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			const auto offer = *setup.Host.GetOffer();
			{
				SceneEdit edit(editor, "New edit before the decision");
				(void)editor.GetScene().CreateEntity("KeepNewEdit");
				REQUIRE(edit.Commit().has_value());
			}
			const auto revision = editor.GetRevision();
			const auto expected = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(expected.has_value());
			CHECK(setup.Host.QueueDecision(offer, EditorRecoveryDecision::Accept).error().GetCode() == ErrorCode::Conflict);
			REQUIRE(setup.Host.GetOffer()->Failure.has_value());
			CHECK(setup.Host.GetOffer()->Failure->GetCode() == ErrorCode::Conflict);
			CHECK_FALSE(setup.Host.GetOffer()->DecisionPending);
			REQUIRE(setup.Host.Pump().has_value());
			CHECK(editor.GetRevision() == revision);
			CHECK(SceneSerializer::SaveToString(editor.GetScene()).value_or("") == *expected);
		}

		TEST_CASE("EditorHostRecovery: read-only acceptance refusal remains visible and permits decline")
		{
			HostRecoverySetup setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			const auto projectFile = editor.GetProject().GetProjectFile();
			setup.Host.Reset();
			REQUIRE(setup.Saves.Reset());
			REQUIRE(editor.CloseProject().has_value());
			const ProjectOpenOptions options{ .ReadOnly = true, .ReadOnlyCacheDirectory = setup.Fixture.GetDirectory().GetPath() / "Readonly" };
			auto project = ProjectManager::OpenProject(projectFile, options, editor.GetTypeRegistry());
			REQUIRE(project.has_value());
			REQUIRE(editor.OpenProject(std::move(*project)).has_value());
			REQUIRE(setup.Host.Inspect(2).has_value());
			const auto offer = setup.Host.GetOffer();
			REQUIRE(offer.has_value());
			CHECK(setup.Host.QueueDecision(*offer, EditorRecoveryDecision::Accept).error().GetCode() == ErrorCode::PermissionDenied);
			REQUIRE(setup.Host.GetOffer()->Failure.has_value());
			CHECK(setup.Host.GetOffer()->Failure->GetCode() == ErrorCode::PermissionDenied);
			CHECK_FALSE(setup.Host.GetOffer()->DecisionPending);
			REQUIRE(setup.Host.Pump().has_value());
			REQUIRE(setup.Host.QueueDecision(*offer, EditorRecoveryDecision::Decline).has_value());
			CHECK_FALSE(setup.Host.GetOffer()->Failure);
			REQUIRE(setup.Host.Pump().has_value());
			CHECK_FALSE(setup.Host.GetOffer());
			CHECK_FALSE(editor.IsSceneDirty());
			CHECK(FileSystem::ReadText(setup.Fixture.GetProjectRoot() / "Assets/Scenes/Main.scene").value_or("") == setup.Original);
		}

		TEST_CASE("EditorHostRecovery: acceptance while a test run owns the editor is refused and stays visible")
		{
			Test::EditorTestFixture fixture("HostRecoveryTestRun");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			// Every clock reading advances 100 ms, past TestRunFrameBudget, so one Pump advances the run by exactly one runner
			// step in every configuration: the run is still pending after the Pump below however fast the build is.
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient client(editor, std::move(specification));
			// A suite whose single case outlasts this test, so the run keeps owning the editor until the client leaves.
			REQUIRE(client.Call("script.write", Json{ { "path", "Assets/Tests/Long.test.luau" }, { "source", "return Test.Suite('Long', function() Test.Case('waits', function() Test.WaitTicks(100000) end) end)" } }));
			REQUIRE(client.Call("project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Long.test.luau" }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } }));
			const auto path = editor.GetScenePath();
			const auto original = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(original.has_value());
			Autosave saves(editor);
			EditorHostRecovery host(editor, saves);
			{
				SceneEdit edit(editor, "Recover this edit");
				(void)editor.GetScene().CreateEntity("RecoverThisEntity");
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(saves.Save(AutosaveReason::BeforePlay).value_or(AutosaveWriteResult{}).Written);
			REQUIRE(saves.Reset());
			auto clean = editor.CreateScene("Main");
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(*clean, *original, {}, report).has_value());
			editor.SetScene(std::move(clean), path);
			REQUIRE(host.Inspect(1).has_value());
			REQUIRE(host.GetOffer().has_value());
			const auto offer = *host.GetOffer();

			REQUIRE(host.QueueDecision(offer, EditorRecoveryDecision::Accept).has_value());
			static_cast<void>(client.Submit("test.run", Json::object()));
			client.GetServer().Pump();
			REQUIRE(editor.GetPlay().IsTestRunActive());
			// A decision queued before the run started is refused at the safe point, and the refusal stays in the offer.
			const Status applied = host.Pump();
			REQUIRE_FALSE(applied.has_value());
			CHECK(applied.error().GetCode() == ErrorCode::InvalidState);
			REQUIRE(host.GetOffer().has_value());
			REQUIRE(host.GetOffer()->Failure.has_value());
			CHECK(host.GetOffer()->Failure->GetCode() == ErrorCode::InvalidState);
			CHECK(editor.GetScene().GetEntityCount() == 0);
			CHECK_FALSE(editor.IsSceneDirty());
			// Enqueueing during the run is refused at once, and the service itself refuses as well.
			CHECK(host.QueueDecision(offer, EditorRecoveryDecision::Accept).error().GetCode() == ErrorCode::InvalidState);
			CHECK(saves.Recover(offer.Recovery).error().GetCode() == ErrorCode::InvalidState);
			CHECK(editor.GetScene().GetEntityCount() == 0);

			// Once the run is over the same offer recovers the scene.
			client.GetServer().DisconnectInProcess(client.GetClient());
			REQUIRE_FALSE(editor.GetPlay().IsTestRunActive());
			REQUIRE(host.QueueDecision(offer, EditorRecoveryDecision::Accept).has_value());
			REQUIRE(host.Pump().has_value());
			CHECK_FALSE(host.GetOffer());
			CHECK(editor.GetScene().GetEntityCount() == 1);
			CHECK(editor.IsSceneDirty());
		}
	}

}
