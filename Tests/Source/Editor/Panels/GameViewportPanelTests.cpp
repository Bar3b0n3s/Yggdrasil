#include "TestsPCH.h"
#include "Editor/Panels/GameViewportPanel.h"

#include "Editor/PanelInteractionFixture.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Platform/Window.h"
#include "Support/EditorTestFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"

#include <imgui_internal.h>

#include <functional>
#include <string>

namespace Engine {

	namespace {

		// A displayed game image without a GPU; records the capture state the panel establishes, as the host does.
		class GamePanelViewport final : public EditorViewportHost
		{
		public:
			[[nodiscard]] EditorViewportImage GetImage(ViewportView view) const override { return view == ViewportView::Game ? Image : EditorViewportImage{}; }
			void SetRectangle(ViewportView view, const EditorViewportRect& rectangle) override
			{
				if (view == ViewportView::Game)
					Rectangle = rectangle;
			}
			[[nodiscard]] Status RequestPick(const EditorViewportClick&) override { return MakeError(ErrorCode::Unsupported, "the game view never picks"); }
			[[nodiscard]] Status ConfigureSceneSnapshot(RenderSnapshot&, const EditorViewportOptions&, std::span<const UUID>) override { return MakeError(ErrorCode::Unsupported, "no renderer in panel tests"); }
			void SetGameInputFocused(bool focused) override { Focused = focused; }
			[[nodiscard]] bool IsGameInputFocused() const override { return Focused; }

			EditorViewportImage Image{};
			EditorViewportRect Rectangle{};
			bool Focused = false;
		};

		// The real GameViewportPanel over an editor with a (headless) window, whose cursor mode the panel reads.
		class GamePanelHarness
		{
		public:
			explicit GamePanelHarness(std::string_view label)
				: m_Directory(label)
			{
				auto engine = EngineContext::Create({ .WorkerCount = 0, .Window = WindowSpecification{ .Title = "Game panel test", .Width = 640, .Height = 360 }, .RegisterTypes = &RegisterEditorMethodTypes });
				REQUIRE_MESSAGE(engine.has_value(), (engine ? std::string() : engine.error().ToString()));
				m_Engine = std::move(*engine);
				auto editor = EditorContext::Create(*m_Engine, { .IdGeneratorState = Test::EditorTestIdState });
				REQUIRE(editor.has_value());
				m_Editor = std::move(*editor);
				const auto created = ProjectManager::CreateProject({ .Directory = m_Directory / "Project", .Name = "Game", .Template = ProjectTemplate::Empty, .TemplatesDirectory = Test::GetRepositoryRoot() / "Resources/Templates/Projects" }, m_Engine->GetTypeRegistry());
				REQUIRE(created.has_value());
				auto project = ProjectManager::OpenProject(created->ProjectFile, {}, m_Engine->GetTypeRegistry());
				REQUIRE(project.has_value());
				REQUIRE(m_Editor->OpenProject(std::move(*project)).has_value());
				m_Editor->SetScene(m_Editor->CreateScene("Game"), std::nullopt);
				m_Client = CreateScope<Test::AutomationTestClient>(*m_Editor);
				m_Actions = CreateScope<EditorActions>(*m_Editor, m_Client->GetServer());
				m_Controls = CreateScope<EditorAutomationControls>(*m_Editor, m_Client->GetServer());
				m_Edits = CreateScope<ReflectedEditController>(*m_Editor);
				m_Thumbnails = CreateScope<ThumbnailCache>(*m_Editor, [](const ThumbnailRequest& request)
				{
					return CreateImage(request.Size, request.Size, nvrhi::Format::RGBA8_UNORM);
				});
				m_Gizmos = CreateScope<GizmoController>(*m_Editor);
				m_Context = CreateScope<EditorPanelContext>(EditorPanelContext{ *m_Editor, m_Client->GetServer(), *m_Actions, *m_Controls, Viewport, *m_Edits, *m_Thumbnails, *m_Gizmos });
				m_Ui = CreateScope<Test::PanelInteractionUi>(false);
				Utils::ApplyEditorStyle();
				FrameOk();
				FrameOk();
				const glm::uvec2 size(static_cast<uint32_t>(Viewport.Rectangle.Size.x), static_cast<uint32_t>(Viewport.Rectangle.Size.y));
				REQUIRE(size.x > 0);
				Viewport.Image = { .Texture = static_cast<ImTextureID>(9), .Width = size.x, .Height = size.y, .Generation = 1, .FrameIndex = 1 };
				FrameOk();
			}
			~GamePanelHarness()
			{
				if (m_Editor && m_Editor->GetPlay().IsPlaying())
					CHECK(m_Editor->GetPlay().Stop().has_value());
				m_Ui.reset();
				m_Context.reset();
				m_Gizmos.reset();
				m_Thumbnails.reset();
				m_Edits.reset();
				m_Controls.reset();
				m_Actions.reset();
				m_Client.reset();
				m_Editor.reset();
				m_Engine.reset();
			}
			GamePanelHarness(const GamePanelHarness&) = delete;
			GamePanelHarness& operator=(const GamePanelHarness&) = delete;

			void FrameOk(const std::function<void()>& before = {})
			{
				const Status status = m_Ui->Frame([this, &before]() -> Status
				{
					if (before)
						before();
					ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
					ImGui::SetNextWindowSize(ImVec2(900.0f, 640.0f), ImGuiCond_FirstUseEver);
					ImGui::Begin(Utils::EditorWindowTitle(EditorPanel::GameViewport), nullptr, GameViewportPanel::GetWindowFlags());
					const Status drawn = Panel.Draw(*m_Context);
					ImGui::End();
					ImGui::SetNextWindowPos(ImVec2(0.0f, 700.0f), ImGuiCond_FirstUseEver);
					ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f), ImGuiCond_FirstUseEver);
					ImGui::Begin("Other panel");
					ImGui::TextUnformatted("Holds focus");
					ImGui::End();
					return drawn;
				});
				REQUIRE_MESSAGE(status.has_value(), (status ? std::string() : status.error().ToString()));
			}
			void MoveTo(ImVec2 position)
			{
				ImGui::GetIO().AddMousePosEvent(position.x, position.y);
				FrameOk();
			}
			void Button(ImGuiMouseButton button, bool down)
			{
				ImGui::GetIO().AddMouseButtonEvent(button, down);
				FrameOk();
			}
			void Press(ImGuiKey key)
			{
				ImGui::GetIO().AddKeyEvent(key, true);
				FrameOk();
				ImGui::GetIO().AddKeyEvent(key, false);
				FrameOk();
			}
			void Play()
			{
				const auto started = m_Client->Call("play.start", Json{ { "paused", true } });
				REQUIRE_MESSAGE(started.has_value(), (started ? std::string() : started.error().ToString()));
			}
			void Focus(const char* window)
			{
				FrameOk([window]()
				{
					ImGui::SetWindowFocus(window);
				});
			}
			[[nodiscard]] ImVec2 Center() const { return ImVec2(Viewport.Rectangle.Min.x + Viewport.Rectangle.Size.x * 0.5f, Viewport.Rectangle.Min.y + Viewport.Rectangle.Size.y * 0.5f); }
			[[nodiscard]] EditorContext& GetEditor() { return *m_Editor; }
			[[nodiscard]] Window& GetWindow() { return *m_Engine->GetWindow(); }

			GamePanelViewport Viewport{};
			GameViewportPanel Panel{};
		private:
			Test::TempDirectory m_Directory;
			Scope<EngineContext> m_Engine{};
			Scope<EditorContext> m_Editor{};
			Scope<Test::AutomationTestClient> m_Client{};
			Scope<EditorActions> m_Actions{};
			Scope<EditorAutomationControls> m_Controls{};
			Scope<ReflectedEditController> m_Edits{};
			Scope<ThumbnailCache> m_Thumbnails{};
			Scope<GizmoController> m_Gizmos{};
			Scope<EditorPanelContext> m_Context{};
			Scope<Test::PanelInteractionUi> m_Ui{};
		};

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("GameViewportPanel: a click captures game input for a held press and keeps it off the image until released")
		{
			GamePanelHarness test("GameCapture");
			test.Play();
			const char* title = Utils::EditorWindowTitle(EditorPanel::GameViewport);
			// Focus and hover alone never send game input: the view's hint asks for a click.
			test.Focus(title);
			test.MoveTo(test.Center());
			test.FrameOk();
			CHECK_FALSE(test.Viewport.Focused);
			test.Button(ImGuiMouseButton_Left, true);
			CHECK(test.Viewport.Focused);
			// A held press owns the image item: no frame of it drops the capture (and with it the held game input).
			for (int frame = 1; frame <= 30; ++frame)
			{
				CAPTURE(frame);
				test.MoveTo(ImVec2(test.Center().x + static_cast<float>(frame), test.Center().y));
				CHECK(test.Viewport.Focused);
			}
			test.Button(ImGuiMouseButton_Left, false);
			CHECK(test.Viewport.Focused);
			// Leaving the image, even the window, keeps the capture while the window keeps focus.
			test.MoveTo(ImVec2(test.Viewport.Rectangle.Min.x - 4.0f, test.Viewport.Rectangle.Min.y - 4.0f));
			CHECK(test.Viewport.Focused);
			test.MoveTo(ImVec2(990.0f, 1300.0f));
			CHECK(test.Viewport.Focused);
			// Escape releases it; a new click captures again; focusing another panel releases it.
			test.Press(ImGuiKey_Escape);
			CHECK_FALSE(test.Viewport.Focused);
			test.MoveTo(test.Center());
			test.Button(ImGuiMouseButton_Right, true);
			CHECK(test.Viewport.Focused);
			test.Button(ImGuiMouseButton_Right, false);
			test.Focus("Other panel");
			CHECK_FALSE(test.Viewport.Focused);
			// A host release (the native window blurred) needs a new click; nothing re-captures by itself.
			test.MoveTo(test.Center());
			test.Button(ImGuiMouseButton_Left, true);
			test.Button(ImGuiMouseButton_Left, false);
			REQUIRE(test.Viewport.Focused);
			test.Viewport.SetGameInputFocused(false);
			test.FrameOk();
			test.FrameOk();
			CHECK_FALSE(test.Viewport.Focused);
			// Without a play session the image never captures.
			REQUIRE(test.GetEditor().GetPlay().Stop().has_value());
			test.Button(ImGuiMouseButton_Left, true);
			test.Button(ImGuiMouseButton_Left, false);
			CHECK_FALSE(test.Viewport.Focused);
		}

		TEST_CASE("GameViewportPanel: a locked cursor keeps game input captured when its position leaves the image")
		{
			GamePanelHarness test("GameLockedCursor");
			test.Play();
			test.MoveTo(test.Center());
			test.Button(ImGuiMouseButton_Left, true);
			test.Button(ImGuiMouseButton_Left, false);
			REQUIRE(test.Viewport.Focused);
			// Mouselook: GLFW_CURSOR_DISABLED reports unbounded virtual positions that leave the image and the window.
			test.GetWindow().SetCursorMode(CursorMode::Locked);
			for (int frame = 1; frame <= 10; ++frame)
				test.MoveTo(ImVec2(test.Center().x - 120.0f * static_cast<float>(frame), test.Center().y + 90.0f * static_cast<float>(frame)));
			CHECK(test.Viewport.Focused);
			test.Focus("Other panel");
			CHECK(test.Viewport.Focused);
			// Once the game unlocks the cursor, the window's lost focus ends the capture.
			test.GetWindow().SetCursorMode(CursorMode::Normal);
			test.FrameOk();
			CHECK_FALSE(test.Viewport.Focused);
		}

		TEST_CASE("GameViewportPanel: a fixed resolution letterboxes the published rectangle and its bars never capture")
		{
			GamePanelHarness test("GameLetterbox");
			test.Play();
			REQUIRE(test.GetEditor().GetViewportState().SetGameResolution({ 1920, 1080 }).has_value());
			test.Viewport.Image.Width = 1920;
			test.Viewport.Image.Height = 1080;
			test.FrameOk();
			const EditorViewportRect& rectangle = test.Viewport.Rectangle;
			CHECK(rectangle.ImageSize == glm::uvec2(1920, 1080));
			CHECK(rectangle.Size.x / rectangle.Size.y == doctest::Approx(16.0 / 9.0).epsilon(1e-4));
			const ImGuiWindow* window = ImGui::FindWindowByName(Utils::EditorWindowTitle(EditorPanel::GameViewport));
			REQUIRE(window != nullptr);
			// The content region is taller than 16:9, so the bars are above and below the centred image.
			REQUIRE(rectangle.Min.y + rectangle.Size.y < window->InnerRect.Max.y - 4.0f);
			CHECK(rectangle.Min.x == doctest::Approx(window->InnerRect.Min.x + ImGui::GetStyle().WindowPadding.x).epsilon(1e-3));
			test.MoveTo(ImVec2(rectangle.Min.x + rectangle.Size.x * 0.5f, rectangle.Min.y + rectangle.Size.y + 3.0f));
			test.Button(ImGuiMouseButton_Left, true);
			test.Button(ImGuiMouseButton_Left, false);
			CHECK_FALSE(test.Viewport.Focused);
			test.MoveTo(test.Center());
			test.Button(ImGuiMouseButton_Left, true);
			test.Button(ImGuiMouseButton_Left, false);
			CHECK(test.Viewport.Focused);
		}
	}

}
