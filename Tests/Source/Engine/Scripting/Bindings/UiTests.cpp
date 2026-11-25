#include "TestsPCH.h"

#include "Engine/Core/VfsPath.h"
#include "Engine/Renderer/UiDrawList.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/UiSystem.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <format>
#include <string>
#include <string_view>
#include <vector>

// The Ui module (M22, C22.2, R-UI-EVENTS; Docs/Decisions/0033-m22-contract.md decision 28): the game UI's interaction state
// and layout, read through the session's UiSystem (IScriptHost::GetUi), and Ui.SetFocus, which applies UiSystem::SetFocus's
// focus rules.

namespace Engine {

	namespace {

		// A canvas holding the button "Button" at (10, 20), 100 x 30 canvas units with an image, the plain element "Label"
		// below it, and "Loose", an element without a canvas.
		void PopulateUiModuleScene(Scene& scene)
		{
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity button = scene.CreateEntity("Button", canvas);
			button.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
				.AnchorMax = glm::vec2(0.0f),
				.Pivot = glm::vec2(0.0f),
				.Position = glm::vec2(10.0f, 20.0f),
				.Size = glm::vec2(100.0f, 30.0f) });
			button.AddComponent<UiImageComponent>(UiImageComponent{});
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			Entity label = scene.CreateEntity("Label", canvas);
			label.AddComponent<UiRectComponent>(UiRectComponent{});
			Entity loose = scene.CreateEntity("Loose");
			loose.AddComponent<UiRectComponent>(UiRectComponent{});
		}

	}

	namespace Test {

		// The Ui module's coverage scenario ("ScriptApi: every registered function, method, property, operator and constructor
		// has a test"): every function called through the dispatcher in `mode`, in a test-mode evaluation with a session's UI.
		// Ui.SetFocus is called with an element that cannot take focus, which every implementation refuses.
		void RunUiBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			PopulateUiModuleScene(fixture.GetScene());
			// M28 (Docs/Decisions/0043-m28-game-services.md decision 5): a text element for the UiText presentation methods.
			Entity caption = fixture.GetScene().CreateEntity("Caption", fixture.GetScene().FindEntityByPath("/Canvas"));
			caption.AddComponent<UiRectComponent>(UiRectComponent{});
			caption.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "--", .RaycastTarget = false });
			UiSystem ui;
			ui.LayoutStep(fixture.GetScene(), glm::uvec2(320, 180));
			fixture.Ui = &ui;
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
assert(Ui.GetFocus() == nil and Ui.GetHovered() == nil and Ui.GetPressed() == nil and not Ui.IsPointerOverUi())
assert(not pcall(Ui.SetFocus, Scene.FindByName("Label")))
local caption = Scene.FindByName("Caption").UiText
assert(caption:GetPresentationText() == nil)
caption:SetPresentationText("16.7 ms")
assert(caption:GetPresentationText() == "16.7 ms" and caption.Text == "--")
local rect = Ui.GetRect(Scene.FindByName("Button"))
return rect ~= nil and rect.Min == vector.create(10, 20, 0) and rect.Max == vector.create(110, 50, 0)
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			REQUIRE(ui.FindPresentationText(caption.GetUUID()) != nullptr);
			CHECK(*ui.FindPresentationText(caption.GetUUID()) == "16.7 ms");
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
			{
				if (counter.Owner == "Ui" || (counter.Owner == "UiText" && counter.Member.ends_with("PresentationText")))
				{
					CAPTURE(counter.Member);
					CHECK(counter.Calls > 0);
				}
			}
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		// M28 (Docs/Decisions/0043-m28-game-services.md decision 5; the W28.1 review's finding 8): presentation text is at most
		// MaxScriptPresentationTextBytes of valid UTF-8, refused before the UI keeps it, and an evaluation without a session's
		// UI (an edit evaluation) cannot show it.
		TEST_CASE("UiBindings: presentation text is refused past its byte limit, as invalid UTF-8 and without a game UI")
		{
			Test::ScriptTestFixture fixture;
			PopulateUiModuleScene(fixture.GetScene());
			Entity caption = fixture.GetScene().CreateEntity("Caption", fixture.GetScene().FindEntityByPath("/Canvas"));
			caption.AddComponent<UiRectComponent>(UiRectComponent{});
			caption.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "--", .RaycastTarget = false });
			UiSystem ui;
			ui.LayoutStep(fixture.GetScene(), glm::uvec2(320, 180));
			fixture.Ui = &ui;
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(std::format(R"(
local caption = Scene.FindByName("Caption").UiText
caption:SetPresentationText(string.rep("x", {0}))
assert(#caption:GetPresentationText() == {0})
assert(not pcall(function() caption:SetPresentationText(string.rep("x", {1})) end))
assert(#caption:GetPresentationText() == {0})
assert(not pcall(function() caption:SetPresentationText("\xff") end))
local set = caption.SetPresentationText
assert(not pcall(function() set(Scene.FindByName("Button").UiRect, "x") end))
return true
)",
				MaxScriptPresentationTextBytes, MaxScriptPresentationTextBytes + 1));
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			REQUIRE(ui.FindPresentationText(caption.GetUUID()) != nullptr);
			CHECK(ui.FindPresentationText(caption.GetUUID())->size() == MaxScriptPresentationTextBytes);

			// Without a session's UI: an edit evaluation refuses the set and reads nothing.
			Test::ScriptTestFixture edit;
			PopulateUiModuleScene(edit.GetScene());
			Entity label = edit.GetScene().CreateEntity("Caption", edit.GetScene().FindEntityByPath("/Canvas"));
			label.AddComponent<UiRectComponent>(UiRectComponent{});
			label.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "--", .RaycastTarget = false });
			REQUIRE(edit.Start().has_value());
			const auto refused = edit.Evaluate(R"(
local caption = Scene.FindByName("Caption").UiText
assert(not pcall(function() caption:SetPresentationText("16.7 ms") end))
return caption:GetPresentationText() == nil
)");
			REQUIRE_MESSAGE(refused.has_value(), (refused ? "" : refused.error().ToString()));
			CHECK(refused->Value.Get() == Json(true));
		}

		// Decision 28: GetRect reads the UiSystem's current layout in scene pixels (nil for an entity it does not lay out); with
		// no interaction yet the state reads empty; without a session's UI (an edit evaluation) every read reports nothing and
		// SetFocus raises, with an element or nil.
		TEST_CASE("Ui: GetRect reads the current layout, and without a session's UI the reads report nothing and SetFocus raises")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			Scene& scene = fixture.GetScene();
			PopulateUiModuleScene(scene);
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(320, 180));
			fixture.Ui = &ui;
			REQUIRE(fixture.Start());

			const auto result = fixture.Evaluate(R"(
assert(Ui.GetFocus() == nil and Ui.GetHovered() == nil and Ui.GetPressed() == nil and Ui.IsPointerOverUi() == false)
local rect = Ui.GetRect(Scene.FindByName("Button"))
assert(rect.Min == vector.create(10, 20, 0) and rect.Max == vector.create(110, 50, 0))
return Ui.GetRect(Scene.FindByName("Canvas")) == nil and Ui.GetRect(Scene.FindByName("Loose")) == nil
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));

			// The frame's layout of a moved button is the current one.
			scene.FindEntityByPath("/Canvas/Button").GetComponent<UiRectComponent>().Position = glm::vec2(30.0f, 40.0f);
			ui.LayoutFrame(scene, glm::uvec2(320, 180));
			const auto moved = fixture.Evaluate(R"(
local rect = Ui.GetRect(Scene.FindByName("Button"))
return rect.Min == vector.create(30, 40, 0) and rect.Max == vector.create(130, 70, 0)
)");
			REQUIRE_MESSAGE(moved.has_value(), (moved ? "" : moved.error().ToString()));
			CHECK(moved->Value.Get() == Json(true));

			// An entity pending destruction still reads (no rectangle: it was never laid out), but takes no focus.
			const auto pending = fixture.Evaluate(R"(
local temp = Scene.CreateEntity("Temp")
Scene.Destroy(temp)
return Ui.GetRect(temp) == nil and not pcall(Ui.SetFocus, temp)
)");
			REQUIRE_MESSAGE(pending.has_value(), (pending ? "" : pending.error().ToString()));
			CHECK(pending->Value.Get() == Json(true));

			fixture.Ui = nullptr;
			const auto without = fixture.Evaluate(R"(
local button = Scene.FindByName("Button")
assert(Ui.GetFocus() == nil and Ui.GetHovered() == nil and Ui.GetPressed() == nil and not Ui.IsPointerOverUi())
assert(Ui.GetRect(button) == nil)
return not pcall(Ui.SetFocus, button) and not pcall(Ui.SetFocus, nil)
)");
			REQUIRE_MESSAGE(without.has_value(), (without ? "" : without.error().ToString()));
			CHECK(without->Value.Get() == Json(true));
			CHECK(fixture.Errors.empty());
		}

		// C22.3 (the review of C22.3, its major on the consumers of World canvases' pixels; stream W, decision 56's text): an
		// element of a World canvas is laid out in its canvas's own pixels, which Ui.GetRect reports with the canvas, as it
		// reports every element's canvas.
		TEST_CASE("Ui: GetRect reports a World canvas's element in its canvas pixels, with its canvas")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			Scene& scene = fixture.GetScene();
			PopulateUiModuleScene(scene);
			// A World canvas of 400 x 300 canvas pixels holding "Sign" at (100, 50), 200 x 100, whatever the scene's size.
			Entity board = scene.CreateEntity("Board");
			board.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 400, .WorldHeight = 300 });
			Entity sign = scene.CreateEntity("Sign", board);
			sign.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
				.AnchorMax = glm::vec2(0.0f),
				.Pivot = glm::vec2(0.0f),
				.Position = glm::vec2(100.0f, 50.0f),
				.Size = glm::vec2(200.0f, 100.0f) });
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(320, 180));
			fixture.Ui = &ui;
			REQUIRE(fixture.Start());

			const auto result = fixture.Evaluate(R"(
local sign = Ui.GetRect(Scene.FindByName("Sign"))
assert(sign.Min == vector.create(100, 50, 0) and sign.Max == vector.create(300, 150, 0))
assert(sign.Canvas == Scene.FindByName("Board"))
local button = Ui.GetRect(Scene.FindByName("Button"))
return button.Canvas == Scene.FindByName("Canvas")
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.Errors.empty());
		}

		// W22.3's review: UiElementRect's Canvas is an Entity, never nil, for every element GetRect reports; an element pending
		// destruction (readable until the flush, M20's W-P02) is still reported, with its canvas, which a destroyed ancestor
		// leaves pending too.
		TEST_CASE("Ui: GetRect reports the canvas of an element pending destruction")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			Scene& scene = fixture.GetScene();
			PopulateUiModuleScene(scene);
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(320, 180));
			fixture.Ui = &ui;
			REQUIRE(fixture.Start());

			const auto result = fixture.Evaluate(R"(
local canvas = Scene.FindByName("Canvas")
local button = Scene.FindByName("Button")
Scene.Destroy(canvas)
local rect = Ui.GetRect(button)
return rect ~= nil and rect.Min == vector.create(10, 20, 0) and rect.Canvas ~= nil and rect.Canvas == canvas
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.Errors.empty());
		}

		// Decision 28: the play session's script host serves its UiSystem (IScriptHost::GetUi), so the session's scripts read
		// the layout its UiLayout phase made.
		TEST_CASE("Ui: a play session's scripts read the session's UI")
		{
			Test::ScriptTestFixture compiler;
			PopulateUiModuleScene(compiler.GetScene());
			const auto document = SceneSerializer::ToJson(compiler.GetScene());
			REQUIRE(document.has_value());
			PlaySessionSpecification specification;
			specification.Registry = &compiler.GetTypes();
			specification.Assets = &compiler.GetAssetManager();
			specification.ScriptSchemas = compiler.GetSchemaSnapshot();
			specification.Serial = 1;
			auto session = PlaySession::Create(specification, *document);
			REQUIRE_MESSAGE(session.has_value(), (session ? "" : session.error().ToString()));
			(*session)->Tick();
			const auto path = VfsPath::Parse("project://Assets/Tests/UiProbe.luau");
			REQUIRE(path.has_value());
			const auto result = (*session)->GetScripts()->Evaluate(R"(
local rect = Ui.GetRect(Scene.FindByName("Button"))
return {rect.Min.x, rect.Min.y, rect.Max.x, rect.Max.y}
)",
				*path);
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json::array({ 10, 20, 110, 50 }));
			const UiLayoutNode* node =
				FindUiLayoutNode((*session)->GetUi().GetLayout(), (*session)->GetScene().FindEntityByPath("/Canvas/Button").GetUUID());
			REQUIRE(node != nullptr);
			CHECK(node->Min == glm::vec2(10.0f, 20.0f));
		}

		// M28 (Docs/Decisions/0043-m28-game-services.md decision 5): a behaviour shows a frame-paced value through presentation
		// text in OnUpdate; the session's extraction draws it in place of Text, the state hash never sees it, and the fixed
		// step cannot set it.
		TEST_CASE("UiText: a behaviour's presentation text draws in the session's extraction, stays out of the state hash and is refused in the fixed step")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle script(0x7a2805);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Hud.luau", R"(
local Hud = {}
function Hud:OnFixedUpdate()
	if self.FixedDone then return end
	self.FixedDone = true
	local ok = pcall(function() self.Entity.UiText:SetPresentationText("tick") end)
	Scene.CreateEntity("trace:fixed:" .. tostring(ok))
end
function Hud:OnUpdate()
	self.Frames = (self.Frames or 0) + 1
	self.Entity.UiText:SetPresentationText(string.format("%d frames", self.Frames))
end
return Script.Define("Hud", Hud)
)"));
			Scene& scene = fixture.GetScene();
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity hud = scene.CreateEntity("Hud", canvas);
			hud.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Size = glm::vec2(200.0f, 40.0f) });
			hud.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "--" });
			ScriptSlot slot{};
			slot.Script.SetHandle(script);
			hud.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Serial = 1;
			auto created = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(created.has_value(), (created ? "" : created.error().ToString()));
			PlaySession& session = **created;
			const auto frame = [&session]()
			{
				session.FixedStep();
				session.FrameUpdate(FrameTime{ .DeltaTime = session.GetFixedDelta(), .UnscaledDeltaTime = session.GetFixedDelta(), .Alpha = 1.0, .FrameIndex = 0 });
			};
			frame();
			const UUID hudId = session.GetScene().FindEntityByPath("/Canvas/Hud").GetUUID();
			const auto drawnText = [&session, hudId]() -> std::string
			{
				for (const UiDrawItem& item : session.GetLastExtraction().Ui.Items)
					if (item.Kind == UiDrawKind::Text && item.Entity == hudId)
						return item.Text;
				return "<none>";
			};
			const auto hasEntityNamed = [&session](std::string_view name)
			{
				for (const UUID id : session.GetScene().GetCanonicalOrder())
					if (session.GetScene().FindEntityByID(id).GetName() == name)
						return true;
				return false;
			};
			CHECK(hasEntityNamed("trace:fixed:false"));
			REQUIRE(session.GetUi().FindPresentationText(hudId) != nullptr);
			CHECK(*session.GetUi().FindPresentationText(hudId) == "1 frames");
			CHECK(drawnText() == "1 frames");
			CHECK(session.GetScene().FindEntityByID(hudId).GetComponent<UiTextComponent>().Text == "--");
			// Clearing it (an evaluation, which may set it in either phase) changes no hash and draws the authored Text again.
			const uint64_t hash = session.ComputeStateHash();
			REQUIRE(session.GetScripts()->Evaluate("Scene.FindByName('Hud').UiText:SetPresentationText(nil)", {}));
			CHECK(session.ComputeStateHash() == hash);
			CHECK(session.GetUi().FindPresentationText(hudId) == nullptr);
		}

		// Decision 3 (the review of W22.2's major, the lead's decision 1): the session lays its scene out before the scene's
		// startup callbacks too, so a menu's OnStart focuses its first button and reads its rectangle at the session's start
		// (where the layout was empty), and after a Scene.Reload reads the reloaded scene's rectangle, not the replaced
		// scene's (whose entities have the same ids). The Focus event reaches OnUiFocus at the first UiEvents phase.
		TEST_CASE("Ui: a scene's startup callbacks read its own layout and take focus, at the session's start and after a Scene.Reload")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle script(0x7100c1);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Menu.luau", R"(
local Menu = {}
function Menu:OnStart()
	local play = Scene.FindByName("Play")
	Ui.SetFocus(play)
	local rect = Ui.GetRect(play)
	Scene.CreateEntity("trace:start " .. rect.Min.x .. "," .. rect.Min.y)
end
function Menu:OnUiFocus(element, focused)
	Scene.CreateEntity("trace:focus " .. element.Name .. " " .. tostring(focused))
end
return Script.Define("Menu", Menu)
)"));
			Entity menu = fixture.GetScene().CreateEntity("Menu");
			menu.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			ScriptSlot slot{};
			slot.Script.SetHandle(script);
			menu.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
			Entity play = fixture.GetScene().CreateEntity("Play", menu);
			play.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
				.AnchorMax = glm::vec2(0.0f),
				.Pivot = glm::vec2(0.0f),
				.Position = glm::vec2(10.0f, 20.0f),
				.Size = glm::vec2(100.0f, 30.0f) });
			play.AddComponent<UiButtonComponent>(UiButtonComponent{});
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Serial = 1;
			specification.ViewWidth = 320;
			specification.ViewHeight = 180;
			auto created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), (created ? "" : created.error().ToString()));
			PlaySession& session = **created;
			session.SetExtractionEnabled(false);
			const auto traces = [&session]
			{
				std::vector<std::string> names;
				for (const UUID id : session.GetScene().GetCanonicalOrder())
				{
					const std::string& name = session.GetScene().FindEntityByID(id).GetName();
					if (name.starts_with("trace:"))
						names.push_back(name.substr(6));
				}
				return names;
			};
			const auto focusedPlay = [&session]
			{
				return session.GetUi().GetInteraction().Focused == session.GetScene().FindEntityByPath("/Menu/Play").GetUUID();
			};

			CHECK(session.GetScriptErrors().GetErrors().empty());
			CHECK(focusedPlay());
			CHECK(traces() == std::vector<std::string>{ "start 10,20" });
			session.Tick();
			CHECK(traces() == std::vector<std::string>{ "start 10,20", "focus Play true" });

			// The button moved in the running scene, then the scene reloaded from its start document, where it is at (10, 20).
			session.GetScene().FindEntityByPath("/Menu/Play").GetComponent<UiRectComponent>().Position = glm::vec2(200.0f, 100.0f);
			session.Tick();
			const UiLayoutNode* moved = FindUiLayoutNode(session.GetUi().GetLayout(), session.GetScene().FindEntityByPath("/Menu/Play").GetUUID());
			REQUIRE(moved != nullptr);
			REQUIRE(moved->Min == glm::vec2(200.0f, 100.0f));
			REQUIRE(session.GetScripts()->GetHost().RequestSceneReload(std::nullopt).has_value());
			session.Tick();
			REQUIRE(session.GetSceneGeneration() == 2);
			CHECK(session.GetScriptErrors().GetErrors().empty());
			CHECK(focusedPlay());
			CHECK(traces() == std::vector<std::string>{ "start 10,20" });
			session.Tick();
			CHECK(traces() == std::vector<std::string>{ "start 10,20", "focus Play true" });
		}

		// The milestone review (decision 3, the lead's decision 5): the layout made after the startup callbacks takes the world
		// transforms they changed, so a World canvas that an OnStart moves is hit-tested on the first tick where it is drawn, at
		// the session's start and after a Scene.Reload. The canvas "Kiosk" (2 x 2 units) starts far to the right; its OnStart
		// moves it in front of the camera, where the pointer's ray through the view's centre meets its button.
		TEST_CASE("Ui: a World canvas that an OnStart moves takes a click on the first tick where it is drawn")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle script(0x7100c2);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Kiosk.luau", R"(
local Kiosk = {}
function Kiosk:OnStart()
	self.Entity.Transform.Translation = vector.create(0, 0, -5)
end
return Script.Define("Kiosk", Kiosk)
)"));
			Scene& scene = fixture.GetScene();
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			Entity kiosk = scene.CreateEntity("Kiosk");
			kiosk.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(100.0f, 0.0f, -5.0f);
			});
			kiosk.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 64, .WorldHeight = 64, .PixelsPerUnit = 32.0f });
			ScriptSlot slot{};
			slot.Script.SetHandle(script);
			kiosk.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
			Entity button = scene.CreateEntity("Button", kiosk);
			button.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(1.0f), .Size = glm::vec2(0.0f) });
			button.AddComponent<UiImageComponent>(UiImageComponent{});
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Serial = 1;
			specification.ViewWidth = 320;
			specification.ViewHeight = 180;
			auto created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), (created ? "" : created.error().ToString()));
			PlaySession& session = **created;
			session.SetExtractionEnabled(false);
			REQUIRE(session.GetScriptErrors().GetErrors().empty());
			const auto clickCentre = [&session]()
			{
				const PlayInputEvent click{ .Type = PlayInputEventType::MouseButtonInput,
					.State = PlayInputEventState::Tap,
					.Button = MouseButton::Left,
					.HasPosition = true,
					.Position = glm::vec2(160.0f, 90.0f) };
				REQUIRE(session.GetInput().Queue(session.GetTick(), click).has_value());
				session.Tick();
				const UUID pressed = session.GetScene().FindEntityByPath("/Kiosk/Button").GetUUID();
				CHECK(session.GetUi().GetInteraction().Hovered == pressed);
				CHECK(session.GetUi().GetInteraction().Pressed == pressed);
			};

			clickCentre();
			// The kiosk moved away in the running scene, then the scene reloaded: its OnStart moves the reloaded kiosk again.
			session.Tick();
			session.GetScene().FindEntityByPath("/Kiosk").Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(-100.0f, 0.0f, -5.0f);
			});
			REQUIRE(session.GetScripts()->GetHost().RequestSceneReload(std::nullopt).has_value());
			session.Tick();
			REQUIRE(session.GetSceneGeneration() == 2);
			REQUIRE(session.GetScriptErrors().GetErrors().empty());
			clickCentre();
		}

		// Decision 28: the module reads the session's interaction state and layout; SetFocus moves focus to a focusable
		// widget and raises for any other element; without a session's UI (an edit evaluation) it reports nothing.
		TEST_CASE("Ui: the module reads focus, hover, press and rectangles, and SetFocus refuses an element that cannot take focus")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity button = scene.CreateEntity("Button", canvas);
			button.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Position = glm::vec2(10.0f, 20.0f), .Size = glm::vec2(100.0f, 30.0f) });
			button.AddComponent<UiImageComponent>(UiImageComponent{});
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			Entity label = scene.CreateEntity("Label", canvas);
			label.AddComponent<UiRectComponent>(UiRectComponent{});
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(320, 180));
			fixture.Ui = &ui;
			REQUIRE(fixture.Start());

			const auto result = fixture.Evaluate(R"(
local button = Scene.FindByName("Button")
assert(Ui.GetFocus() == nil and Ui.GetHovered() == nil and Ui.GetPressed() == nil)
assert(Ui.IsPointerOverUi() == false)
local rect = Ui.GetRect(button)
assert(rect.Min == vector.create(10, 20, 0) and rect.Max == vector.create(110, 50, 0))
assert(Ui.GetRect(Scene.FindByName("Canvas")) == nil)
Ui.SetFocus(button)
assert(Ui.GetFocus() == button)
assert(not pcall(Ui.SetFocus, Scene.FindByName("Label")))
assert(Ui.GetFocus() == button)
Ui.SetFocus(nil)
return Ui.GetFocus() == nil
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			CHECK(ui.GetQueuedEventCount() == 2);

			fixture.Ui = nullptr;
			const auto without = fixture.Evaluate(R"(
assert(Ui.GetFocus() == nil and Ui.GetRect(Scene.FindByName("Button")) == nil and not Ui.IsPointerOverUi())
return not pcall(Ui.SetFocus, Scene.FindByName("Button"))
)");
			REQUIRE_MESSAGE(without.has_value(), (without ? "" : without.error().ToString()));
			CHECK(without->Value.Get() == Json(true));
		}

		// Decision 28's two rules stream B chose (accepted at W22.2's integration): Ui.SetFocus reports an external mutation
		// only when it changes focus, so the focus the element already has, nil with nothing focused and a refused element
		// report none, and a read-only evaluation refuses only a real change; and GetFocus, GetHovered and GetPressed return
		// nil for an element the scene holds pending destruction, before the flush and the next UiInput clear it.
		TEST_CASE("Ui: SetFocus reports a mutation only when focus changes, and the reads return nil for an element pending destruction")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			Scene& scene = fixture.GetScene();
			PopulateUiModuleScene(scene);
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(320, 180));
			fixture.Ui = &ui;
			REQUIRE(fixture.Start());
			const auto evaluate = [&fixture](std::string_view source)
			{
				const auto result = fixture.Evaluate(source);
				REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
				CHECK(result->Value.Get() == Json(true));
			};

			// Each evaluation that changes focus reports once; one that changes nothing reports nothing.
			evaluate("Ui.SetFocus(nil)\nreturn Ui.GetFocus() == nil");
			CHECK(fixture.ExternalMutations.empty());
			evaluate("Ui.SetFocus(Scene.FindByName(\"Button\"))\nreturn Ui.GetFocus() == Scene.FindByName(\"Button\")");
			CHECK(fixture.ExternalMutations.size() == 1);
			evaluate("Ui.SetFocus(Scene.FindByName(\"Button\"))\nreturn not pcall(Ui.SetFocus, Scene.FindByName(\"Label\"))");
			CHECK(fixture.ExternalMutations.size() == 1);
			evaluate("Ui.SetFocus(nil)\nreturn Ui.GetFocus() == nil");
			CHECK(fixture.ExternalMutations.size() == 2);

			// The button focused, hovered and pressed (UiInput), then destroyed by a script: pending until the flush, it is
			// no longer the focus, the hover or the press the module reads, though the state still names it.
			const UUID button = scene.FindEntityByPath("/Canvas/Button").GetUUID();
			REQUIRE(ui.SetFocus(scene, button).has_value());
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(20.0f, 30.0f), .PointerDown = true, .PointerPressed = true });
			REQUIRE(ui.GetInteraction().Hovered == button);
			REQUIRE(ui.GetInteraction().Pressed == button);
			evaluate(R"(
local button = Scene.FindByName("Button")
assert(Ui.GetFocus() == button and Ui.GetHovered() == button and Ui.GetPressed() == button)
Scene.Destroy(button)
return Ui.GetFocus() == nil and Ui.GetHovered() == nil and Ui.GetPressed() == nil
)");
			CHECK(ui.GetInteraction().Focused == button);
			CHECK(fixture.Errors.empty());
		}

		// Decision 28, amended by the review of W22.2 (the lead's decision 2): a pause menu that a script shows takes focus in
		// the same call, before any layout has seen it (UiSystem::SetFocus and CheckFocus lay the scene out as it is).
		TEST_CASE("Ui: SetFocus focuses a menu the same script has just shown")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			Scene& scene = fixture.GetScene();
			PopulateUiModuleScene(scene);
			Entity pause = scene.CreateEntity("Pause");
			pause.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .SortOrder = 1 });
			Entity resume = scene.CreateEntity("Resume", pause);
			resume.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
				.AnchorMax = glm::vec2(0.0f),
				.Pivot = glm::vec2(0.0f),
				.Position = glm::vec2(150.0f, 20.0f),
				.Size = glm::vec2(100.0f, 30.0f) });
			resume.AddComponent<UiButtonComponent>(UiButtonComponent{});
			pause.SetActive(false);
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(320, 180));
			fixture.Ui = &ui;
			REQUIRE(fixture.Start());

			const auto result = fixture.Evaluate(R"(
local resume = Scene.FindByName("Resume")
assert(not pcall(Ui.SetFocus, resume))
Scene.FindByName("Pause"):SetActive(true)
Ui.SetFocus(resume)
return Ui.GetFocus() == resume and Ui.GetRect(resume) == nil
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			CHECK(ui.GetInteraction().Focused == resume.GetUUID());
		}

		// Decision 28: Ui.SetFocus is registered as mutating, so a read-only evaluation (script.eval's) refuses every call
		// before it runs, whatever the element, while the module's reads work; nothing changes, is queued or is reported.
		TEST_CASE("Ui: a read-only evaluation reads the UI and refuses every SetFocus")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true, .ReadOnly = true });
			Scene& scene = fixture.GetScene();
			PopulateUiModuleScene(scene);
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(320, 180));
			fixture.Ui = &ui;
			REQUIRE(fixture.Start());
			const UUID button = scene.FindEntityByPath("/Canvas/Button").GetUUID();
			REQUIRE(ui.SetFocus(scene, button).has_value());
			static_cast<void>(ui.TakeEvents());

			const auto result = fixture.Evaluate(R"(
local button = Scene.FindByName("Button")
assert(Ui.GetFocus() == button and Ui.GetRect(button) ~= nil)
local same = pcall(Ui.SetFocus, button)
local cleared = pcall(Ui.SetFocus, nil)
local refused = pcall(Ui.SetFocus, Scene.FindByName("Label"))
return not same and not cleared and not refused and Ui.GetFocus() == button
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			CHECK(ui.GetInteraction().Focused == button);
			CHECK(ui.GetQueuedEventCount() == 0);
			CHECK(fixture.ExternalMutations.empty());
		}
	}

}
