#include "TestsPCH.h"
#include "Editor/Panels/SceneViewportPanel.h"

#include "Editor/PanelInteractionFixture.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		// The real toolbar only needs a measured viewport; no image, renderer or picking is involved.
		class SceneToolbarViewport final : public EditorViewportHost
		{
		public:
			[[nodiscard]] EditorViewportImage GetImage(ViewportView view) const override
			{
				CHECK(view == ViewportView::Scene);
				return {};
			}
			void SetRectangle(ViewportView view, const EditorViewportRect& rectangle) override
			{
				CHECK(view == ViewportView::Scene);
				Rectangle = rectangle;
			}
			[[nodiscard]] Status RequestPick(const EditorViewportClick&) override { return MakeError(ErrorCode::Unsupported, "toolbar test has no picking image"); }
			[[nodiscard]] Status ConfigureSceneSnapshot(RenderSnapshot&, const EditorViewportOptions&, std::span<const UUID>) override { return MakeError(ErrorCode::Unsupported, "toolbar test has no renderer"); }
			void SetGameInputFocused(bool) override { FAIL("scene toolbar must not change game input focus"); }

			EditorViewportRect Rectangle{};
		};

		// A displayed scene image without a GPU: an opaque texture key plus the camera extracted for it, so the real panel,
		// gizmo and navigation run unchanged. Picks are recorded instead of read back.
		class ScenePanelViewport final : public EditorViewportHost
		{
		public:
			[[nodiscard]] EditorViewportImage GetImage(ViewportView view) const override { return view == ViewportView::Scene ? Image : EditorViewportImage{}; }
			void SetRectangle(ViewportView view, const EditorViewportRect& rectangle) override
			{
				if (view == ViewportView::Scene)
					Rectangle = rectangle;
			}
			[[nodiscard]] Status RequestPick(const EditorViewportClick& click) override
			{
				Clicks.push_back(click);
				return {};
			}
			[[nodiscard]] Status ConfigureSceneSnapshot(RenderSnapshot&, const EditorViewportOptions&, std::span<const UUID>) override { return MakeError(ErrorCode::Unsupported, "no renderer in panel tests"); }
			void SetGameInputFocused(bool focused) override { CHECK_FALSE(focused); }

			EditorViewportImage Image{};
			EditorViewportRect Rectangle{};
			std::vector<EditorViewportClick> Clicks{};
		};

		// The real SceneViewportPanel in a floating window, next to another panel that can hold focus.
		class ScenePanelHarness
		{
		public:
			// `engineResources` mounts the shipped resources, for the file-backed built-in environments.
			explicit ScenePanelHarness(std::string_view label, bool openScene = true, bool engineResources = false)
				: m_Automation(label, openScene, std::nullopt, engineResources), m_Actions(m_Automation.GetEditor(), m_Automation.GetClient().GetServer()), m_Controls(m_Automation.GetEditor(), m_Automation.GetClient().GetServer()), m_Edits(m_Automation.GetEditor()), m_Thumbnails(m_Automation.GetEditor(), [](const ThumbnailRequest& request)
			{
				return CreateImage(request.Size, request.Size, nvrhi::Format::RGBA8_UNORM);
			}),
				  m_Gizmos(m_Automation.GetEditor()),
				  m_Context{ m_Automation.GetEditor(), m_Automation.GetClient().GetServer(), m_Actions, m_Controls, Viewport, m_Edits, m_Thumbnails, m_Gizmos }
			{
				Utils::ApplyEditorStyle();
				REQUIRE(Frame());
				REQUIRE(Frame());
				// The displayed image matches the measured rectangle, like a rendered frame after a resize.
				const glm::uvec2 size(static_cast<uint32_t>(Viewport.Rectangle.Size.x), static_cast<uint32_t>(Viewport.Rectangle.Size.y));
				REQUIRE(size.x > 0);
				REQUIRE(size.y > 0);
				Viewport.Image = { .Texture = static_cast<ImTextureID>(7), .Width = size.x, .Height = size.y, .Generation = 1, .FrameIndex = 1, .Picking = true };
				RefreshCamera();
				REQUIRE(Frame());
			}

			// The image's camera, as the host copies it from the snapshot it renders.
			void RefreshCamera()
			{
				EditorContext& editor = GetEditor();
				const Scene* scene = editor.GetPlay().GetSession() ? &editor.GetPlay().GetSession()->GetScene() : editor.HasScene() ? &editor.GetScene()
																																	: nullptr;
				if (!scene)
					return;
				const auto snapshot = ExtractRenderSnapshot(*scene, { .Camera = RenderCameraSource::Explicit, .ExplicitCamera = editor.GetSceneViewCamera(), .Width = Viewport.Image.Width, .Height = Viewport.Image.Height });
				REQUIRE(snapshot.has_value());
				Viewport.Image.Camera = snapshot->Camera;
			}

			[[nodiscard]] Status Frame(const std::function<void()>& before = {})
			{
				return m_Ui.Frame([this, &before]() -> Status
				{
					ImGuizmo::BeginFrame();
					if (before)
						before();
					// FirstUseEver keeps the window floating and movable, so a press that became a window move would show.
					ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
					ImGui::SetNextWindowSize(ImVec2(900.0f, 640.0f), ImGuiCond_FirstUseEver);
					ImGui::Begin(Utils::EditorWindowTitle(EditorPanel::SceneViewport), nullptr, SceneViewportPanel::GetWindowFlags());
					const Status status = Panel.Draw(m_Context);
					ImGui::End();
					ImGui::SetNextWindowPos(ImVec2(0.0f, 700.0f), ImGuiCond_FirstUseEver);
					ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f), ImGuiCond_FirstUseEver);
					ImGui::Begin("Other panel");
					ImGui::TextUnformatted("Holds focus");
					ImGui::End();
					return status;
				});
			}

			void FrameOk(const std::function<void()>& before = {})
			{
				const Status status = Frame(before);
				REQUIRE_MESSAGE(status.has_value(), (status ? std::string() : status.error().ToString()));
			}

			void FocusOther()
			{
				FrameOk([]()
				{
					ImGui::SetWindowFocus("Other panel");
				});
				REQUIRE_FALSE(IsSceneFocused());
			}

			void MoveTo(ImVec2 position)
			{
				ImGui::GetIO().AddMousePosEvent(position.x, position.y);
				FrameOk();
			}

			[[nodiscard]] ImVec2 Center() const { return ImVec2(Viewport.Rectangle.Min.x + Viewport.Rectangle.Size.x * 0.5f, Viewport.Rectangle.Min.y + Viewport.Rectangle.Size.y * 0.5f); }
			[[nodiscard]] static ImGuiWindow* SceneWindow() { return ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::SceneViewport)); }
			[[nodiscard]] static bool IsSceneFocused() { return ImGui::GetCurrentContext()->NavWindow == SceneWindow(); }
			[[nodiscard]] EditorContext& GetEditor() { return m_Context.Editor; }
			[[nodiscard]] EditorPanelContext& GetContext() { return m_Context; }
			[[nodiscard]] Test::AutomationFixture& GetAutomation() { return m_Automation; }
			[[nodiscard]] const ExplicitRenderCamera& Camera() { return GetEditor().GetViewportState().GetCamera(); }

			ScenePanelViewport Viewport{};
			SceneViewportPanel Panel{};
		private:
			Test::AutomationFixture m_Automation;
			EditorActions m_Actions;
			EditorAutomationControls m_Controls;
			ReflectedEditController m_Edits;
			ThumbnailCache m_Thumbnails;
			GizmoController m_Gizmos;
			EditorPanelContext m_Context;
			Test::PanelInteractionUi m_Ui{ false };
		};

		std::vector<std::byte> AssetPayload(AssetHandle handle)
		{
			// Canonical handle text with its trailing NUL, as the content browser sends it.
			const std::string text = handle.ToString();
			std::vector<std::byte> bytes(text.size() + 1, std::byte{ 0 });
			std::ranges::transform(text, bytes.begin(), [](char character)
			{
				return static_cast<std::byte>(character);
			});
			return bytes;
		}

		// Drags an external ENGINE_ASSET payload onto the image centre and releases it there, then runs the queued command
		// and the frame that collects its result. Returns that frame's status (the drop's own errors appear at the drop).
		Status DropOnImage(ScenePanelHarness& harness, std::span<const std::byte> bytes)
		{
			bool dragging = false;
			const auto source = [&dragging, bytes]()
			{
				if (dragging && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern))
				{
					ImGui::SetDragDropPayload("ENGINE_ASSET", bytes.data(), bytes.size());
					ImGui::EndDragDropSource();
				}
			};
			harness.MoveTo(harness.Center());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			dragging = true;
			ENGINE_TRY(harness.Frame(source));
			ENGINE_TRY(harness.Frame(source));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			ENGINE_TRY(harness.Frame(source));
			dragging = false;
			harness.GetContext().Actions.Pump();
			return harness.Frame();
		}

		size_t CountEntities(const Scene& scene)
		{
			return scene.GetCanonicalOrder().size();
		}

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("SceneViewportPanel: consecutive keyboard space toggles retain focus when the coordinate label changes")
		{
			Test::PanelInteractionFixture fixture("SceneToolbarKeyboard");
			auto& services = fixture.GetContext();
			SceneToolbarViewport viewport;
			EditorPanelContext context{ services.Editor, services.Automation, services.Actions, services.AutomationControls,
				viewport, services.InspectorEdits, services.Thumbnails, services.Gizmos };
			Test::PanelInteractionUi ui(false);
			Utils::ApplyEditorStyle();
			SceneViewportPanel panel;
			std::string text;
			bool focusScale = false;
			const char* title = Utils::EditorWindowTitle(EditorPanel::SceneViewport);
			const auto draw = [&panel, &context, &text, &focusScale, title]() -> Status
			{
				ImGuizmo::BeginFrame();
				if (focusScale)
				{
					auto* window = ImGui::FindWindowByName(title);
					REQUIRE(window != nullptr);
					ImGui::SetWindowFocus(title);
					ImGui::SetNavID(window->GetID("Scale"), ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
					// SetNavID alone leaves keyboard navigation hidden; the first Tab would only reveal Scale.
					ImGui::SetNavCursorVisible(true);
					focusScale = false;
				}
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(1000.0f, 700.0f));
				ImGui::Begin(title, nullptr, SceneViewportPanel::GetWindowFlags());
				ImGui::LogToBuffer();
				const Status status = panel.Draw(context);
				text = ImGui::GetCurrentContext()->LogBuffer.c_str();
				ImGui::LogFinish();
				ImGui::End();
				return status;
			};
			const auto press = [&ui, &draw](ImGuiKey key)
			{
				ImGui::GetIO().AddKeyEvent(key, true);
				REQUIRE(ui.Frame(draw));
				ImGui::GetIO().AddKeyEvent(key, false);
				REQUIRE(ui.Frame(draw));
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			CAPTURE(text);
			// ImGui logs button decorations and visible text as separate, space-delimited fragments.
			REQUIRE(text.contains("[ World ]"));
			CHECK_FALSE(text.contains("[ Local ]"));
			focusScale = true;
			REQUIRE(ui.Frame(draw));
			auto* window = ImGui::FindWindowByName(title);
			REQUIRE(window != nullptr);
			auto& gui = *ImGui::GetCurrentContext();
			REQUIRE(gui.NavIdIsAlive);
			REQUIRE(gui.NavId == window->GetID("Scale"));

			// Tab from the preceding control discovers the live space button without assuming its changing label/ID.
			// After this initial focus, both activations use only ordinary key events; no refocus repairs a stale ID.
			press(ImGuiKey_Tab);
			REQUIRE(gui.NavWindow == window);
			REQUIRE(gui.NavIdIsAlive);
			const ImGuiID spaceButton = gui.NavId;
			REQUIRE(spaceButton != window->GetID("Scale"));
			const ImRect button = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
			REQUIRE(button.GetWidth() > 0.0f);
			REQUIRE(button.GetHeight() > 0.0f);
			const auto history = context.Editor.GetHistory().GetUndoCount();
			for (const char* expected : { "Local", "World" })
			{
				press(ImGuiKey_Space);
				CAPTURE(text);
				CHECK(text.contains(std::string("[ ") + expected + " ]"));
				CHECK_FALSE(text.contains(std::string(expected) == "Local" ? "[ World ]" : "[ Local ]"));
				CHECK(gui.NavWindow == window);
				CHECK(gui.NavId == spaceButton);
				CHECK(gui.NavIdIsAlive);
			}
			CHECK(viewport.Rectangle.Size.x > 0.0f);
			CHECK(viewport.Rectangle.Size.y > 0.0f);
			CHECK(context.Editor.GetHistory().GetUndoCount() == history);
		}

		TEST_CASE("SceneViewportPanel: drawing renders into the caller's window instead of beginning its own")
		{
			Test::PanelInteractionFixture fixture("SceneOwnWindow");
			auto& services = fixture.GetContext();
			SceneToolbarViewport viewport;
			EditorPanelContext context{ services.Editor, services.Automation, services.Actions, services.AutomationControls,
				viewport, services.InspectorEdits, services.Thumbnails, services.Gizmos };
			Test::PanelInteractionUi ui(false);
			Utils::ApplyEditorStyle();
			SceneViewportPanel panel;
			ImRect host;
			for (int frame = 0; frame < 2; ++frame)
			{
				REQUIRE(ui.Frame([&panel, &context, &host]() -> Status
				{
					ImGuizmo::BeginFrame();
					ImGui::SetNextWindowPos(ImVec2(20.0f, 30.0f));
					ImGui::SetNextWindowSize(ImVec2(700.0f, 500.0f));
					ImGui::Begin("Viewport host", nullptr, SceneViewportPanel::GetWindowFlags());
					host = ImGui::GetCurrentWindow()->InnerRect;
					const Status status = panel.Draw(context);
					ImGui::End();
					return status;
				}));
			}
			// The host's Begin applies the window flags; a second Begin by the panel would have opened its titled window.
			CHECK(ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::SceneViewport)) == nullptr);
			const ImGuiWindow* window = ImGui::FindWindowByName("Viewport host");
			REQUIRE(window != nullptr);
			CHECK((window->Flags & ImGuiWindowFlags_NoScrollWithMouse) != 0);
			CHECK((window->Flags & ImGuiWindowFlags_NoScrollbar) != 0);
			CHECK(viewport.Rectangle.Min.x >= host.Min.x);
			CHECK(viewport.Rectangle.Min.y >= host.Min.y);
			CHECK(viewport.Rectangle.Min.x + viewport.Rectangle.Size.x <= host.Max.x + 0.5f);
			CHECK(viewport.Rectangle.Min.y + viewport.Rectangle.Size.y <= host.Max.y + 0.5f);
		}

		TEST_CASE("SceneViewportPanel: Alt and the left button orbit the camera on every frame of the drag")
		{
			ScenePanelHarness test("SceneOrbit");
			const ImVec2 origin = ScenePanelHarness::SceneWindow()->Pos;
			const ExplicitRenderCamera initial = test.Camera();
			const float distance = glm::length(initial.Position - initial.Target);
			test.MoveTo(test.Center());
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
			test.FrameOk();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			glm::vec3 previous = test.Camera().Position;
			for (int frame = 1; frame <= 30; ++frame)
			{
				CAPTURE(frame);
				test.MoveTo(ImVec2(test.Center().x + 6.0f * static_cast<float>(frame), test.Center().y + 2.0f * static_cast<float>(frame)));
				const ExplicitRenderCamera& camera = test.Camera();
				CHECK(camera.Position != previous);
				CHECK(camera.Target == initial.Target);
				CHECK(glm::length(camera.Position - camera.Target) == doctest::Approx(distance).epsilon(1e-4));
				previous = camera.Position;
			}
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
			test.FrameOk();
			CHECK(test.Viewport.Clicks.empty()); // Alt+click orbits and never picks
			CHECK(ScenePanelHarness::SceneWindow()->Pos.x == origin.x);
			CHECK(ScenePanelHarness::SceneWindow()->Pos.y == origin.y);
		}

		TEST_CASE("SceneViewportPanel: wheel zoom, middle-button pan and right-button fly work without a prior left click")
		{
			ScenePanelHarness test("SceneNavigation");
			test.FocusOther();
			test.MoveTo(test.Center());
			const ExplicitRenderCamera initial = test.Camera();

			// The wheel zooms the hovered view and leaves keyboard focus where it was.
			ImGui::GetIO().AddMouseWheelEvent(0.0f, 1.0f);
			test.FrameOk();
			const ExplicitRenderCamera zoomed = test.Camera();
			CHECK(glm::length(zoomed.Position - zoomed.Target) < glm::length(initial.Position - initial.Target));
			CHECK(zoomed.Target == initial.Target);
			CHECK_FALSE(ScenePanelHarness::IsSceneFocused());

			// The middle button pans from the first press, and that press focuses the view.
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
			test.FrameOk();
			CHECK(ScenePanelHarness::IsSceneFocused());
			for (int frame = 1; frame <= 5; ++frame)
				test.MoveTo(ImVec2(test.Center().x + 10.0f * static_cast<float>(frame), test.Center().y));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
			test.FrameOk();
			const ExplicitRenderCamera panned = test.Camera();
			CHECK(panned.Target != zoomed.Target);
			CHECK(glm::length((panned.Position - panned.Target) - (zoomed.Position - zoomed.Target)) < 1.0e-4f);

			// The right button flies with WASD from an unfocused view too, preserving the view direction.
			test.FocusOther();
			test.MoveTo(test.Center());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
			test.FrameOk();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
			for (int frame = 0; frame < 6; ++frame)
				test.FrameOk();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
			test.FrameOk();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			test.FrameOk();
			const ExplicitRenderCamera flown = test.Camera();
			const glm::vec3 forward = glm::normalize(panned.Target - panned.Position);
			CHECK(glm::dot(flown.Position - panned.Position, forward) > 0.1f);
			CHECK(glm::length(glm::normalize(flown.Target - flown.Position) - forward) < 1.0e-4f);
			CHECK(test.Viewport.Clicks.empty());
		}

		TEST_CASE("SceneViewportPanel: the first left click on an unfocused view focuses it and requests one pick")
		{
			ScenePanelHarness test("ScenePick");
			test.FocusOther();
			test.MoveTo(test.Center());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			CHECK(ScenePanelHarness::IsSceneFocused());
			REQUIRE(test.Viewport.Clicks.size() == 1);
			const EditorViewportClick& click = test.Viewport.Clicks.front();
			CHECK(click.View == ViewportView::Scene);
			CHECK(click.Pixel.X == test.Viewport.Image.Width / 2);
			CHECK(click.Pixel.Y == test.Viewport.Image.Height / 2);
			CHECK(click.FrameIndex == test.Viewport.Image.FrameIndex);
			CHECK(click.ViewGeneration == test.Viewport.Image.Generation);
			CHECK_FALSE(click.ExtendSelection);
			// An image rendered without a picking image (no scene content to pick) takes no pick request.
			test.Viewport.Image.Picking = false;
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			CHECK(test.Viewport.Clicks.size() == 1);
		}

		TEST_CASE("SceneViewportPanel: without a scene the view shows its placeholder and takes no pick")
		{
			ScenePanelHarness test("SceneNoScene", false);
			REQUIRE_FALSE(test.GetEditor().HasScene());
			test.MoveTo(test.Center());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			CHECK(test.Viewport.Clicks.empty());
			CHECK(test.Viewport.Rectangle.ImageSize == glm::uvec2(0));
			CHECK(test.Viewport.Rectangle.Size.x > 0.0f);
		}

		TEST_CASE("SceneViewportPanel: the gizmo shows while another panel has focus and its drag commits one undo step in place")
		{
			ScenePanelHarness test("SceneGizmo");
			EditorContext& editor = test.GetEditor();
			UUID box;
			{
				SceneEdit edit(editor, "Create box");
				box = editor.GetScene().CreateEntity("Box").GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(editor.SetSelection({ box }, SceneTarget::Edit).has_value());
			const size_t history = editor.GetHistory().GetUndoCount();
			const ImVec2 origin = ScenePanelHarness::SceneWindow()->Pos;
			test.FocusOther();
			test.MoveTo(test.Center());
			test.FrameOk();
			// Hovering the unfocused view reaches the gizmo's handle: it is drawn and hit-tested without focus.
			REQUIRE(ImGuizmo::IsOver());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			REQUIRE(test.GetContext().Gizmos.IsDragging());
			CHECK(test.Viewport.Clicks.empty()); // the handle took the press: no background pick
			for (int frame = 1; frame <= 3; ++frame)
				test.MoveTo(ImVec2(test.Center().x + 12.0f * static_cast<float>(frame), test.Center().y));
			CHECK(test.GetContext().Gizmos.IsDragging());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			CHECK_FALSE(test.GetContext().Gizmos.IsDragging());
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			CHECK(editor.GetHistory().GetUndoLabel() == "Transform selection");
			const glm::vec3 moved = editor.GetScene().FindEntityByID(box).GetComponent<TransformComponent>().Translation;
			CHECK(moved.x > 0.01f);
			// The drag moved the entity, never the floating window under it.
			CHECK(ScenePanelHarness::SceneWindow()->Pos.x == origin.x);
			CHECK(ScenePanelHarness::SceneWindow()->Pos.y == origin.y);
		}

		TEST_CASE("SceneViewportPanel: a press in the first frame over a gizmo handle starts the drag")
		{
			ScenePanelHarness test("SceneGizmoFirstFrame");
			EditorContext& editor = test.GetEditor();
			UUID box;
			{
				SceneEdit edit(editor, "Create box");
				box = editor.GetScene().CreateEntity("Box").GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(editor.SetSelection({ box }, SceneTarget::Edit).has_value());
			const size_t history = editor.GetHistory().GetUndoCount();
			// The pointer rests on the image's background, away from the handles, for a few frames.
			const ImVec2 corner(test.Viewport.Rectangle.Min.x + 40.0f, test.Viewport.Rectangle.Min.y + 40.0f);
			test.MoveTo(corner);
			test.FrameOk();
			REQUIRE_FALSE(ImGuizmo::IsOver());
			// One slow frame: the pointer reaches the gizmo's centre handle and the left button goes down in it.
			ImGui::GetIO().AddMousePosEvent(test.Center().x, test.Center().y);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			REQUIRE(test.GetContext().Gizmos.IsDragging());
			CHECK(test.Viewport.Clicks.empty()); // the handle took the press: no background pick
			for (int frame = 1; frame <= 3; ++frame)
				test.MoveTo(ImVec2(test.Center().x + 12.0f * static_cast<float>(frame), test.Center().y));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			CHECK_FALSE(test.GetContext().Gizmos.IsDragging());
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			CHECK(editor.GetScene().FindEntityByID(box).GetComponent<TransformComponent>().Translation.x > 0.01f);
			CHECK(test.Viewport.Clicks.empty());
		}

		TEST_CASE("SceneViewportPanel: F frames the selection and W E R X switch the gizmo in the focused hovered view")
		{
			ScenePanelHarness test("SceneShortcuts");
			EditorContext& editor = test.GetEditor();
			UUID box;
			{
				SceneEdit edit(editor, "Create box");
				const Entity entity = editor.GetScene().CreateEntity("Box");
				entity.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = { 4.0f, 1.0f, -3.0f };
				});
				box = entity.GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(editor.SetSelection({ box }, SceneTarget::Edit).has_value());
			// Away from the gizmo handles, which would own the pointer.
			const ImVec2 corner(test.Viewport.Rectangle.Min.x + 40.0f, test.Viewport.Rectangle.Min.y + 40.0f);
			test.FrameOk([]()
			{
				ImGui::SetWindowFocus(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
			});
			test.MoveTo(corner);
			const auto press = [&test](ImGuiKey key)
			{
				ImGui::GetIO().AddKeyEvent(key, true);
				test.FrameOk();
				ImGui::GetIO().AddKeyEvent(key, false);
				test.FrameOk();
			};
			press(ImGuiKey_E);
			CHECK(test.Panel.GetGizmoSettings().Operation == GizmoOperation::Rotate);
			press(ImGuiKey_R);
			CHECK(test.Panel.GetGizmoSettings().Operation == GizmoOperation::Scale);
			press(ImGuiKey_W);
			CHECK(test.Panel.GetGizmoSettings().Operation == GizmoOperation::Translate);
			const GizmoSpace space = test.Panel.GetGizmoSettings().Space;
			press(ImGuiKey_X);
			CHECK(test.Panel.GetGizmoSettings().Space != space);
			press(ImGuiKey_F);
			test.GetContext().Actions.Pump();
			test.FrameOk();
			CHECK(test.Camera().Target == glm::vec3(4.0f, 1.0f, -3.0f));
			// Unhovered, the same keys reach neither the gizmo nor the camera.
			test.MoveTo(ImVec2(test.Viewport.Rectangle.Min.x - 5.0f, test.Viewport.Rectangle.Min.y - 5.0f));
			press(ImGuiKey_E);
			CHECK(test.Panel.GetGizmoSettings().Operation == GizmoOperation::Translate);
		}

		TEST_CASE("SceneViewportPanel: dropped assets create, update or instantiate through queued commands")
		{
			ScenePanelHarness test("SceneDrop", true, true);
			EditorContext& editor = test.GetEditor();
			Scene& scene = editor.GetScene();
			const size_t entities = CountEntities(scene);
			const size_t history = editor.GetHistory().GetUndoCount();
			SUBCASE("a mesh creates an entity rendering it")
			{
				REQUIRE(DropOnImage(test, AssetPayload(BuiltinAssetHandles::CubeMesh)).has_value());
				REQUIRE(CountEntities(scene) == entities + 1);
				const ConstEntity created = scene.FindEntityByID(scene.GetCanonicalOrder().back());
				CHECK(created.GetName() == "Mesh");
				CHECK(created.GetComponent<MeshRendererComponent>().Mesh.GetHandle() == BuiltinAssetHandles::CubeMesh);
				CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			}
			SUBCASE("a mesh dropped while playing goes to the runtime scene only")
			{
				REQUIRE(test.GetAutomation().Call("play.start", Json{ { "paused", true } }).has_value());
				test.RefreshCamera();
				const Scene& runtime = editor.GetPlay().GetSession()->GetScene();
				const size_t running = CountEntities(runtime);
				REQUIRE(DropOnImage(test, AssetPayload(BuiltinAssetHandles::CubeMesh)).has_value());
				CHECK(CountEntities(runtime) == running + 1);
				CHECK(CountEntities(scene) == entities);
				CHECK(editor.GetHistory().GetUndoCount() == history);
			}
			SUBCASE("a prefab instantiates in the edit scene and is refused while playing")
			{
				REQUIRE(test.GetAutomation().Call("entity.create", Json{ { "name", "Crate" } }).has_value());
				const auto created = test.GetAutomation().Call("prefab.create", Json{ { "entity", "/Crate" }, { "path", "Assets/Prefabs/Crate.prefab" } });
				REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
				const auto prefab = JsonReader((*created)["prefab"]["id"]).ReadUUID();
				REQUIRE(prefab.has_value());
				const size_t before = CountEntities(scene);
				REQUIRE(DropOnImage(test, AssetPayload(*prefab)).has_value());
				CHECK(CountEntities(scene) == before + 1);
				REQUIRE(test.GetAutomation().Call("play.start", Json{ { "paused", true } }).has_value());
				test.RefreshCamera();
				const Status refused = DropOnImage(test, AssetPayload(*prefab));
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			}
			SUBCASE("an environment updates the scene's Environment entity or creates one")
			{
				const Status created = DropOnImage(test, AssetPayload(BuiltinAssetHandles::StudioEnvironment));
				REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
				REQUIRE(CountEntities(scene) == entities + 1);
				const UUID environment = scene.GetCanonicalOrder().back();
				CHECK(scene.FindEntityByID(environment).GetName() == "Environment");
				CHECK(scene.FindEntityByID(environment).GetComponent<EnvironmentComponent>().Environment.GetHandle() == BuiltinAssetHandles::StudioEnvironment);
				REQUIRE(DropOnImage(test, AssetPayload(BuiltinAssetHandles::SkyEnvironment)).has_value());
				CHECK(CountEntities(scene) == entities + 1);
				CHECK(scene.FindEntityByID(environment).GetComponent<EnvironmentComponent>().Environment.GetHandle() == BuiltinAssetHandles::SkyEnvironment);
			}
			SUBCASE("malformed payloads and unknown assets are refused without a command")
			{
				std::vector<std::byte> truncated = AssetPayload(BuiltinAssetHandles::CubeMesh);
				truncated.pop_back();
				const Status malformed = DropOnImage(test, truncated);
				REQUIRE_FALSE(malformed.has_value());
				CHECK(malformed.error().GetCode() == ErrorCode::InvalidArgument);
				const Status unknown = DropOnImage(test, AssetPayload(AssetHandle(0x0123456789abcdefull)));
				REQUIRE_FALSE(unknown.has_value());
				CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
				const Status material = DropOnImage(test, AssetPayload(BuiltinAssetHandles::DefaultMaterial));
				REQUIRE_FALSE(material.has_value());
				CHECK(material.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(CountEntities(scene) == entities);
				CHECK(editor.GetHistory().GetUndoCount() == history);
			}
		}

		// M22 (Docs/Decisions/0033-m22-contract.md decisions 33 and 34): the UI preview shows the game frame letterboxed in the
		// view, without the camera's controls, the gizmo or mesh picking; a click selects the topmost UI element and a handle
		// drag resizes the selected one in one undo step.
		TEST_CASE("SceneViewportPanel: the UI preview letterboxes the game frame, selects an element with a click and resizes it with a handle drag")
		{
			ScenePanelHarness test("SceneUiPreview");
			EditorContext& editor = test.GetEditor();
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(400, 300)).has_value());
			UUID panel;
			{
				SceneEdit edit(editor, "Create UI");
				const Entity canvas = editor.GetScene().CreateEntity("Canvas");
				canvas.AddComponent<UiCanvasComponent>();
				// [100, 300] x [50, 150] scene pixels on a canvas of one pixel per unit.
				const Entity element = editor.GetScene().CreateEntity("Panel", canvas);
				element.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
					.AnchorMax = glm::vec2(0.0f),
					.Pivot = glm::vec2(0.0f),
					.Position = glm::vec2(100.0f, 50.0f),
					.Size = glm::vec2(200.0f, 100.0f) });
				panel = element.GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(test.GetAutomation().Call("viewport.setOptions", Json{ { "uiPreview", true } }).has_value());
			REQUIRE(editor.SetSelection({}, SceneTarget::Edit).has_value());
			// The host renders the preview at the game frame.
			test.Viewport.Image.Width = 400;
			test.Viewport.Image.Height = 300;
			test.FrameOk();
			const EditorViewportRect frame = test.Viewport.Rectangle;
			CHECK(frame.ImageSize == glm::uvec2(400, 300));
			REQUIRE(frame.Size.x > 0.0f);
			CHECK(frame.Size.x * 300.0f == doctest::Approx(frame.Size.y * 400.0f));
			const float scale = frame.Size.x / 400.0f;
			const auto at = [&frame, scale](glm::vec2 scenePixel)
			{
				return ImVec2(frame.Min.x + scenePixel.x * scale, frame.Min.y + scenePixel.y * scale);
			};
			const ExplicitRenderCamera camera = test.Camera();

			// A click on the element selects it, and requests no mesh pick.
			test.MoveTo(at(glm::vec2(200.0f, 100.0f)));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			test.GetContext().Actions.Pump();
			test.FrameOk();
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == panel);
			CHECK(test.Viewport.Clicks.empty());

			// The right edge dragged 50 scene pixels right: one entity.update at the release, the left edge in place.
			const size_t history = editor.GetHistory().GetUndoCount();
			test.MoveTo(at(glm::vec2(300.0f, 100.0f)));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			for (int step = 1; step <= 5; ++step)
				test.MoveTo(at(glm::vec2(300.0f + 10.0f * static_cast<float>(step), 100.0f)));
			CHECK(editor.GetHistory().GetUndoCount() == history);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			test.GetContext().Actions.Pump();
			test.FrameOk();
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			const UiRectComponent& resized = editor.GetScene().FindEntityByID(panel).GetComponent<UiRectComponent>();
			CHECK(resized.Size.x == doctest::Approx(250.0f).epsilon(1e-3));
			CHECK(resized.Size.y == 100.0f);
			CHECK(resized.Position == glm::vec2(100.0f, 50.0f));
			// Neither the camera nor the transform gizmo took the presses.
			CHECK(test.Camera().Position == camera.Position);
			CHECK(test.Camera().Target == camera.Target);
			CHECK_FALSE(test.GetContext().Gizmos.IsDragging());
			CHECK(test.Viewport.Clicks.empty());

			// A click over no element clears the selection; without the preview the image fills the view again.
			test.MoveTo(at(glm::vec2(20.0f, 280.0f)));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			test.GetContext().Actions.Pump();
			CHECK(editor.GetSelection().empty());
			REQUIRE(test.GetAutomation().Call("viewport.setOptions", Json{ { "uiPreview", false } }).has_value());
			test.FrameOk();
			const EditorViewportRect full = test.Viewport.Rectangle;
			CHECK(full.Size.x >= frame.Size.x);
			CHECK(full.Size.y >= frame.Size.y);
			CHECK((full.Size.x > frame.Size.x + 1.0f || full.Size.y > frame.Size.y + 1.0f));
		}

		// M22 (decision 34; the review of W22.2): over the UI preview, a click on the selected element's body keeps it
		// selected and writes nothing, a drag of the body moves it by the pointer's delta in one undo step, and a drag that
		// Escape cancels, or whose scene changed after it began (its entity.update's ifRevision), leaves the element as it was.
		TEST_CASE("SceneViewportPanel: the UI preview moves the selected element with a body drag, and a click, Escape or a changed scene writes nothing")
		{
			ScenePanelHarness test("SceneUiPreviewDrag");
			EditorContext& editor = test.GetEditor();
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(400, 300)).has_value());
			UUID panel;
			{
				SceneEdit edit(editor, "Create UI");
				const Entity canvas = editor.GetScene().CreateEntity("Canvas");
				canvas.AddComponent<UiCanvasComponent>();
				// [100, 300] x [50, 150] scene pixels on a canvas of one pixel per unit.
				const Entity element = editor.GetScene().CreateEntity("Panel", canvas);
				element.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
					.AnchorMax = glm::vec2(0.0f),
					.Pivot = glm::vec2(0.0f),
					.Position = glm::vec2(100.0f, 50.0f),
					.Size = glm::vec2(200.0f, 100.0f) });
				panel = element.GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(test.GetAutomation().Call("viewport.setOptions", Json{ { "uiPreview", true } }).has_value());
			REQUIRE(editor.SetSelection({ panel }, SceneTarget::Edit).has_value());
			test.Viewport.Image.Width = 400;
			test.Viewport.Image.Height = 300;
			test.FrameOk();
			const EditorViewportRect frame = test.Viewport.Rectangle;
			const float scale = frame.Size.x / 400.0f;
			const auto at = [&frame, scale](glm::vec2 scenePixel)
			{
				return ImVec2(frame.Min.x + scenePixel.x * scale, frame.Min.y + scenePixel.y * scale);
			};
			const auto rect = [&editor, panel]()
			{
				return editor.GetScene().FindEntityByID(panel).GetComponent<UiRectComponent>();
			};
			// A press at `from`, the pointer moved to `to` in five steps (past ImGui's drag threshold), the press held.
			const auto pressAndMove = [&test, &at](glm::vec2 from, glm::vec2 to)
			{
				test.MoveTo(at(from));
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				test.FrameOk();
				for (int step = 1; step <= 5; ++step)
					test.MoveTo(at(from + (to - from) * (static_cast<float>(step) / 5.0f)));
			};
			const auto release = [&test]()
			{
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				test.FrameOk();
				test.GetContext().Actions.Pump();
			};

			// A click on the selected element's body selects it again and writes nothing.
			size_t history = editor.GetHistory().GetUndoCount();
			pressAndMove(glm::vec2(200.0f, 100.0f), glm::vec2(200.0f, 100.0f));
			release();
			test.FrameOk();
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == panel);
			CHECK(editor.GetHistory().GetUndoCount() == history);
			CHECK(rect().Position == glm::vec2(100.0f, 50.0f));

			// The body dragged 30 right and 20 down: Position follows by (30, 20) canvas units, within the panel pixel ImGui
			// rounds the pointer to, the size kept, one undo step.
			pressAndMove(glm::vec2(200.0f, 100.0f), glm::vec2(230.0f, 120.0f));
			CHECK(editor.GetHistory().GetUndoCount() == history);
			release();
			test.FrameOk();
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			CHECK(std::abs(rect().Position.x - 130.0f) <= 1.0f / scale);
			CHECK(std::abs(rect().Position.y - 70.0f) <= 1.0f / scale);
			CHECK(rect().Size == glm::vec2(200.0f, 100.0f));
			const UiRectComponent moved = rect();

			// The right edge (now at x 330, y 70 to 170) dragged, then Escape before the release: no write.
			history = editor.GetHistory().GetUndoCount();
			const glm::vec2 edge(moved.Position.x + moved.Size.x, moved.Position.y + moved.Size.y * 0.5f);
			pressAndMove(edge, edge + glm::vec2(40.0f, 0.0f));
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
			test.FrameOk();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
			release();
			test.FrameOk();
			CHECK(editor.GetHistory().GetUndoCount() == history);
			CHECK(rect().Size == moved.Size);
			CHECK(rect().Position == moved.Position);

			// The right edge dragged while another edit changes the scene: the release's entity.update is refused at once by
			// its ifRevision, so only that edit is recorded and the element keeps its size.
			pressAndMove(edge, edge + glm::vec2(40.0f, 0.0f));
			{
				SceneEdit edit(editor, "Add an entity");
				static_cast<void>(editor.GetScene().CreateEntity("Other"));
				REQUIRE(edit.Commit().has_value());
			}
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			const Status refused = test.Frame();
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Conflict);
			test.GetContext().Actions.Pump();
			test.FrameOk();
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			CHECK(rect().Size == moved.Size);
		}

		// C22.3 (the review of C22.3, its major on the consumers of World canvases' pixels; stream W): the UI preview shows
		// and edits Screen canvases' elements only. An element of a World canvas, whose rectangle is in its canvas's own
		// pixels, is neither selected by a click at those pixels nor given the rect tool's handles while selected (a World
		// canvas is placed by its entity's Transform and its elements are edited in the inspector or with entity.update).
		TEST_CASE("SceneViewportPanel: the UI preview neither selects nor resizes an element of a World canvas")
		{
			ScenePanelHarness test("SceneUiPreviewWorld");
			EditorContext& editor = test.GetEditor();
			REQUIRE(editor.GetViewportState().SetGameResolution(glm::uvec2(400, 300)).has_value());
			UUID sign;
			{
				SceneEdit edit(editor, "Create UI");
				const Entity board = editor.GetScene().CreateEntity("Board");
				board.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 400, .WorldHeight = 300 });
				// [100, 300] x [50, 150] in the World canvas's own pixels.
				const Entity element = editor.GetScene().CreateEntity("Sign", board);
				element.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
					.AnchorMax = glm::vec2(0.0f),
					.Pivot = glm::vec2(0.0f),
					.Position = glm::vec2(100.0f, 50.0f),
					.Size = glm::vec2(200.0f, 100.0f) });
				sign = element.GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(test.GetAutomation().Call("viewport.setOptions", Json{ { "uiPreview", true } }).has_value());
			REQUIRE(editor.SetSelection({}, SceneTarget::Edit).has_value());
			test.Viewport.Image.Width = 400;
			test.Viewport.Image.Height = 300;
			test.FrameOk();
			const EditorViewportRect frame = test.Viewport.Rectangle;
			REQUIRE(frame.Size.x > 0.0f);
			const float scale = frame.Size.x / 400.0f;
			const auto at = [&frame, scale](glm::vec2 scenePixel)
			{
				return ImVec2(frame.Min.x + scenePixel.x * scale, frame.Min.y + scenePixel.y * scale);
			};
			const auto rect = [&editor, sign]() -> const UiRectComponent&
			{
				return editor.GetScene().FindEntityByID(sign).GetComponent<UiRectComponent>();
			};

			// A click inside the sign's canvas-pixel rectangle selects nothing.
			test.MoveTo(at(glm::vec2(200.0f, 100.0f)));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			test.GetContext().Actions.Pump();
			test.FrameOk();
			CHECK(editor.GetSelection().empty());

			// Selected in the hierarchy, its canvas-pixel right edge takes no handle drag: nothing is written.
			REQUIRE(editor.SetSelection({ sign }, SceneTarget::Edit).has_value());
			test.FrameOk();
			const size_t history = editor.GetHistory().GetUndoCount();
			test.MoveTo(at(glm::vec2(300.0f, 100.0f)));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			for (int step = 1; step <= 5; ++step)
				test.MoveTo(at(glm::vec2(300.0f + 10.0f * static_cast<float>(step), 100.0f)));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			test.GetContext().Actions.Pump();
			test.FrameOk();
			CHECK(editor.GetHistory().GetUndoCount() == history);
			CHECK(rect().Size == glm::vec2(200.0f, 100.0f));
			CHECK(rect().Position == glm::vec2(100.0f, 50.0f));
		}
	}

}
