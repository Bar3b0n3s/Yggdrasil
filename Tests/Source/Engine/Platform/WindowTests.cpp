#include "TestsPCH.h"

#include "Engine/Platform/Window.h"

#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Input/InputState.h"
#include "Support/DeathTest.h"
#include "Support/TestOptions.h"
#include "Support/WindowedChild.h"

namespace Engine {

	static bool IsInputEvent(const Event& event)
	{
		return std::holds_alternative<KeyEvent>(event) || std::holds_alternative<MouseMoveEvent>(event)
			|| std::holds_alternative<GamepadEvent>(event);
	}

	// GLFW forbids polling from inside its own callbacks, and a nested delivery would reorder the queue.
	ENGINE_DEATH_TEST("Platform/WindowPollFromCallback")
	{
		Result<Window> created = Window::Create({ .Title = "Nested", .Width = 64, .Height = 48 });
		if (!created.has_value())
			return;
		Window& window = *created;
		window.SetEventCallback([&window](Event& /*event*/)
		{
			window.PollEvents();
		});
		window.InjectEvent(WindowCloseEvent{});
		window.PollEvents();
	}

	// SetSize's precondition: a window never has a zero dimension (minimizing is Minimize).
	ENGINE_DEATH_TEST("Platform/WindowSetSizeZero")
	{
		Result<Window> created = Window::Create({ .Title = "ZeroSize", .Width = 64, .Height = 48 });
		if (!created.has_value())
			return;
		created->SetSize(0, 48);
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("Window: null platform creates a window and accepts injected events")
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

		TEST_CASE("Window: injected window events are delivered without changing the window's state")
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

		TEST_CASE("Window: minimizing and restoring a null-platform window updates IsMinimized")
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

		TEST_CASE("Window: the title, the cursor mode and moving keep their values")
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

		TEST_CASE("Window: a zero size is InvalidArgument")
		{
			const Result<Window> zeroWidth = Window::Create({ .Title = "Zero", .Width = 0, .Height = 100 });
			REQUIRE_FALSE(zeroWidth.has_value());
			CHECK(zeroWidth.error().GetCode() == ErrorCode::InvalidArgument);

			const Result<Window> zeroHeight = Window::Create({ .Title = "Zero", .Width = 100, .Height = 0 });
			REQUIRE_FALSE(zeroHeight.has_value());
			CHECK(zeroHeight.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Window: the state already follows OS events when the callback sees them")
		{
			Result<Window> created = Window::Create({ .Title = "State", .Width = 64, .Height = 48 });
			REQUIRE(created.has_value());
			Window& window = *created;

			// The injected key is a probe queued after the OS events of the minimize.
			std::vector<bool> minimizedAtDelivery;
			window.SetEventCallback([&window, &minimizedAtDelivery](Event& event)
			{
				if (std::holds_alternative<KeyEvent>(event))
					minimizedAtDelivery.push_back(window.IsMinimized());
			});
			window.Minimize();
			window.InjectEvent(KeyEvent{ .KeyCode = Key::M });
			window.PollEvents();
			window.Restore();
			window.InjectEvent(KeyEvent{ .KeyCode = Key::R });
			window.PollEvents();

			REQUIRE(minimizedAtDelivery.size() == 2);
			CHECK(minimizedAtDelivery[0]);
			CHECK_FALSE(minimizedAtDelivery[1]);
		}

		TEST_CASE("Window: a callback set during delivery receives the events after the current one")
		{
			Result<Window> created = Window::Create({ .Title = "Callbacks", .Width = 64, .Height = 48 });
			REQUIRE(created.has_value());
			Window& window = *created;

			std::vector<std::string> received;
			window.SetEventCallback([&window, &received](Event& event)
			{
				if (!std::holds_alternative<KeyEvent>(event))
					return;
				received.push_back(std::format("first {}", KeyToString(std::get<KeyEvent>(event).KeyCode)));
				window.SetEventCallback([&received](Event& later)
				{
					if (std::holds_alternative<KeyEvent>(later))
						received.push_back(std::format("second {}", KeyToString(std::get<KeyEvent>(later).KeyCode)));
				});
			});
			window.InjectEvent(KeyEvent{ .KeyCode = Key::A });
			window.InjectEvent(KeyEvent{ .KeyCode = Key::B });
			window.InjectEvent(KeyEvent{ .KeyCode = Key::C });
			window.PollEvents();

			const std::vector<std::string> expected = { "first A", "second B", "second C" };
			CHECK(received == expected);
		}

		TEST_CASE("Window: events injected during delivery arrive with the next poll")
		{
			Result<Window> created = Window::Create({ .Title = "Reinjected", .Width = 64, .Height = 48 });
			REQUIRE(created.has_value());
			Window& window = *created;

			std::vector<Key> received;
			window.SetEventCallback([&window, &received](Event& event)
			{
				if (!std::holds_alternative<KeyEvent>(event))
					return;
				const Key key = std::get<KeyEvent>(event).KeyCode;
				received.push_back(key);
				if (key == Key::D1)
					window.InjectEvent(KeyEvent{ .KeyCode = Key::D2 });
			});
			window.InjectEvent(KeyEvent{ .KeyCode = Key::D1 });
			window.PollEvents();
			REQUIRE(received.size() == 1);
			CHECK(received[0] == Key::D1);

			window.PollEvents();
			REQUIRE(received.size() == 2);
			CHECK(received[1] == Key::D2);
		}

		TEST_CASE("Window: events queued without a callback are dropped")
		{
			Result<Window> created = Window::Create({ .Title = "Dropped", .Width = 64, .Height = 48 });
			REQUIRE(created.has_value());
			Window& window = *created;

			window.InjectEvent(KeyEvent{ .KeyCode = Key::Z });
			window.PollEvents();

			int deliveredCount = 0;
			window.SetEventCallback([&deliveredCount](Event& event)
			{
				if (IsInputEvent(event))
					++deliveredCount;
			});
			window.PollEvents();
			CHECK(deliveredCount == 0);

			// An empty callback removes the callback again.
			window.SetEventCallback({});
			window.InjectEvent(KeyEvent{ .KeyCode = Key::Z });
			window.PollEvents();
			CHECK(deliveredCount == 0);
		}

		TEST_CASE("Window: polling from inside the event callback asserts")
		{
			ENGINE_CHECK_DEATH("Platform/WindowPollFromCallback", "Window::PollEvents called from inside the event callback");
		}

		TEST_CASE("Window: SetSize resizes a null-platform window and delivers one WindowResizeEvent")
		{
			Result<Window> created = Window::Create({ .Title = "Resize", .Width = 64, .Height = 48 });
			REQUIRE(created.has_value());
			Window& window = *created;
			std::vector<WindowResizeEvent> resizes;
			window.SetEventCallback([&resizes](Event& event)
			{
				if (const WindowResizeEvent* resize = std::get_if<WindowResizeEvent>(&event))
					resizes.push_back(*resize);
			});

			window.SetSize(100, 80);
			window.PollEvents();
			CHECK(window.GetWidth() == 100);
			CHECK(window.GetHeight() == 80);
			// The null platform's content scale is 1, so the framebuffer follows the window size exactly.
			CHECK(window.GetFramebufferWidth() == 100);
			CHECK(window.GetFramebufferHeight() == 80);
			// The window and framebuffer changes arrive as one event with both sizes.
			REQUIRE(resizes.size() == 1);
			CHECK(resizes[0].Width == 100);
			CHECK(resizes[0].Height == 80);
			CHECK(resizes[0].FramebufferWidth == 100);
			CHECK(resizes[0].FramebufferHeight == 80);

			// The current size again changes nothing and delivers nothing.
			window.SetSize(100, 80);
			window.PollEvents();
			CHECK(resizes.size() == 1);
		}

		TEST_CASE("Window: SetSize with a zero dimension asserts")
		{
			ENGINE_CHECK_DEATH("Platform/WindowSetSizeZero", "Window::SetSize needs a non-zero size");
		}

		TEST_CASE("Window: a full-screen window takes the monitor's current video mode")
		{
			// GLFW's null platform has one monitor with a 1920x1080 mode.
			Result<Window> created = Window::Create({ .Title = "Full screen", .Width = 320, .Height = 200, .Fullscreen = true });
			REQUIRE(created.has_value());
			CHECK(created->GetWidth() == 1920);
			CHECK(created->GetHeight() == 1080);
			CHECK_FALSE(created->IsMinimized());
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

		TEST_CASE("Window: a native window opens in a windowed child process")
		{
			ENGINE_CHECK_WINDOWED_CHILD("Window: a native window opens with its requested size");
		}

		// Runs only in a windowed child process. Iconifying is asynchronous on X11 (the window manager reports it through
		// WM_STATE, WindowedChild.h) and animated on macOS, so the target waits for each change, a bounded number of times.
		TEST_CASE("Window: a native window minimizes and restores"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			REQUIRE(GlfwLibrary::GetMode() == WindowMode::Windowed);

			Result<Window> created = Window::Create({ .Title = "Native minimize", .Width = 320, .Height = 240 });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			Window& window = *created;
			window.PollEvents();
			REQUIRE_FALSE(window.IsMinimized());

			window.Minimize();
			CHECK(Test::WaitUntilMinimized(window, true));
			window.Restore();
			CHECK(Test::WaitUntilMinimized(window, false));
			CHECK(window.GetFramebufferWidth() > 0);
			CHECK(window.GetFramebufferHeight() > 0);
		}

		TEST_CASE("Window: a native window minimizes and restores in a windowed child process")
		{
			ENGINE_CHECK_WINDOWED_CHILD("Window: a native window minimizes and restores");
		}
	}

}
