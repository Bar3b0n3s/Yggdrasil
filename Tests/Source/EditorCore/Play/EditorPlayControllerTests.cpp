#include "TestsPCH.h"

#include "EditorCore/Play/EditorPlayController.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>

// The editor's play mode (Architecture §5.6, §12.4). Skipped skeletons of the M7 contract
// (Docs/Decisions/0012-m7-decisions.md decision 11): stream A implements the controller and removes the skips.

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorPlayController: play then stop leaves the edit scene, its revision and its history untouched" * doctest::skip(true))
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

		TEST_CASE("EditorPlayController: a second start and a stop in Edit mode are InvalidState" * doctest::skip(true))
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

		TEST_CASE("EditorPlayController: a disconnect of the lockstep owner releases lockstep and pauses play" * doctest::skip(true))
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

		TEST_CASE("EditorPlayController: the editor's loop runs at the project's FixedHz while playing" * doctest::skip(true))
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
	}

}
