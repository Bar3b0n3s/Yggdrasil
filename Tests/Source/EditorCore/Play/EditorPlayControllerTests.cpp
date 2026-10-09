#include "TestsPCH.h"

#include "EditorCore/Play/EditorPlayController.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>

// The editor's play mode (Architecture Â§5.6, Â§12.4; Docs/Decisions/0012-m7-decisions.md decisions 3 and 11).

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorPlayController: before-play failure prevents the copy and a retry observes completed preparation")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			uint32_t calls = 0;
			editor.SetLifecycleCallbacks({ .BeforePlay = [&calls, &editor]() -> Status
			{
				if (++calls == 1)
					return MakeError(ErrorCode::Io, "cannot save before play");
				static_cast<void>(editor.GetScene().CreateEntity("Prepared"));
				return {};
			} });
			const auto failed = editor.GetPlay().Start({});
			REQUIRE_FALSE(failed);
			CHECK(failed.error().GetCode() == ErrorCode::Io);
			CHECK_FALSE(editor.GetPlay().IsPlaying());
			CHECK(calls == 1);
			REQUIRE(editor.GetPlay().Start({}));
			CHECK(calls == 2);
			CHECK(editor.GetPlay().GetSession()->GetScene().FindEntityByPath("/Prepared").IsValid());
			REQUIRE(editor.GetPlay().Stop());
			editor.SetLifecycleCallbacks({});
		}

		TEST_CASE("EditorPlayController: play then stop leaves the edit scene, its revision and its history untouched")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			static_cast<void>(editor.GetScene().CreateEntity("Ball"));
			const Result<std::string> before = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(before.has_value());
			const uint64_t revision = editor.GetRevision();
			const size_t historySize = editor.GetHistory().GetUndoCount();

			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true }).has_value());
			REQUIRE(play.IsPlaying());
			CHECK(play.GetPlayStateName() == "Play");
			for (int tick = 0; tick < 5; ++tick)
				play.GetSession()->Tick();
			CHECK(play.GetTick() == std::optional<uint64_t>(5));
			REQUIRE(play.Stop().has_value());
			CHECK_FALSE(play.IsPlaying());
			CHECK(play.GetPlayStateName() == "Edit");

			const Result<std::string> after = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(after.has_value());
			CHECK(*after == *before);
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetUndoCount() == historySize);
		}

		TEST_CASE("EditorPlayController: a second start and a stop in Edit mode are InvalidState")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorPlayController& play = fixture.GetEditor().GetPlay();
			const Status notPlaying = play.Stop();
			REQUIRE_FALSE(notPlaying.has_value());
			CHECK(notPlaying.error().GetCode() == ErrorCode::InvalidState);
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			const Status again = play.Start(PlayStartOptions{});
			REQUIRE_FALSE(again.has_value());
			CHECK(again.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EditorPlayController: a disconnect of the lockstep owner releases lockstep and pauses play")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorPlayController& play = fixture.GetEditor().GetPlay();
			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true, .LockstepOwner = 3 }).has_value());
			play.OnClientDisconnected(2); // not the owner
			CHECK(play.GetSession()->IsLockstep());
			play.OnClientDisconnected(3);
			CHECK_FALSE(play.GetSession()->IsLockstep());
			CHECK(play.GetSession()->IsPaused());
			CHECK(play.GetPlayStateName() == "Paused");
		}

		TEST_CASE("EditorPlayController: the editor's loop runs at the project's FixedHz while playing")
		{
			// EditorApp applies GetFrameLoopConfig at its safe point (FrameLoop::SetLoopConfig), so a 30 Hz project plays at
			// 30 ticks per wall-clock second and its frame deltas are 1/30 s (Docs/Decisions/0012-m7-decisions.md decision 3).
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const Result<Json> patch = JsonReader::Parse(R"({"Simulation":{"FixedHz":30,"MaxStepsPerFrame":3}})");
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Set Project Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());

			EditorPlayController& play = editor.GetPlay();
			CHECK_FALSE(play.GetFrameLoopConfig().has_value()); // Edit mode: the editor's own loop
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			const std::optional<FrameLoopConfig> config = play.GetFrameLoopConfig();
			REQUIRE(config.has_value());
			CHECK(config->FixedHz == 30);
			CHECK(config->MaxStepsPerFrame == 3);
			CHECK(config->MaxFrameDelta == FrameLoopConfig{}.MaxFrameDelta);
			CHECK(play.GetSession()->GetFixedDelta() == doctest::Approx(1.0 / 30.0));
			REQUIRE(play.Stop().has_value());
			CHECK_FALSE(play.GetFrameLoopConfig().has_value());
		}

		TEST_CASE("EditorPlayController: the frame hooks drive a running session; paused and lockstep sessions wait")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorPlayController& play = fixture.GetEditor().GetPlay();
			const FrameTime frame{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 1.0, .FrameIndex = 0 };
			// No effect in Edit mode.
			play.OnFixedStep();
			play.OnUpdate(frame);
			CHECK(play.GetFrameTimeScale() == 1.0);
			CHECK_FALSE(play.IsFrameThrottleSuspended());
			CHECK(play.GetPlayStateName() == "Edit");
			CHECK_FALSE(play.GetTick().has_value());

			REQUIRE(play.Start(PlayStartOptions{ .Mode = PlayMode::Simulate, .TimeScale = 0.5 }).has_value());
			CHECK(play.GetPlayStateName() == "Simulate");
			CHECK(play.GetFrameTimeScale() == 0.5);
			play.OnFixedStep();
			play.OnFixedStep();
			play.OnUpdate(frame);
			CHECK(play.GetTick() == std::optional<uint64_t>(2));
			play.GetSession()->SetPaused(true);
			CHECK(play.GetPlayStateName() == "Paused");
			play.OnFixedStep();
			CHECK(play.GetTick() == std::optional<uint64_t>(2));
			play.GetSession()->SetStepping(true);
			CHECK(play.IsFrameThrottleSuspended());
			REQUIRE(play.Stop().has_value());

			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true, .LockstepOwner = 4 }).has_value());
			play.OnFixedStep();
			play.OnUpdate(frame);
			CHECK(play.GetTick() == std::optional<uint64_t>(0));
			CHECK(play.GetSession()->GetLockstepOwner() == 4);
		}

		TEST_CASE("EditorPlayController: Start checks its options and the edit state, and changes nothing when it fails")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			EditorPlayController& play = fixture.GetEditor().GetPlay();
			const Status noScene = play.Start(PlayStartOptions{});
			REQUIRE_FALSE(noScene.has_value());
			CHECK(noScene.error().GetCode() == ErrorCode::InvalidState);

			fixture.CreateAndOpenScene();
			const Status tooFast = play.Start(PlayStartOptions{ .TimeScale = PlaySession::MaxTimeScale * 2.0 });
			REQUIRE_FALSE(tooFast.has_value());
			CHECK(tooFast.error().GetCode() == ErrorCode::InvalidArgument);
			const Status outside = play.Start(PlayStartOptions{ .ScenePath = "../Elsewhere.scene" });
			REQUIRE_FALSE(outside.has_value());
			CHECK(outside.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(play.IsPlaying());
			CHECK(play.GetPlayStateName() == "Edit");
		}

		TEST_CASE("EditorPlayController: the session seed is the project's Seed xor the scene's, unless one is given")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const Result<Json> patch = JsonReader::Parse(R"({"Simulation":{"Seed":1337}})");
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Set Project Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
			editor.GetScene().SetSeed(42);

			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			CHECK(play.GetSession()->GetSeed() == PlaySession::ComputeSessionSeed(1337, 42));
			REQUIRE(play.Stop().has_value());
			REQUIRE(play.Start(PlayStartOptions{ .Seed = 7 }).has_value());
			CHECK(play.GetSession()->GetSeed() == 7);
		}

		TEST_CASE("EditorPlayController: a lockstep session defers asset reloads until it stops")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			CHECK_FALSE(editor.GetAssets().AreReloadsDeferred()); // ordinary play reloads (see the next test case)
			REQUIRE(play.Stop().has_value());

			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true, .LockstepOwner = 3 }).has_value());
			CHECK(editor.GetAssets().AreReloadsDeferred());
			// A released lockstep keeps the deferral until the session ends.
			play.OnClientDisconnected(3);
			CHECK_FALSE(play.GetSession()->IsLockstep());
			CHECK(editor.GetAssets().AreReloadsDeferred());
			REQUIRE(play.Stop().has_value());
			CHECK_FALSE(editor.GetAssets().AreReloadsDeferred());
		}

		TEST_CASE("EditorPlayController: a reload during ordinary play marks the session modified, whatever started it")
		{
			// Â§7.5 race rule 4: the editor's own write of an asset (asset.setProperties, asset.import) reloads it in the running
			// game as an external change does, and both mark the session modified; a lockstep session defers both.
			Test::EditorTestFixture fixture("PlayReloadModified");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			EditorAssetManager& assets = editor.GetAssets();
			const Result<VfsPath> material = VfsPath::Parse("project://Assets/Materials/Red.material");
			REQUIRE(material.has_value());
			const auto materialText = [](double roughness)
			{
				return std::format(R"({{"Format": "Material", "Version": 1, "Roughness": {}}})", roughness);
			};
			const auto writeFromEditor = [&editor, &material](const std::string& text)
			{
				REQUIRE(editor.WriteProjectFile(*material, std::as_bytes(std::span(text.data(), text.size()))).has_value());
			};
			REQUIRE(editor.GetVfs().CreateDirectories(material->GetParent()).has_value());
			writeFromEditor(materialText(0.1));
			REQUIRE(assets.Refresh().has_value());
			const AssetHandle handle = assets.Resolve("Assets/Materials/Red.material").value_or(AssetHandle());
			REQUIRE(assets.Load(handle).has_value());
			const uint64_t version = assets.GetVersion(handle);

			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			CHECK_FALSE(play.GetSession()->IsModified());
			// The editor's write reimports the material on a job; WaitIdle publishes it.
			writeFromEditor(materialText(0.2));
			assets.WaitIdle();
			REQUIRE(assets.GetVersion(handle) == version + 1);
			CHECK(play.GetSession()->IsModified());
			REQUIRE(play.Stop().has_value());

			// An external change, found by a refresh.
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			const std::string external = materialText(0.3);
			REQUIRE(FileSystem::WriteFileAtomic(editor.GetProject().GetRoot() / "Assets/Materials/Red.material",
				std::as_bytes(std::span(external.data(), external.size())))
					.has_value());
			REQUIRE(assets.Refresh().has_value());
			assets.WaitIdle();
			REQUIRE(assets.GetVersion(handle) == version + 2);
			CHECK(play.GetSession()->IsModified());
			REQUIRE(play.Stop().has_value());

			// A lockstep session defers the reload until it stops, so it is never marked.
			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true }).has_value());
			writeFromEditor(materialText(0.4));
			assets.WaitIdle();
			CHECK(assets.GetVersion(handle) == version + 2);
			CHECK_FALSE(play.GetSession()->IsModified());
			REQUIRE(play.Stop().has_value());
			assets.WaitIdle();
			CHECK(assets.GetVersion(handle) == version + 3);
		}

		TEST_CASE("EditorPlayController: every session gets a serial of its own")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorPlayController& play = fixture.GetEditor().GetPlay();
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			const uint64_t first = play.GetSession()->GetSerial();
			CHECK(first != 0);
			REQUIRE(play.Stop().has_value());
			// A start that fails uses up no serial; the next session's differs from every earlier one.
			CHECK_FALSE(play.Start(PlayStartOptions{ .TimeScale = -1.0 }).has_value());
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			CHECK(play.GetSession()->GetSerial() == first + 1);
		}

		TEST_CASE("EditorPlayController: Stop restores the selection by UUID")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const UUID ball = editor.GetScene().CreateEntity("Ball").GetUUID();
			editor.SetSelection({ ball });

			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true }).has_value());
			// The play scene copies the ids, so the selection names the same entities in it; a runtime spawn can join it.
			CHECK(play.GetSession()->GetScene().FindEntityByID(ball).IsValid());
			Result<Entity> spawned = play.GetSession()->CreateEntity("Spawned");
			REQUIRE(spawned.has_value());
			editor.SetSelection({ ball, spawned->GetUUID() });
			CHECK(editor.GetSelection().size() == 2);

			REQUIRE(play.Stop().has_value());
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == ball);
		}

		TEST_CASE("EditorPlayController: closing the project stops its session")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }).has_value());
			REQUIRE(editor.CloseProject());
			CHECK_FALSE(editor.GetPlay().IsPlaying());
			CHECK_FALSE(editor.GetAssets().AreReloadsDeferred());
		}
	}

}
