#include "TestsPCH.h"

#include "Engine/Platform/Events.h"

namespace Engine {

	TEST_SUITE("Platform")
	{
		TEST_CASE("Events: Overloaded dispatches each alternative to its own handler")
		{
			const std::array<Event, 3> events = {
				Event(KeyEvent{ .KeyCode = Key::Space, .Action = ButtonAction::Pressed }),
				Event(MouseScrollEvent{ .Offset = glm::vec2(0.0f, 2.0f) }),
				Event(WindowCloseEvent{}),
			};

			const auto describeKey = [](const KeyEvent& key)
			{
				return std::format("key {}", std::to_underlying(key.KeyCode));
			};
			const auto describeScroll = [](const MouseScrollEvent& scroll)
			{
				return std::format("scroll {}", scroll.Offset.y);
			};
			const auto describeOther = [](const auto& other)
			{
				return std::string(GetEventName(Event(other)));
			};
			const Overloaded describe{ describeKey, describeScroll, describeOther };

			std::vector<std::string> seen;
			for (const Event& event : events)
				seen.push_back(std::visit(describe, event));

			REQUIRE(seen.size() == 3);
			CHECK(seen[0] == "key 32");
			CHECK(seen[1] == "scroll 2");
			CHECK(seen[2] == "WindowCloseEvent");
		}

		TEST_CASE("Events: the Handled flag is reachable through the variant")
		{
			Event event = MouseButtonEvent{ .Button = MouseButton::Right, .Action = ButtonAction::Released };
			CHECK_FALSE(IsHandled(event));

			SetHandled(event);
			CHECK(IsHandled(event));
			CHECK(std::get<MouseButtonEvent>(event).Handled);
			CHECK(std::get<MouseButtonEvent>(event).Button == MouseButton::Right);

			SetHandled(event, false);
			CHECK_FALSE(IsHandled(event));
		}

		TEST_CASE("Events: every alternative starts unhandled and has its own name")
		{
			const std::array<Event, std::variant_size_v<Event>> events = {
				Event(WindowCloseEvent{}),
				Event(WindowResizeEvent{}),
				Event(WindowFocusEvent{}),
				Event(KeyEvent{}),
				Event(CharEvent{}),
				Event(MouseButtonEvent{}),
				Event(MouseMoveEvent{}),
				Event(MouseScrollEvent{}),
				Event(FileDropEvent{}),
				Event(GamepadEvent{}),
			};

			std::vector<std::string_view> names;
			for (size_t index = 0; index < events.size(); ++index)
			{
				CHECK(events[index].index() == index);
				CHECK_FALSE(IsHandled(events[index]));
				names.push_back(GetEventName(events[index]));
			}

			CHECK(names.front() == "WindowCloseEvent");
			CHECK(names.back() == "GamepadEvent");
			std::ranges::sort(names);
			CHECK(std::ranges::adjacent_find(names) == names.end());
		}

		TEST_CASE("Events: default values describe a neutral event")
		{
			const KeyEvent key;
			CHECK(key.KeyCode == Key::None);
			CHECK(key.Modifiers == KeyModifiers::None);

			const MouseMoveEvent move;
			CHECK(move.Position == glm::vec2(0.0f));

			const GamepadEvent gamepad;
			CHECK(gamepad.Gamepad == 0);
			CHECK(gamepad.Value == 0.0f);
			CHECK_FALSE(gamepad.Pressed);

			const WindowResizeEvent resize;
			CHECK(resize.Width == 0);
			CHECK(resize.FramebufferHeight == 0);
		}
	}

}
