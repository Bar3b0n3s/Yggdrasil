#include "TestsPCH.h"

#include "EditorCore/Automation/UiAuthoringMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

// ui.createWidget (M22, C22.2, R-UI-EDITOR; Docs/Decisions/0033-m22-contract.md decision 32), in process through the
// editor's AutomationFixture; the editor's Create UI menu submits the same action.

namespace Engine {

	namespace {

		// The entity of `id` (16 hex digits) in the editor's active scene.
		Entity FindCreated(Test::AutomationFixture& setup, const Json& id)
		{
			const std::optional<UUID> uuid = UUID::FromString(JsonReader(id).ReadString().value_or(std::string()));
			REQUIRE(uuid.has_value());
			const Entity entity = setup.GetEditor().GetScene().FindEntityByID(*uuid);
			REQUIRE(entity.IsValid());
			return entity;
		}

		Json CreateWidget(Test::AutomationFixture& setup, const Json& params)
		{
			Result<Json> created = setup.Call("ui.createWidget", params);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			return std::move(*created);
		}

	}

	TEST_SUITE("EditorCore")
	{
		// The templates of UiAuthoringMethods.h, each kind in one undo step.
		TEST_CASE("UiAuthoringMethods: ui.createWidget builds each kind from its template in one undo step")
		{
			Test::AutomationFixture setup("UiCreateWidget");
			const Json canvas = CreateWidget(setup, Json{ { "kind", "Canvas" } });
			REQUIRE(canvas["entities"].size() == 1);
			const Entity canvasEntity = FindCreated(setup, canvas["entity"]["id"]);
			CHECK(canvasEntity.HasComponent<UiCanvasComponent>());
			const std::string parent = JsonReader(canvas["entity"]["id"]).ReadString().value_or(std::string());
			const size_t entitiesBefore = setup.GetEditor().GetScene().GetEntityCount();

			const Json button = CreateWidget(setup, Json{ { "kind", "Button" }, { "parent", parent }, { "text", "Play" } });
			REQUIRE(button["entities"].size() == 2);
			const Entity buttonEntity = FindCreated(setup, button["entities"][0]);
			CHECK(buttonEntity.GetName() == "Button");
			CHECK(buttonEntity.GetParent().GetUUID() == canvasEntity.GetUUID());
			CHECK(buttonEntity.GetComponent<UiRectComponent>().Size == glm::vec2(200.0f, 50.0f));
			CHECK(buttonEntity.GetComponent<UiImageComponent>().Color == glm::vec4(0.9f, 0.9f, 0.9f, 1.0f));
			CHECK(buttonEntity.HasComponent<UiButtonComponent>());
			const Entity label = FindCreated(setup, button["entities"][1]);
			CHECK(label.GetName() == "Label");
			CHECK(label.GetParent().GetUUID() == buttonEntity.GetUUID());
			CHECK(label.GetComponent<UiTextComponent>().Text == "Play");
			CHECK(label.GetComponent<UiTextComponent>().Alignment == TextAlignment::Center);
			CHECK(label.GetComponent<UiRectComponent>().AnchorMax == glm::vec2(1.0f));

			const Json toggle = CreateWidget(setup, Json{ { "kind", "Toggle" }, { "parent", parent } });
			REQUIRE(toggle["entities"].size() == 3);
			const Entity toggleEntity = FindCreated(setup, toggle["entities"][0]);
			CHECK(toggleEntity.GetComponent<UiToggleComponent>().Graphic == FindCreated(setup, toggle["entities"][1]).GetUUID());
			CHECK(FindCreated(setup, toggle["entities"][2]).GetComponent<UiTextComponent>().Text == "Toggle");

			const Json slider = CreateWidget(setup, Json{ { "kind", "Slider" }, { "parent", parent }, { "name", "Volume" } });
			REQUIRE(slider["entities"].size() == 3);
			const Entity sliderEntity = FindCreated(setup, slider["entities"][0]);
			CHECK(sliderEntity.GetName() == "Volume");
			CHECK(sliderEntity.GetComponent<UiSliderComponent>().Fill == FindCreated(setup, slider["entities"][1]).GetUUID());
			CHECK(sliderEntity.GetComponent<UiSliderComponent>().Handle == FindCreated(setup, slider["entities"][2]).GetUUID());

			const Json scroll = CreateWidget(setup, Json{ { "kind", "ScrollView" }, { "parent", parent } });
			REQUIRE(scroll["entities"].size() == 2);
			const Entity scrollEntity = FindCreated(setup, scroll["entities"][0]);
			CHECK(scrollEntity.GetName() == "Scroll View");
			const Entity content = FindCreated(setup, scroll["entities"][1]);
			CHECK(scrollEntity.GetComponent<UiScrollViewComponent>().Content == content.GetUUID());
			CHECK(content.GetComponent<UiLayoutComponent>().Kind == UiLayoutKind::Stack);
			CHECK(content.GetComponent<UiRectComponent>().Size == glm::vec2(0.0f, 400.0f));

			for (const std::string_view kind : { "Panel", "Image", "Text" })
				CHECK(CreateWidget(setup, Json{ { "kind", kind }, { "parent", parent } })["entities"].size() == 1);

			// Each widget is one undo step: undoing the last four leaves the button's, the toggle's and the slider's entities.
			for (int step = 0; step < 4; ++step)
				REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == entitiesBefore + 2 + 3 + 3);
		}

		// Decision 32: a widget without a canvas above it (no parent, or a parent without a UiCanvas at or above it) goes below
		// a new canvas created in the same command, so one undo removes both; without a parent, index places the created root
		// among the scene's roots.
		TEST_CASE("UiAuthoringMethods: ui.createWidget puts a widget without a canvas above it below a new canvas, and refuses a label on a kind without one")
		{
			Test::AutomationFixture setup("UiCreateWidgetRoot");
			const size_t entitiesBefore = setup.GetEditor().GetScene().GetEntityCount();
			const Json dryRun = CreateWidget(setup, Json{ { "kind", "Button" }, { "dryRun", true } });
			CHECK(dryRun["entities"].size() == 3);
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == entitiesBefore);

			const Json button = CreateWidget(setup, Json{ { "kind", "Button" } });
			REQUIRE(button["entities"].size() == 3);
			const Entity canvas = FindCreated(setup, button["entities"][0]);
			CHECK(canvas.GetName() == "Canvas");
			CHECK_FALSE(canvas.GetParent().IsValid());
			CHECK(canvas.HasComponent<UiCanvasComponent>());
			CHECK(FindCreated(setup, button["entity"]["id"]).GetParent().GetUUID() == canvas.GetUUID());
			CHECK(button["entity"]["id"] == button["entities"][1]);

			const Result<Json> labelled = setup.Call("ui.createWidget", Json{ { "kind", "Image" }, { "text", "Logo" } });
			REQUIRE_FALSE(labelled.has_value());
			CHECK(labelled.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(labelled.error().GetIssues().empty());
			CHECK(labelled.error().GetIssues()[0].JsonPointer == "/text");
			const Result<Json> unnamed = setup.Call("ui.createWidget", Json{ { "kind", "Text" }, { "name", "" } });
			REQUIRE_FALSE(unnamed.has_value());
			CHECK(unnamed.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<Json> orphan = setup.Call("ui.createWidget", Json{ { "kind", "Text" }, { "parent", "/Nowhere" } });
			REQUIRE_FALSE(orphan.has_value());
			CHECK(orphan.error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == entitiesBefore + 3);
			// The new canvas is part of the button's command: one undo removes all three entities.
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == entitiesBefore);

			// A parent without a canvas at or above it (the hierarchy's "Create UI child" on a plain entity) gets a new canvas.
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Player" } }).has_value());
			const Json below = CreateWidget(setup, Json{ { "kind", "Text" }, { "parent", "/Player" } });
			REQUIRE(below["entities"].size() == 2);
			const Entity playerCanvas = FindCreated(setup, below["entities"][0]);
			CHECK(playerCanvas.GetName() == "Canvas");
			CHECK(playerCanvas.HasComponent<UiCanvasComponent>());
			CHECK(playerCanvas.GetParent().GetName() == "Player");
			CHECK(FindCreated(setup, below["entity"]["id"]).GetParent().GetUUID() == playerCanvas.GetUUID());

			// Without a parent, index is the created root's place among the scene's roots, and past them it is refused.
			const Json first = CreateWidget(setup, Json{ { "kind", "Canvas" }, { "index", 0 } });
			REQUIRE_FALSE(setup.GetEditor().GetScene().GetRootEntities().empty());
			CHECK(setup.GetEditor().GetScene().GetRootEntities().front() == FindCreated(setup, first["entity"]["id"]).GetUUID());
			const Result<Json> past = setup.Call("ui.createWidget", Json{ { "kind", "Canvas" }, { "index", 99 } });
			REQUIRE_FALSE(past.has_value());
			CHECK(past.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(past.error().GetIssues().empty());
			CHECK(past.error().GetIssues()[0].JsonPointer == "/index");
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 53; stream T): the TextInput kind, appended to UiWidgetKind: an
		// element with a dark image and a UiTextInput whose TextElement and Placeholder are its two text children, the text as
		// the placeholder's.
		TEST_CASE("UiAuthoringMethods: ui.createWidget builds a text input with its text and placeholder elements")
		{
			Test::AutomationFixture setup("UiCreateTextInput");
			const Json created = CreateWidget(setup, Json{ { "kind", "TextInput" }, { "name", "Name" }, { "text", "Your name" } });
			// A new canvas, the input, its Text and its Placeholder, in creation order.
			REQUIRE(created["entities"].size() == 4);
			const Entity input = FindCreated(setup, created["entity"]["id"]);
			REQUIRE(input.HasComponent<UiTextInputComponent>());
			REQUIRE(input.HasComponent<UiImageComponent>());
			const UiTextInputComponent& field = input.GetComponent<UiTextInputComponent>();
			CHECK(field.Text.empty());
			const Entity text = setup.GetEditor().GetScene().FindEntityByID(field.TextElement);
			const Entity placeholder = setup.GetEditor().GetScene().FindEntityByID(field.Placeholder);
			REQUIRE(text.IsValid());
			REQUIRE(placeholder.IsValid());
			CHECK(text.GetParent() == input);
			CHECK(placeholder.GetParent() == input);
			CHECK_FALSE(text.GetComponent<UiTextComponent>().Wrap);
			CHECK(placeholder.GetComponent<UiTextComponent>().Text == "Your name");
			// One undo step removes all four (their ids are read before the undo destroys the handles' entities).
			std::vector<UUID> ids;
			for (const Json& id : created["entities"])
				ids.push_back(FindCreated(setup, id).GetUUID());
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			for (const UUID id : ids)
				CHECK_FALSE(setup.GetEditor().GetScene().FindEntityByID(id).IsValid());
		}
	}

}
