#include "TestsPCH.h"
#include "EditorCore/Automation/EditorMethods.h"

#include "EditorCore/Automation/Private/M10MethodFixture.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("EditorMethods: state reports panels selection mode and lockstep owner")
		{
			Test::M10MethodFixture setup;
			EditorContext& editor = setup.Fixture.GetEditor();
			SceneEdit edit(editor, "Selected");
			const auto entity = editor.GetScene().CreateEntity("Selected");
			REQUIRE(edit.Commit().has_value());
			REQUIRE(editor.SetSelection({ entity.GetUUID() }, SceneTarget::Edit).has_value());
			REQUIRE(editor.GetUiState().SetPanelOpen(EditorPanel::Console, true).has_value());
			editor.GetUiState().CompleteFrame();
			const auto result = setup.Call("editor.state");
			REQUIRE(result.has_value());
			CHECK((*result)["selection"][0]["name"] == "Selected");
			CHECK((*result)["mode"] == "Edit");
			CHECK((*result)["lockstepOwner"] == "");
			CHECK((*result)["uiFrame"] == 1);
			CHECK((*result)["openPanels"].dump().contains("Console"));
			REQUIRE(setup.Client->Call("play.start", Json{ { "lockstep", true } }).has_value());
			const auto playing = setup.Call("editor.state");
			REQUIRE(playing.has_value());
			CHECK((*playing)["mode"] == "Play");
			CHECK((*playing)["lockstepOwner"] == "test");
			REQUIRE(setup.Client->Call("play.pause", Json::object()).has_value());
			CHECK((*setup.Call("editor.state"))["mode"] == "Paused");
			REQUIRE(setup.Client->Call("play.stop", Json::object()).has_value());
		}

		TEST_CASE("EditorMethods: state is batchable and works without a renderer")
		{
			Test::M10MethodFixture setup;
			const auto revision = setup.Fixture.GetEditor().GetRevision();
			const auto result = setup.Call("editor.state");
			REQUIRE(result.has_value());
			CHECK((*result)["uiFrame"] == 0);
			CHECK((*result)["selection"] == Json::array());
			CHECK(setup.Methods.Find("editor.state")->Specification.AllowedInBatch);
			CHECK_FALSE(setup.Methods.Find("editor.state")->Specification.Mutates);
			CHECK(setup.Fixture.GetEditor().GetRevision() == revision);
		}
	}

}
