#include "TestsPCH.h"

#include "Engine/Platform/Window.h"

#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Input/InputState.h"
#include "Support/TestOptions.h"
#include "Support/WindowedChild.h"

namespace Engine {

	static bool IsInputEvent(const Event& event)
	{
		return std::holds_alternative<KeyEvent>(event) || std::holds_alternative<MouseMoveEvent>(event)
			|| std::holds_alternative<GamepadEvent>(event);
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("Window: null platform creates a window and accepts injected events" * doctest::skip(true))
		{
			// The Tests binary runs headless: GLFW's null platform (Architecture §4.1, §15.2).
			REQUIRE(GlfwLibrary::GetPlatform() == GlfwPlatform::Null);

			const uint32_t windowsBefore = GlfwLibrary::GetWindowCount();
			{
				Result<Window> created = Window::Create({ .Title = "Null window", .Width = 320, .Height = 200 });
				REQUIRE(created.has_value());
				Window& window = *created;
				CHECK(GlfwLibrary::GetWindowCount() == windowsBefore + 1);
				CHECK(window.GetWidth() == 320);
				CHECK(window.GetHeight() == 200);
				CHECK(window.GetFramebufferWidth() == 320);
				CHECK(window.GetFramebufferHeight() == 200);
				CHECK(window.GetContentScale() == glm::vec2(1.0f));
				CHECK(window.GetTitle() == "Null window");
				CHECK(window.GetNativeHandle() != nullptr);
				CHECK_FALSE(window.IsMinimized());

				std::vector<Event> delivered;
				window.SetEventCallback([&delivered](Event& event)
				{
					if (IsInputEvent(event))
						delivered.push_back(event);
				});

				window.InjectEvent(KeyEvent{ .KeyCode = Key::W, .Action = ButtonAction::Pressed });
				window.InjectEvent(MouseMoveEvent{ .Position = glm::vec2(10.0f, 20.0f) });
				CHECK(delivered.empty()); // queued until the next poll

				window.PollEvents();
				REQUIRE(delivered.size() == 2);
				REQUIRE(std::holds_alternative<KeyEvent>(delivered[0]));
				CHECK(std::get<KeyEvent>(delivered[0]).KeyCode == Key::W);
				REQUIRE(std::holds_alternative<MouseMoveEvent>(delivered[1]));
				CHECK(std::get<MouseMoveEvent>(delivered[1]).Position == glm::vec2(10.0f, 20.0f));

				// The delivered events take the frame loop's path into InputState like real ones.
				InputState input;
				for (const Event& event : delivered)
					input.Inject(event);
				input.LatchFrame();
				CHECK(input.IsKeyDown(InputPhase::Frame, Key::W));
				CHECK(input.GetMousePosition(InputPhase::Frame) == glm::vec2(10.0f, 20.0f));

				// Delivered once: the queue is empty afterwards.
				window.PollEvents();
				CHECK(delivered.size() == 2);
			}
			CHECK(GlfwLibrary::GetWindowCount() == windowsBefore);
		}

		TEST_CASE("Window: injected window events are delivered without changing the window's state" * doctest::skip(true))
		{
			Result<Window> created = Window::Create({ .Title = "Injected", .Width = 64, .Height = 48 });
			REQUIRE(created.has_value());
			Window& window = *created;
			std::vector<Event> delivered;
			window.SetEventCallback([&delivered](Event& event)
			{
				if (std::holds_alternative<WindowResizeEvent>(event) || std::holds_alternative<WindowFocusEvent>(event))
					delivered.push_back(event);
			});
			const bool focusedBefore = window.IsFocused();

			window.InjectEvent(WindowResizeEvent{ .Width = 10, .Height = 20, .FramebufferWidth = 0, .FramebufferHeight = 0 });
			window.InjectEvent(WindowFocusEvent{ .Focused = !focusedBefore });
			window.PollEvents();

			REQUIRE(delivered.size() == 2);
			REQUIRE(std::holds_alternative<WindowResizeEvent>(delivered[0]));
			CHECK(std::get<WindowResizeEvent>(delivered[0]).Width == 10);
			CHECK(std::holds_alternative<WindowFocusEvent>(delivered[1]));
			// The accessors still follow the OS: the window was neither resized, minimized (0x0) nor refocused.
			CHECK(window.GetWidth() == 64);
			CHECK(window.GetHeight() == 48);
			CHECK(window.GetFramebufferWidth() == 64);
			CHECK_FALSE(window.IsMinimized());
			CHECK(window.IsFocused() == focusedBefore);
		}

		TEST_CASE("Window: minimizing and restoring a null-platform window updates IsMinimized" * doctest::skip(true))
		{
			Result<Window> created = Window::Create({ .Title = "Minimize", .Width = 64, .Height = 64 });
			REQUIRE(created.has_value());
			Window& window = *created;

			window.Minimize();
			window.PollEvents();
			CHECK(window.IsMinimized());

			window.Restore();
			window.PollEvents();
			CHECK_FALSE(window.IsMinimized());

			// WaitEventsTimeout returns at the latest after its timeout and delivers queued injected events at once.
			std::vector<Event> delivered;
			window.SetEventCallback([&delivered](Event& event)
			{
				if (IsInputEvent(event))
					delivered.push_back(event);
			});
			window.InjectEvent(KeyEvent{ .KeyCode = Key::Escape, .Action = ButtonAction::Pressed });
			window.WaitEventsTimeout(0.001);
			CHECK(delivered.size() == 1);
		}

		TEST_CASE("Window: the title, the cursor mode and moving keep their values" * doctest::skip(true))
		{
			Result<Window> created = Window::Create({ .Title = "First", .Width = 64, .Height = 48 });
			REQUIRE(created.has_value());
			created->SetTitle("Second");
			created->SetCursorMode(CursorMode::Locked);

			Window moved = std::move(*created);
			CHECK(moved.GetTitle() == "Second");
			CHECK(moved.GetCursorMode() == CursorMode::Locked);
			CHECK(moved.GetWidth() == 64);

			// GLFW's callbacks still reach the moved window.
			int deliveredCount = 0;
			moved.SetEventCallback([&deliveredCount](Event& event)
			{
				if (IsInputEvent(event))
					++deliveredCount;
			});
			moved.InjectEvent(KeyEvent{ .KeyCode = Key::Q });
			moved.PollEvents();
			CHECK(deliveredCount == 1);

			CHECK(CursorModeToString(CursorMode::Normal) == "Normal");
			CHECK(CursorModeToString(CursorMode::Locked) == "Locked");
		}

		TEST_CASE("Window: a zero size is InvalidArgument" * doctest::skip(true))
		{
			const Result<Window> zeroWidth = Window::Create({ .Title = "Zero", .Width = 0, .Height = 100 });
			REQUIRE_FALSE(zeroWidth.has_value());
			CHECK(zeroWidth.error().GetCode() == ErrorCode::InvalidArgument);

			const Result<Window> zeroHeight = Window::Create({ .Title = "Zero", .Width = 100, .Height = 0 });
			REQUIRE_FALSE(zeroHeight.has_value());
			CHECK(zeroHeight.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// Runs only in a windowed child process (Support/WindowedChild.h): a native window on the host's platform.
		TEST_CASE("Window: a native window opens with its requested size"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			REQUIRE(GlfwLibrary::GetMode() == WindowMode::Windowed);
			CHECK(GlfwLibrary::GetPlatform() != GlfwPlatform::Null);

			Result<Window> created = Window::Create({ .Title = "Native window", .Width = 400, .Height = 300 });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			CHECK(created->GetWidth() == 400);
			CHECK(created->GetHeight() == 300);
			// Retina and scaled displays have more framebuffer pixels than screen units, never fewer.
			CHECK(created->GetFramebufferWidth() >= 400);
			CHECK(created->GetContentScale().x >= 1.0f);
			created->PollEvents();
		}

		TEST_CASE("Window: a native window opens in a windowed child process" * doctest::skip(true))
		{
			ENGINE_CHECK_WINDOWED_CHILD("Window: a native window opens with its requested size");
		}
	}

}
