#include "TestsPCH.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static Json ParseSettingsJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	static std::string ReadProjectFile(const Test::EditorTestFixture& fixture, std::string_view name = "TestProject")
	{
		const Result<std::string> text = FileSystem::ReadText(fixture.GetProjectRoot(name) / (std::string(name) + ".eproj"));
		REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
		return *text;
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ProjectSettingsCommand: a merge patch changes the settings and writes the .eproj" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("SettingsPatch");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();

			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor,
				ParseSettingsJson(R"({"Window":{"Title":"Tetris","Width":720},"Simulation":{"FixedHz":30},
					"Input":{"Actions":{"MoveLeft":{"Type":"Button","Bindings":["Key.Left"]}}}})"),
				"Set Project Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			const Result<uint64_t> undoIndex = editor.Execute(std::move(*command));
			REQUIRE_MESSAGE(undoIndex.has_value(), undoIndex.error().ToString());

			const ProjectSettings& settings = editor.GetProject().GetSettings();
			CHECK(settings.Window.Title == "Tetris");
			CHECK(settings.Window.Width == 720);
			CHECK(settings.Window.Height == 900); // untouched by the patch
			CHECK(settings.Simulation.FixedHz == 30);
			CHECK(settings.Input.Actions.contains("MoveLeft"));
			const std::string file = ReadProjectFile(fixture);
			CHECK(file.contains("\"Title\": \"Tetris\""));
			CHECK_FALSE(editor.IsSceneDirty());
		}

		TEST_CASE("ProjectSettingsCommand: undo restores the previous settings and file" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("SettingsUndo");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const std::string before = ReadProjectFile(fixture);

			Result<Scope<ProjectSettingsCommand>> command =
				ProjectSettingsCommand::CreateFromPatch(editor, ParseSettingsJson(R"({"StartScene":"Assets/Scenes/Main.scene"})"), "Set Start Scene");
			REQUIRE(command.has_value());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
			CHECK(editor.GetProject().GetSettings().StartScene == "Assets/Scenes/Main.scene");
			CHECK(editor.GetHistory().Undo(editor) == 1);
			CHECK(editor.GetProject().GetSettings().StartScene.empty());
			CHECK(ReadProjectFile(fixture) == before);
		}

		TEST_CASE("ProjectSettingsCommand: an invalid patch is a located Validation error and changes nothing" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("SettingsInvalid");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();

			const Result<Scope<ProjectSettingsCommand>> outOfRange =
				ProjectSettingsCommand::CreateFromPatch(editor, ParseSettingsJson(R"({"Simulation":{"FixedHz":0}})"), "Bad");
			REQUIRE_FALSE(outOfRange.has_value());
			CHECK(outOfRange.error().GetCode() == ErrorCode::Validation);
			CHECK(outOfRange.error().ToString().contains("/Simulation/FixedHz"));

			const Result<Scope<ProjectSettingsCommand>> notObject = ProjectSettingsCommand::CreateFromPatch(editor, ParseSettingsJson("[1]"), "Bad");
			REQUIRE_FALSE(notObject.has_value());
			CHECK(notObject.error().GetCode() == ErrorCode::Validation);
			CHECK(editor.GetProject().GetSettings().Simulation.FixedHz == 60);
		}

		TEST_CASE("ProjectSettingsCommand: needs an open project" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("SettingsNoProject");
			const Result<Scope<ProjectSettingsCommand>> command =
				ProjectSettingsCommand::CreateFromPatch(fixture.GetEditor(), ParseSettingsJson("{}"), "Nothing");
			REQUIRE_FALSE(command.has_value());
			CHECK(command.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("ProjectSettingsCommand: keeps both documents and never dirties the scene" * doctest::skip(true))
		{
			const ProjectSettingsCommand command("Label", CreateRef<const Json>(ParseSettingsJson(R"({"Name":"A"})")),
				CreateRef<const Json>(ParseSettingsJson(R"({"Name":"B"})")));
			CHECK(command.GetLabel() == "Label");
			CHECK_FALSE(command.ChangesScene());
			CHECK(command.GetBefore() == ParseSettingsJson(R"({"Name":"A"})"));
			CHECK(command.GetAfter() == ParseSettingsJson(R"({"Name":"B"})"));
			CHECK(command.GetMemorySize() > 0);
		}
	}

}
