#include "TestsPCH.h"
#include "Editor/Panels/SceneViewportPanel.h"

#include "Editor/PanelInteractionFixture.h"
#include "Editor/Ui/EditorStyle.h"
#include "Editor/Viewport/GizmoOverlay.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Viewport/EditorCamera.h"
#include "EditorCore/Viewport/TilemapPainting.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
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
#include <optional>
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

		// Activates the toolbar's 2D toggle with the keyboard (Space on the focused button), then runs its queued
		// viewport.setOptions and the frame that collects its result.
		void Toggle2D(ScenePanelHarness& harness)
		{
			harness.FrameOk([]()
			{
				ImGuiWindow* window = ScenePanelHarness::SceneWindow();
				REQUIRE(window != nullptr);
				ImGui::SetWindowFocus(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
				ImGui::SetNavID(window->GetID("2D"), ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
				ImGui::SetNavCursorVisible(true);
			});
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, true);
			harness.FrameOk();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, false);
			harness.FrameOk();
			harness.GetContext().Actions.Pump();
			harness.FrameOk();
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

		// M21 (Docs/Decisions/0032-m21-contract.md decision 14): the toolbar's 2D toggle and the 2D mode's navigation.
		TEST_CASE("SceneViewportPanel: the 2D toggle makes a 2D camera that pans with every button and zooms in the plane")
		{
			ScenePanelHarness test("Scene2D");
			const size_t history = test.GetEditor().GetHistory().GetUndoCount();
			Toggle2D(test);
			REQUIRE(test.GetEditor().GetViewportState().GetOptions().Mode2D);
			const ExplicitRenderCamera flat = test.Camera();
			CHECK(flat.Projection == RenderProjection::Orthographic);
			CHECK(flat.Position == glm::vec3(flat.Target.x, flat.Target.y, EditorCamera::Make2DDistance));
			CHECK(flat.Target.z == 0.0f);
			test.RefreshCamera();

			// The middle button pans: the content follows the pointer, so the camera moves against it, in X only here.
			test.MoveTo(test.Center());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
			test.FrameOk();
			for (int frame = 1; frame <= 4; ++frame)
				test.MoveTo(ImVec2(test.Center().x + 10.0f * static_cast<float>(frame), test.Center().y));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
			test.FrameOk();
			const ExplicitRenderCamera panned = test.Camera();
			CHECK(panned.Target.x < flat.Target.x);
			CHECK(panned.Target.y == flat.Target.y);
			CHECK(panned.Target.z == 0.0f);
			CHECK(panned.Position.z == EditorCamera::Make2DDistance);
			CHECK(panned.OrthographicSize == flat.OrthographicSize);

			// The right button pans too instead of flying, and W moves nothing.
			test.MoveTo(test.Center());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
			test.FrameOk();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
			for (int frame = 1; frame <= 4; ++frame)
				test.MoveTo(ImVec2(test.Center().x, test.Center().y + 10.0f * static_cast<float>(frame)));
			ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			test.FrameOk();
			const ExplicitRenderCamera dragged = test.Camera();
			CHECK(dragged.Target.x == panned.Target.x);
			CHECK(dragged.Target.y > panned.Target.y);
			CHECK(dragged.Position.z == EditorCamera::Make2DDistance);
			CHECK(dragged.Projection == RenderProjection::Orthographic);

			// The wheel zooms the orthographic size and keeps the pose.
			ImGui::GetIO().AddMouseWheelEvent(0.0f, 1.0f);
			test.FrameOk();
			const ExplicitRenderCamera zoomed = test.Camera();
			CHECK(zoomed.OrthographicSize == doctest::Approx(dragged.OrthographicSize / 1.2f));
			CHECK(zoomed.Position == dragged.Position);
			CHECK(test.Viewport.Clicks.empty());

			// Turning the mode off keeps the camera; neither is a scene change.
			Toggle2D(test);
			CHECK_FALSE(test.GetEditor().GetViewportState().GetOptions().Mode2D);
			CHECK(test.Camera().Position == zoomed.Position);
			CHECK(test.Camera().Projection == RenderProjection::Orthographic);
			CHECK(test.GetEditor().GetHistory().GetUndoCount() == history);
		}

		TEST_CASE("SceneViewportPanel: the 2D mode's gizmo moves in X and Y, rotates about Z and scales in X and Y")
		{
			const auto bits = [](ImGuizmo::OPERATION operation)
			{
				return static_cast<uint32_t>(operation);
			};
			CHECK(GetGizmoOperationMask(GizmoOperation::Translate, false) == bits(ImGuizmo::TRANSLATE));
			CHECK(GetGizmoOperationMask(GizmoOperation::Rotate, false) == bits(ImGuizmo::ROTATE));
			CHECK(GetGizmoOperationMask(GizmoOperation::Scale, false) == bits(ImGuizmo::SCALE));
			CHECK(GetGizmoOperationMask(GizmoOperation::Translate, true) == bits(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y));
			CHECK(GetGizmoOperationMask(GizmoOperation::Rotate, true) == bits(ImGuizmo::ROTATE_Z));
			CHECK(GetGizmoOperationMask(GizmoOperation::Scale, true) == bits(ImGuizmo::SCALE_X | ImGuizmo::SCALE_Y));
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
			SUBCASE("a texture or a sprite sheet creates a sprite where the cursor's ray meets z = 0")
			{
				// The default camera looks from (0, 3, 10) at the origin, so the image's centre shows about the origin.
				REQUIRE(DropOnImage(test, AssetPayload(BuiltinAssetHandles::CheckerTexture)).has_value());
				REQUIRE(CountEntities(scene) == entities + 1);
				const ConstEntity textured = scene.FindEntityByID(scene.GetCanonicalOrder().back());
				CHECK(textured.GetName() == "Sprite");
				REQUIRE(textured.HasComponent<SpriteRendererComponent>());
				CHECK(textured.GetComponent<SpriteRendererComponent>().Texture.GetHandle() == BuiltinAssetHandles::CheckerTexture);
				CHECK_FALSE(textured.GetComponent<SpriteRendererComponent>().Sheet.GetHandle().IsValid());
				const glm::vec3 position = textured.GetComponent<TransformComponent>().Translation;
				CHECK(position.z == 0.0f);
				CHECK(std::abs(position.x) < 0.1f);
				CHECK(std::abs(position.y) < 0.1f);
				CHECK(editor.GetHistory().GetUndoCount() == history + 1);

				const auto created = test.GetAutomation().Call("asset.create", Json{ { "type", "SpriteSheet" }, { "path", "Assets/Sheets/Drop.spritesheet" } });
				REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
				const auto sheet = JsonReader((*created)["asset"]["id"]).ReadUUID();
				REQUIRE(sheet.has_value());
				REQUIRE(DropOnImage(test, AssetPayload(*sheet)).has_value());
				REQUIRE(CountEntities(scene) == entities + 2);
				const ConstEntity sheeted = scene.FindEntityByID(scene.GetCanonicalOrder().back());
				CHECK(sheeted.GetComponent<SpriteRendererComponent>().Sheet.GetHandle() == *sheet);
				CHECK(sheeted.GetComponent<SpriteRendererComponent>().Frame == 0);
				CHECK(sheeted.GetComponent<TransformComponent>().Translation.z == 0.0f);
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

		// M21 (Docs/Decisions/0032-m21-contract.md decision 26): the tile brush. It needs stream T1's Tilemap asset (its
		// importer, loader and asset.create) and tilemap.paint's registration, so it runs from W21.2's integration (decision 33).
		TEST_CASE("SceneViewportPanel: the tile brush paints a left stroke's cells and erases a right stroke's, each in one tilemap.paint")
		{
			ScenePanelHarness test("SceneTileBrush");
			EditorContext& editor = test.GetEditor();
			Test::AutomationFixture& automation = test.GetAutomation();
			const Json sheet{ { "Texture", "engine://Textures/Checker" },
				{ "Frames", Json::array({ Json{ { "Name", "Ground" }, { "Rect", Json{ { "X", 0 }, { "Y", 0 }, { "Width", 16 }, { "Height", 16 } } } } }) } };
			REQUIRE(automation.Call("asset.create", Json{ { "type", "SpriteSheet" }, { "path", "Assets/Sheets/Brush.spritesheet" }, { "values", sheet } }).has_value());
			const Json palette = Json::array({ Json{ { "Sheet", "Assets/Sheets/Brush.spritesheet" }, { "Frame", 0 }, { "Solid", true } } });
			const auto tilemap = automation.Call("asset.create", Json{ { "type", "Tilemap" }, { "path", "Assets/Tilemaps/Brush.tilemap" }, { "values", Json{ { "Palette", palette } } } });
			REQUIRE_MESSAGE(tilemap.has_value(), (tilemap ? std::string() : tilemap.error().ToString()));
			const auto created = automation.Call("entity.create", Json{ { "name", "Ground" }, { "components", Json{ { "TilemapRenderer", Json{ { "Tilemap", "Assets/Tilemaps/Brush.tilemap" } } } } } });
			REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
			const auto ground = JsonReader((*created)["entity"]["id"]).ReadUUID();
			REQUIRE(ground.has_value());
			REQUIRE(editor.SetSelection({ *ground }, SceneTarget::Edit).has_value());
			Toggle2D(test);
			test.RefreshCamera();
			test.FrameOk();
			const ExplicitRenderCamera camera = test.Camera();

			// The cells the brush must find: those under three image points, through the displayed image's camera.
			const EditorViewportImage& image = test.Viewport.Image;
			const auto cellAt = [&image](glm::uvec2 pixel)
			{
				const RenderOutputMapping mapping = ComputeRenderOutputMapping(image.Width, image.Height, RenderOutputSettings{});
				const auto ray = ComputeOutputPixelRayInterval(image.Camera, mapping, pixel.x, pixel.y);
				REQUIRE(ray.has_value());
				REQUIRE(ray->has_value());
				const std::optional<glm::ivec2> cell = IntersectTilemapCell((*ray)->Ray, glm::mat4(1.0f), glm::vec2(1.0f));
				REQUIRE(cell.has_value());
				return *cell;
			};
			const auto pointAt = [&test](glm::uvec2 pixel)
			{
				// The image fills the measured rectangle one to one, so pixel p lies at the rectangle's corner plus p.
				return ImVec2(test.Viewport.Rectangle.Min.x + static_cast<float>(pixel.x) + 0.5f, test.Viewport.Rectangle.Min.y + static_cast<float>(pixel.y) + 0.5f);
			};
			const glm::uvec2 middle(image.Width / 2, image.Height / 2);
			const std::vector<glm::uvec2> path = { middle, middle + glm::uvec2(120, 0), middle + glm::uvec2(120, 0) - glm::uvec2(0, 60) };
			std::vector<glm::ivec2> expected;
			for (size_t index = 0; index < path.size(); ++index)
			{
				const std::vector<glm::ivec2> segment = index == 0 ? std::vector<glm::ivec2>{ cellAt(path[0]) } : GetTilemapStrokeCells(cellAt(path[index - 1]), cellAt(path[index]));
				for (const glm::ivec2 cell : segment)
				{
					if (std::ranges::find(expected, cell) == expected.end())
						expected.push_back(cell);
				}
			}
			REQUIRE(expected.size() > 3);
			const auto readCells = [&editor]()
			{
				const VfsPath file = VfsPath::Create("project", "Assets/Tilemaps/Brush.tilemap").value_or(VfsPath());
				const Result<std::string> text = editor.GetVfs().ReadText(file);
				REQUIRE(text.has_value());
				TilemapLoadReport report;
				Result<TilemapData> data = TilemapFromText(*text, editor.GetTypeRegistry(), report, true);
				REQUIRE_MESSAGE(data.has_value(), data.error().ToString());
				return std::move(*data);
			};
			const auto stroke = [&test, &pointAt, &path](ImGuiMouseButton button, size_t points)
			{
				test.MoveTo(pointAt(path[0]));
				ImGui::GetIO().AddMouseButtonEvent(button, true);
				test.FrameOk();
				for (size_t index = 1; index < points; ++index)
					test.MoveTo(pointAt(path[index]));
				ImGui::GetIO().AddMouseButtonEvent(button, false);
				test.FrameOk();
				test.GetContext().Actions.Pump();
				test.FrameOk();
			};

			// Unarmed, the toolbar shows the brush but a left click away from the gizmo still picks.
			test.MoveTo(pointAt(glm::uvec2(40, 40)));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			CHECK(test.Viewport.Clicks.size() == 1);
			CHECK(readCells().Chunks.empty());

			// The picker's first entry arms the brush: Space opens it on "Off", Down moves to the entry and Space takes it.
			test.FrameOk([]()
			{
				ImGuiWindow* window = ScenePanelHarness::SceneWindow();
				REQUIRE(window != nullptr);
				ImGui::SetWindowFocus(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
				ImGui::SetNavID(window->GetID("##TilemapBrushTile"), ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
				ImGui::SetNavCursorVisible(true);
			});
			for (const ImGuiKey key : { ImGuiKey_Space, ImGuiKey_DownArrow, ImGuiKey_Space })
			{
				ImGui::GetIO().AddKeyEvent(key, true);
				test.FrameOk();
				ImGui::GetIO().AddKeyEvent(key, false);
				test.FrameOk();
			}

			// A left stroke through the three points paints every cell it crossed, once each, as one undo step; it neither
			// picks nor moves the camera, and it starts on the tilemap's origin, where the hidden gizmo's handles were.
			const size_t history = editor.GetHistory().GetUndoCount();
			stroke(ImGuiMouseButton_Left, path.size());
			CHECK(test.Viewport.Clicks.size() == 1);
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			CHECK(test.Camera().Position == camera.Position);
			TilemapData painted = readCells();
			for (const glm::ivec2 cell : expected)
			{
				CAPTURE(cell.x);
				CAPTURE(cell.y);
				CHECK(GetTilemapCell(painted, cell) == 1);
			}

			// A right stroke over the first two points erases what it crossed and keeps the rest, again one step, and the
			// right button flies nowhere.
			stroke(ImGuiMouseButton_Right, 2);
			CHECK(editor.GetHistory().GetUndoCount() == history + 2);
			CHECK(test.Camera().Position == camera.Position);
			const std::vector<glm::ivec2> erased = GetTilemapStrokeCells(cellAt(path[0]), cellAt(path[1]));
			painted = readCells();
			for (const glm::ivec2 cell : expected)
			{
				CAPTURE(cell.x);
				CAPTURE(cell.y);
				CHECK(GetTilemapCell(painted, cell) == (std::ranges::find(erased, cell) == erased.end() ? 1u : 0u));
			}
			REQUIRE(automation.Call("edit.undo", Json::object()).has_value());
			CHECK(GetTilemapCell(readCells(), cellAt(path[0])) == 1);

			// A selection change during a stroke cancels it with a Conflict, and nothing is painted.
			const size_t undone = editor.GetHistory().GetUndoCount();
			const std::string before = editor.GetVfs().ReadText(VfsPath::Create("project", "Assets/Tilemaps/Brush.tilemap").value_or(VfsPath())).value_or(std::string());
			test.MoveTo(pointAt(path[2]));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
			test.FrameOk();
			REQUIRE(editor.SetSelection({}, SceneTarget::Edit).has_value());
			const Status cancelled = test.Frame();
			REQUIRE_FALSE(cancelled.has_value());
			CHECK(cancelled.error().GetCode() == ErrorCode::Conflict);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			test.FrameOk();
			test.GetContext().Actions.Pump();
			test.FrameOk();
			CHECK(editor.GetHistory().GetUndoCount() == undone);
			CHECK(editor.GetVfs().ReadText(VfsPath::Create("project", "Assets/Tilemaps/Brush.tilemap").value_or(VfsPath())).value_or(std::string()) == before);
		}

		// Decision 26 (W21.2's review): the brush finds the cell under the cursor at the pose the displayed image shows, the
		// target's rendered world matrix at the image's interpolation alpha, as the click selection picks tiles among the image's
		// own tilemaps. Between two ticks of a play scene a moving tilemap is drawn behind its current pose, and a click paints
		// the cell the image shows under the cursor, never the one the current pose puts there.
		TEST_CASE("SceneViewportPanel: the tile brush paints the cell the image shows under the cursor on a moving tilemap")
		{
			ScenePanelHarness test("SceneTileBrushMoving");
			EditorContext& editor = test.GetEditor();
			Test::AutomationFixture& automation = test.GetAutomation();
			const Json palette = Json::array({ Json{ { "Sheet", nullptr }, { "Frame", 0 }, { "Solid", true } } });
			const auto tilemap = automation.Call("asset.create", Json{ { "type", "Tilemap" }, { "path", "Assets/Tilemaps/Moving.tilemap" }, { "values", Json{ { "Palette", palette } } } });
			REQUIRE_MESSAGE(tilemap.has_value(), (tilemap ? std::string() : tilemap.error().ToString()));
			// A platform the 2D world carries along +X at 240 units a second, without gravity or damping.
			const Json components{ { "TilemapRenderer", Json{ { "Tilemap", "Assets/Tilemaps/Moving.tilemap" } } },
				{ "RigidBody2D", Json{ { "GravityFactor", 0 }, { "LinearDamping", 0 }, { "InitialLinearVelocity", Json::array({ 240, 0 }) } } },
				{ "BoxCollider2D", Json::object() } };
			const auto created = automation.Call("entity.create", Json{ { "name", "Platform" }, { "components", components } });
			REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
			const auto platform = JsonReader((*created)["entity"]["id"]).ReadUUID();
			REQUIRE(platform.has_value());
			Toggle2D(test);
			const auto started = automation.Call("play.start", Json{ { "lockstep", true }, { "mode", "simulate" } });
			REQUIRE_MESSAGE(started.has_value(), (started ? std::string() : started.error().ToString()));
			REQUIRE(automation.Call("play.step", Json{ { "ticks", 2 } }).has_value());
			REQUIRE(editor.SetSelection({ *platform }, SceneTarget::Play).has_value());
			// The play scene's view sits under its notice line: rendered at the region it measures now, as a host renders it, and
			// halfway between its last two ticks, as a host extracting between ticks shows it.
			test.FrameOk();
			test.Viewport.Image.Width = static_cast<uint32_t>(test.Viewport.Rectangle.Size.x);
			test.Viewport.Image.Height = static_cast<uint32_t>(test.Viewport.Rectangle.Size.y);
			test.RefreshCamera();
			test.Viewport.Image.Alpha = 0.5f;
			test.FrameOk();

			// The cell under the image's middle at the pose the image shows, and the other one the current pose puts there.
			const EditorViewportImage& image = test.Viewport.Image;
			const ConstEntity entity = editor.GetPlay().GetSession()->GetScene().FindEntityByID(*platform);
			REQUIRE(entity.IsValid());
			const ImVec2 pointer(test.Viewport.Rectangle.Min.x + test.Viewport.Rectangle.Size.x * 0.5f, test.Viewport.Rectangle.Min.y + test.Viewport.Rectangle.Size.y * 0.5f);
			const std::optional<EditorViewportPixel> pixel = ToViewportPixel(test.Viewport.Rectangle, glm::vec2(pointer.x, pointer.y));
			REQUIRE(pixel.has_value());
			const RenderOutputMapping mapping = ComputeRenderOutputMapping(image.Width, image.Height, RenderOutputSettings{});
			const auto ray = ComputeOutputPixelRayInterval(image.Camera, mapping, pixel->X, pixel->Y);
			REQUIRE(ray.has_value());
			REQUIRE(ray->has_value());
			const std::optional<glm::ivec2> shown = IntersectTilemapCell((*ray)->Ray, ComputeRenderedWorldMatrix(entity, image.Alpha), glm::vec2(1.0f));
			const std::optional<glm::ivec2> current = IntersectTilemapCell((*ray)->Ray, ComputeRenderedWorldMatrix(entity, 1.0f), glm::vec2(1.0f));
			REQUIRE(shown.has_value());
			REQUIRE(current.has_value());
			REQUIRE(*shown != *current);

			// The picker's first entry arms the brush, then a left click at the image's middle paints one cell.
			test.FrameOk([]()
			{
				ImGuiWindow* window = ScenePanelHarness::SceneWindow();
				REQUIRE(window != nullptr);
				ImGui::SetWindowFocus(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
				ImGui::SetNavID(window->GetID("##TilemapBrushTile"), ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
				ImGui::SetNavCursorVisible(true);
			});
			for (const ImGuiKey key : { ImGuiKey_Space, ImGuiKey_DownArrow, ImGuiKey_Space })
			{
				ImGui::GetIO().AddKeyEvent(key, true);
				test.FrameOk();
				ImGui::GetIO().AddKeyEvent(key, false);
				test.FrameOk();
			}
			const size_t history = editor.GetHistory().GetUndoCount();
			test.MoveTo(pointer);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			test.GetContext().Actions.Pump();
			test.FrameOk();
			// The click painted, as one undo step, and picked nothing.
			CHECK(test.Viewport.Clicks.empty());
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);

			const Result<std::string> text = editor.GetVfs().ReadText(VfsPath::Create("project", "Assets/Tilemaps/Moving.tilemap").value_or(VfsPath()));
			REQUIRE(text.has_value());
			TilemapLoadReport report;
			const Result<TilemapData> painted = TilemapFromText(*text, editor.GetTypeRegistry(), report, true);
			REQUIRE_MESSAGE(painted.has_value(), painted.error().ToString());
			CHECK(GetTilemapCell(*painted, *shown) == 1);
			CHECK(GetTilemapCell(*painted, *current) == 0);
		}
	}

}
