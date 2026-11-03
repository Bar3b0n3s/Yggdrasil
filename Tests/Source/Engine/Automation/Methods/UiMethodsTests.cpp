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
#include <vector>

// ui.tree (M22, C22.1, R-UI-AUTO; Docs/Decisions/0033-m22-contract.md decision 8), in process through the editor's
// AutomationFixture (§15.2); the Runtime's answer is tested against an exported build in Tests/Automation/test_ui.py
// (UiTreeTests.test_ui_tree_works_in_the_runtime).

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

		// The rectangle of the element `name` in ui.tree's JSON entries `elements`, or null.
		Json FindUiTreeRect(const Json& elements, std::string_view name)
		{
			for (const Json& element : elements)
			{
				if (element["name"] == Json(name))
					return element["rect"];
			}
			return Json();
		}

		// `text` split at its line feeds (the last line's feed ends it, so no empty line follows).
		std::vector<std::string> SplitUiOutline(std::string_view text)
		{
			std::vector<std::string> lines;
			size_t start = 0;
			while (start < text.size())
			{
				const size_t end = text.find('\n', start);
				lines.emplace_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
				if (end == std::string_view::npos)
					break;
				start = end + 1;
			}
			return lines;
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("UiMethods: ui.tree lists every canvas and element with its scene-pixel rectangle in draw order")
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

		// Decision 8: the play scene's rectangles are the session's current layout (UiSystem::GetLayout), which W22.2's UiInput
		// hit-tests, not a layout made afresh: an edit of the play scene shows in ui.tree only once a step lays it out, while
		// ui.tree with an explicit size lays the scene as it is now out at once (the W22.1 review: a lockstep session's view
		// is the window size, so without the edit a fresh layout would give the same answer).
		TEST_CASE("UiMethods: ui.tree of the play scene reads the session's current layout")
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
			const Json laidOut{ { "x", 20 }, { "y", 10 }, { "width", 80 }, { "height", 40 } };
			CHECK(FindUiTreeRect((*listed)["elements"], "Panel") == laidOut);

			// Move the panel in the play scene without stepping: the session's layout still holds the old rectangle.
			const Result<Json> moved = setup.Call("entity.update",
				ParseUiMethodJson(R"({"entity": "/Hud/Panel", "target": "play", "components": {"UiRect": {"AnchorMin": [0, 0], "AnchorMax": [0, 0],
					"Pivot": [0, 0], "Position": [30, 5], "Size": [40, 20]}}})"));
			REQUIRE_MESSAGE(moved.has_value(), moved.error().ToString());
			const Json movedRect{ { "x", 60 }, { "y", 10 }, { "width", 80 }, { "height", 40 } };
			listed = setup.Call("ui.tree", Json{ { "format", "Json" }, { "target", "play" } });
			REQUIRE_MESSAGE(listed.has_value(), listed.error().ToString());
			CHECK(FindUiTreeRect((*listed)["elements"], "Panel") == laidOut);
			// An explicit size lays the play scene out as it is now.
			const Result<Json> fresh = setup.Call("ui.tree",
				Json{ { "format", "Json" }, { "target", "play" }, { "width", settings.Window.Width }, { "height", settings.Window.Height } });
			REQUIRE_MESSAGE(fresh.has_value(), fresh.error().ToString());
			CHECK(FindUiTreeRect((*fresh)["elements"], "Panel") == movedRect);
			// The next step lays the moved panel out.
			REQUIRE(setup.Call("play.step", Json{ { "ticks", 1 }, { "render", "none" } }).has_value());
			listed = setup.Call("ui.tree", Json{ { "format", "Json" }, { "target", "play" } });
			REQUIRE_MESSAGE(listed.has_value(), listed.error().ToString());
			CHECK(FindUiTreeRect((*listed)["elements"], "Panel") == movedRect);
			REQUIRE(setup.Call("play.stop", Json::object()).has_value());
		}

		// Decision 8 and Architecture §13.5 (the W22.1 review): the clip, visibility and short component forms in both
		// formats, and the text outline line by line: the header, the canvas line, the box-drawn prefixes of every depth and
		// the clipped and hidden markers. At 200 x 100 scene pixels and scale 1: a panel that clips its children holds a
		// nested pair inside its clip and an element outside it; a stack container places its cell; an element lies off the
		// scene image.
		TEST_CASE("UiMethods: ui.tree reports clips, hidden elements, sheets and containers in its JSON and its text outline")
		{
			Test::AutomationFixture setup("UiTreeOutline");
			const Result<Json> sheet = setup.Call("asset.create",
				ParseUiMethodJson(R"({"type": "SpriteSheet", "path": "Assets/Ui/Panels.spritesheet", "values": {"Frames": [
					{"Rect": {"X": 0, "Y": 0, "Width": 4, "Height": 4}}, {"Rect": {"X": 4, "Y": 0, "Width": 4, "Height": 4}}]}})"));
			REQUIRE_MESSAGE(sheet.has_value(), sheet.error().ToString());
			const std::string sheetId = JsonReader((*sheet)["asset"]["id"]).ReadString().value_or(std::string());
			REQUIRE(sheetId.size() == 16);
			Json batch = ParseUiMethodJson(R"({"label":"Ui","ops":[
				{"method":"entity.create","params":{"name":"Hud","components":{"UiCanvas":{}}}},
				{"method":"entity.create","params":{"name":"Panel","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[10,10],"Size":[100,50],"ClipChildren":true},
					"UiImage":{"Frame":1}}}},
				{"method":"entity.create","params":{"name":"Inner","parent":{"$ref":"1.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[5,5],"Size":[20,10]}}}},
				{"method":"entity.create","params":{"name":"Deep","parent":{"$ref":"2.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[1,1],"Size":[0,0]}}}},
				{"method":"entity.create","params":{"name":"Outside","parent":{"$ref":"1.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[120,5],"Size":[20,10]}}}},
				{"method":"entity.create","params":{"name":"Row","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[0,70],"Size":[60,20]},
					"UiLayout":{"Kind":"Stack","Direction":"Horizontal"}}}},
				{"method":"entity.create","params":{"name":"Cell","parent":{"$ref":"5.entity.id"},"components":{"UiRect":{"Size":[20,20]}}}},
				{"method":"entity.create","params":{"name":"Offscreen","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[300,0],"Size":[10,10]}}}}]})");
			batch["ops"][1]["params"]["components"]["UiImage"]["Sheet"] = sheetId;
			REQUIRE(setup.Call("edit.batch", batch).has_value());

			Result<Json> listed = setup.Call("ui.tree", Json{ { "format", "Json" }, { "width", 200 }, { "height", 100 } });
			REQUIRE_MESSAGE(listed.has_value(), listed.error().ToString());
			const Json& elements = (*listed)["elements"];
			REQUIRE(elements.size() == 8);
			std::vector<std::string> names;
			std::vector<std::string> ids;
			for (const Json& element : elements)
			{
				names.push_back(JsonReader(element["name"]).ReadString().value_or(std::string()));
				ids.push_back(JsonReader(element["id"]).ReadString().value_or(std::string()).substr(0, 8));
			}
			CHECK(names == std::vector<std::string>{ "Hud", "Panel", "Inner", "Deep", "Outside", "Row", "Cell", "Offscreen" });
			const Json panelClip{ { "x", 10 }, { "y", 10 }, { "width", 100 }, { "height", 50 } };
			// The panel clips its descendants, not itself.
			CHECK(elements[1]["clipped"] == Json(false));
			CHECK(elements[1]["visible"] == Json(true));
			for (const size_t inside : { 2u, 3u })
			{
				CAPTURE(inside);
				CHECK(elements[inside]["clipped"] == Json(true));
				CHECK(elements[inside]["clip"] == panelClip);
				CHECK(elements[inside]["visible"] == Json(true));
			}
			CHECK(elements[3]["depth"] == Json(3));
			// Outside the panel's clip: clipped and covering no pixel.
			CHECK(elements[4]["rect"] == Json{ { "x", 130 }, { "y", 15 }, { "width", 20 }, { "height", 10 } });
			CHECK(elements[4]["clipped"] == Json(true));
			CHECK(elements[4]["clip"] == panelClip);
			CHECK(elements[4]["visible"] == Json(false));
			// The stack places its cell at the container's start; the element off the scene image covers no pixel.
			CHECK(elements[6]["rect"] == Json{ { "x", 0 }, { "y", 70 }, { "width", 20 }, { "height", 20 } });
			CHECK(elements[7]["clipped"] == Json(false));
			CHECK(elements[7]["visible"] == Json(false));

			Result<Json> text = setup.Call("ui.tree", Json{ { "width", 200 }, { "height", 100 } });
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const std::vector<std::string> lines = SplitUiOutline(JsonReader((*text)["text"]).ReadString().value_or(std::string()));
			REQUIRE(lines.size() == 9);
			CHECK(lines[0].starts_with("Main.scene  rev "));
			CHECK(lines[0].ends_with("  200x100 scene pixels  7 elements"));
			CHECK(lines[1] == "Hud  " + ids[0] + "  UiCanvas(sort 0, scale 1)");
			CHECK(lines[2] == "├─ Panel  " + ids[1] + "  UiImage(Ui/Panels.spritesheet#1)  (10,10 100x50)");
			CHECK(lines[3] == "│  ├─ Inner  " + ids[2] + "  (15,15 20x10)  clipped");
			CHECK(lines[4] == "│  │  └─ Deep  " + ids[3] + "  (15,15 20x10)  clipped");
			CHECK(lines[5] == "│  └─ Outside  " + ids[4] + "  (130,15 20x10)  clipped  hidden");
			CHECK(lines[6] == "├─ Row  " + ids[5] + "  UiLayout(Stack Horizontal)  (0,70 60x20)");
			CHECK(lines[7] == "│  └─ Cell  " + ids[6] + "  (0,70 20x20)");
			CHECK(lines[8] == "└─ Offscreen  " + ids[7] + "  (300,0 10x10)  hidden");
		}

		TEST_CASE("UiMethods: ui.tree refuses a canvas reference without a UiCanvas, and one of width and height alone")
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

		// C22.2 (Docs/Decisions/0033-m22-contract.md decision 31; stream A): the interaction fields W22.1 reserved, the widget
		// kind and Interactable from the components in both scenes, Focused, Hovered and Pressed from the play session's state,
		// and the widgets' short forms and markers in the text outline.
		TEST_CASE("UiMethods: ui.tree reports each element's widget and the play session's focus, hover and press")
		{
			Test::AutomationFixture setup("UiTreeWidgets");
			REQUIRE(setup.Call("edit.batch", ParseUiMethodJson(R"({"label":"Widgets","ops":[
				{"method":"entity.create","params":{"name":"Hud","components":{"UiCanvas":{}}}},
				{"method":"entity.create","params":{"name":"Play","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[10,10],"Size":[100,40]},"UiImage":{},"UiButton":{}}}},
				{"method":"entity.create","params":{"name":"Sound","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[10,60],"Size":[30,30]},"UiImage":{},
					"UiToggle":{"IsOn":true,"Interactable":false}}}},
				{"method":"entity.create","params":{"name":"Music","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[50,60],"Size":[30,30]},"UiToggle":{}}}},
				{"method":"entity.create","params":{"name":"Volume","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[10,100],"Size":[100,20]},
					"UiSlider":{"Value":12.4,"Max":10,"WholeNumbers":true}}}},
				{"method":"entity.create","params":{"name":"List","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[150,10],"Size":[50,50]},"UiScrollView":{}}}},
				{"method":"entity.create","params":{"name":"Items","parent":{"$ref":"5.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Size":[50,150]}}}},
				{"method":"entity.update","params":{"entity":"/Hud/List","components":{
					"UiScrollView":{"Content":"/Hud/List/Items","ScrollPosition":[0,300]}}}}]})"))
					.has_value());
			const auto findEntry = [](const Json& elements, std::string_view name)
			{
				for (const Json& element : elements)
				{
					if (element["name"] == Json(name))
						return element;
				}
				return Json();
			};

			Result<Json> edit = setup.Call("ui.tree", Json{ { "format", "Json" } });
			REQUIRE_MESSAGE(edit.has_value(), edit.error().ToString());
			Json play = findEntry((*edit)["elements"], "Play");
			CHECK(play["widget"] == Json("Button"));
			CHECK(play["interactable"] == Json(true));
			CHECK(play["focused"] == Json(false));
			const Json sound = findEntry((*edit)["elements"], "Sound");
			CHECK(sound["widget"] == Json("Toggle"));
			CHECK(sound["interactable"] == Json(false));
			CHECK(findEntry((*edit)["elements"], "Hud")["widget"] == Json(""));
			const Result<Json> outline = setup.Call("ui.tree", Json::object());
			REQUIRE_MESSAGE(outline.has_value(), outline.error().ToString());
			const std::string text = JsonReader((*outline)["text"]).ReadString().value_or(std::string());
			CHECK(text.find("UiButton") != std::string::npos);
			CHECK(text.find("UiToggle(on)") != std::string::npos);
			CHECK(text.find("UiToggle(off)") != std::string::npos);
			CHECK(text.find("disabled") != std::string::npos);
			// The values as read: the whole-number slider's 12.4 clamped to its Max, 10, and the scroll position 300 clamped
			// to the content's overflow, 150 - 50 = 100, on the vertical axis only.
			CHECK(text.find("UiSlider(10)") != std::string::npos);
			CHECK(text.find("UiScrollView(0,100)") != std::string::npos);
			CHECK(findEntry((*edit)["elements"], "Volume")["widget"] == Json("Slider"));
			CHECK(findEntry((*edit)["elements"], "List")["widget"] == Json("ScrollView"));

			// A press on Play in the play session hovers, presses and focuses it.
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const Json press = Json::array({ Json{ { "type", "mouseButton" }, { "button", "Left" }, { "state", "down" }, { "position", Json::array({ 20, 20 }) } } });
			REQUIRE(setup.Call("play.step", Json{ { "ticks", 1 }, { "render", "none" }, { "input", press } }).has_value());
			const Result<Json> played = setup.Call("ui.tree", Json{ { "format", "Json" }, { "target", "play" } });
			REQUIRE_MESSAGE(played.has_value(), played.error().ToString());
			play = findEntry((*played)["elements"], "Play");
			CHECK(play["focused"] == Json(true));
			CHECK(play["hovered"] == Json(true));
			CHECK(play["pressed"] == Json(true));
			const Result<Json> playedOutline = setup.Call("ui.tree", Json{ { "target", "play" } });
			REQUIRE_MESSAGE(playedOutline.has_value(), playedOutline.error().ToString());
			CHECK(JsonReader((*playedOutline)["text"]).ReadString().value_or(std::string()).find("focused hovered pressed") != std::string::npos);
			REQUIRE(setup.Call("play.stop", Json::object()).has_value());
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 53; stream T): a text input in ui.tree.
		TEST_CASE("UiMethods: ui.tree reports a text input as TextInput with its text" * doctest::skip(true))
		{
			Test::AutomationFixture setup("UiTreeTextInput");
			REQUIRE(setup.Call("edit.batch", ParseUiMethodJson(R"({"label":"Ui","ops":[
				{"method":"entity.create","params":{"name":"Form","components":{"UiCanvas":{}}}},
				{"method":"entity.create","params":{"name":"Name","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Size":[120,24]},"UiImage":{},"UiTextInput":{"Text":"Ada"}}}}]})"))
					.has_value());
			Result<Json> listed = setup.Call("ui.tree", Json{ { "format", "Json" }, { "width", 200 }, { "height", 100 } });
			REQUIRE_MESSAGE(listed.has_value(), listed.error().ToString());
			const Json& field = (*listed)["elements"][1];
			CHECK(field["widget"] == Json("TextInput"));
			CHECK(field["interactable"] == Json(true));
			CHECK(field["text"] == Json("Ada"));
			CHECK(field["components"] == Json::array({ "UiRect", "UiImage", "UiTextInput" }));
			Result<Json> text = setup.Call("ui.tree", Json::object());
			REQUIRE(text.has_value());
			CHECK(JsonReader((*text)["text"]).ReadString().value_or(std::string()).contains("UiTextInput(\"Ada\")"));
		}

		// C22.3 (decision 53; stream W): a World canvas in ui.tree, in its own pixels, whatever the scene resolution.
		TEST_CASE("UiMethods: ui.tree reports a World canvas and its elements in the canvas's own pixels" * doctest::skip(true))
		{
			Test::AutomationFixture setup("UiTreeWorld");
			REQUIRE(setup.Call("edit.batch", ParseUiMethodJson(R"({"label":"Ui","ops":[
				{"method":"entity.create","params":{"name":"Board","components":{"UiCanvas":{"RenderMode":"World","WorldWidth":300,"WorldHeight":150}}}},
				{"method":"entity.create","params":{"name":"Sign","parent":{"$ref":"0.entity.id"},"components":{
					"UiRect":{"AnchorMin":[0,0],"AnchorMax":[0,0],"Pivot":[0,0],"Position":[10,20],"Size":[30,40]},"UiImage":{}}}}]})"))
					.has_value());
			Result<Json> listed = setup.Call("ui.tree", Json{ { "format", "Json" }, { "width", 200 }, { "height", 100 } });
			REQUIRE_MESSAGE(listed.has_value(), listed.error().ToString());
			const Json& elements = (*listed)["elements"];
			REQUIRE(elements.size() == 2);
			CHECK(elements[0]["worldCanvas"] == Json(true));
			CHECK(elements[0]["rect"] == Json{ { "x", 0 }, { "y", 0 }, { "width", 300 }, { "height", 150 } });
			CHECK(elements[1]["worldCanvas"] == Json(true));
			CHECK(elements[1]["rect"] == Json{ { "x", 10 }, { "y", 20 }, { "width", 30 }, { "height", 40 } });
			CHECK(elements[1]["visible"] == Json(true));
		}
	}

}
