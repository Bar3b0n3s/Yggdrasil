#include "TestsPCH.h"

#include "Engine/Automation/Methods/UiMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Project/ProjectSettings.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

// ui.tree (M22, C22.1, R-UI-AUTO; Docs/Decisions/0033-m22-contract.md decision 8), in process through the editor's
// AutomationFixture (§15.2); the Runtime's answer is tested against an exported build in Tests/Automation/test_ui.py
// (UiTreeTests.test_ui_tree_works_in_the_runtime).
// Skipped by the contract; stream E un-skips them.

namespace Engine {

	namespace {

		Json ParseUiMethodJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE(json.has_value());
			return std::move(*json);
		}

		// A canvas at two scene pixels per unit, sorted 3, with a panel (an image) and a label (a text) inside it.
		void CreateUi(Test::AutomationFixture& setup)
		{
			REQUIRE(setup.Call("edit.batch", ParseUiMethodJson(R"({"label":"Ui","ops":[
				{"method":"entity.create","params":{"name":"Hud","components":{"UiCanvas":{"ScaleFactor":2,"SortOrder":3}}}},
				{"method":"entity.create","params":{"name":"Panel","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[10,5],"Size":[40,20]},"UiImage":{}}}},
				{"method":"entity.create","params":{"name":"Label","parent":{"$ref":"1.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[1,1],"Size":[0,0]},"UiText":{"Text":"Score"}}}}]})"))
					.has_value());
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("UiMethods: ui.tree lists every canvas and element with its scene-pixel rectangle in draw order" * doctest::skip(true))
		{
			Test::AutomationFixture setup("UiTree");
			CreateUi(setup);
			Result<Json> listed = setup.Call("ui.tree", Json{ { "format", "Json" }, { "width", 200 }, { "height", 100 } });
			REQUIRE_MESSAGE(listed.has_value(), listed.error().ToString());
			CHECK((*listed)["width"] == Json(200));
			CHECK((*listed)["height"] == Json(100));
			Json& elements = (*listed)["elements"];
			REQUIRE(elements.size() == 3);
			CHECK(elements[0]["name"] == Json("Hud"));
			CHECK(elements[0]["depth"] == Json(0));
			CHECK(elements[0]["rect"] == Json{ { "x", 0 }, { "y", 0 }, { "width", 200 }, { "height", 100 } });
			// A canvas reports its sort order and its scale (scene pixels per canvas unit); an element reports 0 for both.
			CHECK(elements[0]["sortOrder"] == Json(3));
			CHECK(elements[0]["scale"] == Json(2));
			CHECK(elements[1]["sortOrder"] == Json(0));
			CHECK(elements[1]["scale"] == Json(0));
			CHECK(elements[1]["name"] == Json("Panel"));
			CHECK(elements[1]["depth"] == Json(1));
			CHECK(elements[1]["rect"] == Json{ { "x", 20 }, { "y", 10 }, { "width", 80 }, { "height", 40 } });
			CHECK(elements[1]["components"] == Json::array({ "UiRect", "UiImage" }));
			CHECK(elements[1]["visible"] == Json(true));
			CHECK(elements[2]["name"] == Json("Label"));
			CHECK(elements[2]["parent"] == elements[1]["id"]);
			CHECK(elements[2]["text"] == Json("Score"));
			// The interaction members are W22.2's; W22.1 reports their defaults.
			CHECK(elements[2]["focused"] == Json(false));
			CHECK(elements[2]["widget"] == Json(""));

			// The text outline, laid out by default at the project's window size (the size of a deterministic session's view).
			Result<Json> text = setup.Call("ui.tree", Json::object());
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const ProjectSettings& settings = setup.GetEditor().GetProject().GetSettings();
			CHECK((*text)["width"] == Json(settings.Window.Width));
			const std::string outline = JsonReader((*text)["text"]).ReadString().value_or(std::string());
			CHECK(outline.starts_with("Main.scene  rev "));
			CHECK(outline.contains("UiText(\"Score\")"));
			CHECK(outline.contains("(20,10 80x40)"));
		}

		TEST_CASE("UiMethods: ui.tree of the play scene reads the session's current layout" * doctest::skip(true))
		{
			Test::AutomationFixture setup("UiTreePlay");
			CreateUi(setup);
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true } }).has_value());
			REQUIRE(setup.Call("play.step", Json{ { "ticks", 1 }, { "render", "none" } }).has_value());
			Result<Json> listed = setup.Call("ui.tree", Json{ { "format", "Json" }, { "target", "play" } });
			REQUIRE_MESSAGE(listed.has_value(), listed.error().ToString());
			// A lockstep session's view is the project's window size (GetDeterministicViewSize).
			const ProjectSettings& settings = setup.GetEditor().GetProject().GetSettings();
			CHECK((*listed)["width"] == Json(settings.Window.Width));
			CHECK((*listed)["height"] == Json(settings.Window.Height));
			CHECK((*listed)["elements"].size() == 3);
			REQUIRE(setup.Call("play.stop", Json::object()).has_value());
		}

		TEST_CASE("UiMethods: ui.tree refuses a canvas reference without a UiCanvas, and one of width and height alone" * doctest::skip(true))
		{
			Test::AutomationFixture setup("UiTreeErrors");
			CreateUi(setup);
			const Result<Json> notCanvas = setup.Call("ui.tree", Json{ { "canvas", "/Hud/Panel" } });
			REQUIRE_FALSE(notCanvas.has_value());
			CHECK(notCanvas.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<Json> oneSize = setup.Call("ui.tree", Json{ { "width", 320 } });
			REQUIRE_FALSE(oneSize.has_value());
			CHECK(oneSize.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<Json> canvas = setup.Call("ui.tree", Json{ { "canvas", "/Hud" }, { "format", "Json" }, { "depth", 1 } });
			REQUIRE_MESSAGE(canvas.has_value(), canvas.error().ToString());
			CHECK((*canvas)["elements"].size() == 2);
		}
	}

}
