#include "EnginePCH.h"
#include "Engine/Platform/Window.h"

#include "Engine/Core/Assert.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Input/InputState.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine {

	namespace Utils {

		static uint32_t ToSize(int value)
		{
			return static_cast<uint32_t>(std::max(value, 0));
		}

		static std::optional<ButtonAction> ToButtonAction(int action)
		{
			switch (action)
			{
				case GLFW_RELEASE: return ButtonAction::Released;
				case GLFW_PRESS:   return ButtonAction::Pressed;
				case GLFW_REPEAT:  return ButtonAction::Repeated;
				default:           return std::nullopt;
			}
		}

		static KeyModifiers ToKeyModifiers(int mods)
		{
			constexpr int KnownModifiers = GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER | GLFW_MOD_CAPS_LOCK
				| GLFW_MOD_NUM_LOCK;
			return static_cast<KeyModifiers>(static_cast<uint8_t>(mods & KnownModifiers));
		}

		static std::optional<int> ToGlfwCursorMode(CursorMode mode)
		{
			switch (mode)
			{
				case CursorMode::Normal: return GLFW_CURSOR_NORMAL;
				case CursorMode::Hidden: return GLFW_CURSOR_HIDDEN;
				case CursorMode::Locked: return GLFW_CURSOR_DISABLED;
			}
			return std::nullopt;
		}

	}

	// KeyCodes.h promises GLFW's numeric values, so the callbacks convert codes with a range check and a cast.
	static_assert(std::to_underlying(Key::Space) == GLFW_KEY_SPACE && std::to_underlying(Key::Menu) == GLFW_KEY_MENU);
	static_assert(std::to_underlying(Key::World2) == GLFW_KEY_WORLD_2 && std::to_underlying(Key::F25) == GLFW_KEY_F25);
	static_assert(std::to_underlying(Key::Keypad0) == GLFW_KEY_KP_0 && std::to_underlying(Key::KeypadEqual) == GLFW_KEY_KP_EQUAL);
	static_assert(KeyCodeCount == GLFW_KEY_LAST + 1);
	static_assert(MouseButtonCount == GLFW_MOUSE_BUTTON_LAST + 1);
	static_assert(GamepadButtonCount == GLFW_GAMEPAD_BUTTON_LAST + 1);
	static_assert(std::to_underlying(GamepadButton::DPadLeft) == GLFW_GAMEPAD_BUTTON_DPAD_LEFT);
	static_assert(GamepadAxisCount == GLFW_GAMEPAD_AXIS_LAST + 1);
	static_assert(std::to_underlying(GamepadAxis::LeftTrigger) == GLFW_GAMEPAD_AXIS_LEFT_TRIGGER);
	static_assert(std::to_underlying(KeyModifiers::Shift) == GLFW_MOD_SHIFT);
	static_assert(std::to_underlying(KeyModifiers::NumLock) == GLFW_MOD_NUM_LOCK);
	static_assert(std::to_underlying(ButtonAction::Repeated) == GLFW_REPEAT);
	static_assert(MaxGamepads <= GLFW_JOYSTICK_LAST + 1);

	// The window's GLFW handle, state and event queue. It never moves (the Window holds it in a Scope), so its address is
	// the GLFW user pointer through which the static callbacks below find it.
	//
	// The callbacks run inside glfwPollEvents and glfwWaitEventsTimeout, and also inside the GLFW calls that change the
	// window synchronously (glfwIconifyWindow, glfwRestoreWindow). They update the window's state at once and queue the
	// event; Deliver hands the queue to the event callback afterwards, so the accessors already agree with the events
	// when the callback sees them. Events the callback injects are queued for the next delivery.
	struct Window::Impl
	{
		// The previous poll of one gamepad (GLFW joystick), in engine values.
		struct PolledGamepad
		{
			bool Connected = false;
			std::array<bool, GamepadButtonCount> Buttons{};
			std::array<float, GamepadAxisCount> Axes{};
		};

		GLFWwindow* Handle = nullptr;
		std::string Title{};
		CursorMode Cursor = CursorMode::Normal;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t FramebufferWidth = 0;
		uint32_t FramebufferHeight = 0;
		bool IsIconified = false;
		bool IsFocused = false;

		std::vector<Event> Queue{};
		// The queue being delivered; kept between deliveries to reuse its storage.
		std::vector<Event> Delivering{};
		bool IsDelivering = false;
		// The index in Queue of the WindowResizeEvent the size callbacks last queued: GLFW reports a resize through two
		// callbacks (window size and framebuffer size), which update one event while it is still the last in the queue.
		std::optional<size_t> LastResizeIndex{};

		WindowEventCallback Callback{};
		// A callback set by SetEventCallback during delivery; it takes over after the event being delivered.
		std::optional<WindowEventCallback> PendingCallback{};

		std::array<PolledGamepad, MaxGamepads> Gamepads{};

		Impl(GLFWwindow* handle, std::string title)
			: Handle(handle), Title(std::move(title))
		{
			GlfwLibrary::RegisterWindow();
			glfwSetWindowUserPointer(Handle, this);

			int width = 0;
			int height = 0;
			glfwGetWindowSize(Handle, &width, &height);
			Width = Utils::ToSize(width);
			Height = Utils::ToSize(height);
			glfwGetFramebufferSize(Handle, &width, &height);
			FramebufferWidth = Utils::ToSize(width);
			FramebufferHeight = Utils::ToSize(height);
			IsIconified = glfwGetWindowAttrib(Handle, GLFW_ICONIFIED) == GLFW_TRUE;
			IsFocused = glfwGetWindowAttrib(Handle, GLFW_FOCUSED) == GLFW_TRUE;

			glfwSetWindowCloseCallback(Handle, &Impl::OnClose);
			glfwSetWindowSizeCallback(Handle, &Impl::OnWindowSize);
			glfwSetFramebufferSizeCallback(Handle, &Impl::OnFramebufferSize);
			glfwSetWindowFocusCallback(Handle, &Impl::OnFocus);
			glfwSetWindowIconifyCallback(Handle, &Impl::OnIconify);
			glfwSetKeyCallback(Handle, &Impl::OnKey);
			glfwSetCharCallback(Handle, &Impl::OnChar);
			glfwSetMouseButtonCallback(Handle, &Impl::OnMouseButton);
			glfwSetCursorPosCallback(Handle, &Impl::OnCursorPosition);
			glfwSetScrollCallback(Handle, &Impl::OnScroll);
			glfwSetDropCallback(Handle, &Impl::OnDrop);
		}

		~Impl()
		{
			ENGINE_CORE_ASSERT(GlfwLibrary::IsMainThread(), "A Window was destroyed off the main thread or after GLFW shut down");
			glfwDestroyWindow(Handle);
			GlfwLibrary::UnregisterWindow();
		}

		Impl(const Impl&) = delete;
		Impl& operator=(const Impl&) = delete;
		Impl(Impl&&) = delete;
		Impl& operator=(Impl&&) = delete;

		[[nodiscard]] bool IsMinimized() const
		{
			// A framebuffer without area cannot be rendered to either (Windows reports 0x0 while minimized).
			return IsIconified || FramebufferWidth == 0 || FramebufferHeight == 0;
		}

		void QueueResize()
		{
			const WindowResizeEvent resize{
				.Width = Width,
				.Height = Height,
				.FramebufferWidth = FramebufferWidth,
				.FramebufferHeight = FramebufferHeight,
			};
			if (LastResizeIndex.has_value() && *LastResizeIndex + 1 == Queue.size())
			{
				Queue.back() = resize;
				return;
			}
			LastResizeIndex = Queue.size();
			Queue.emplace_back(resize);
		}

		// Compares every gamepad with the previous poll and queues one GamepadEvent per change (Window.h).
		void PollGamepads()
		{
			for (uint32_t gamepad = 0; gamepad < MaxGamepads; ++gamepad)
			{
				PolledGamepad& previous = Gamepads[gamepad];
				GLFWgamepadstate state{};
				// False for a joystick that is absent or has no gamepad mapping: neither is a gamepad.
				if (glfwGetGamepadState(GLFW_JOYSTICK_1 + static_cast<int>(gamepad), &state) != GLFW_TRUE)
				{
					if (previous.Connected)
					{
						Queue.emplace_back(GamepadEvent{ .Gamepad = gamepad, .Kind = GamepadEventKind::Disconnected });
						previous = PolledGamepad{};
					}
					continue;
				}

				if (!previous.Connected)
				{
					Queue.emplace_back(GamepadEvent{ .Gamepad = gamepad, .Kind = GamepadEventKind::Connected });
					previous.Connected = true;
				}
				for (size_t index = 0; index < GamepadButtonCount; ++index)
				{
					const bool pressed = state.buttons[index] == GLFW_PRESS;
					if (pressed == previous.Buttons[index])
						continue;
					previous.Buttons[index] = pressed;
					Queue.emplace_back(GamepadEvent{
						.Gamepad = gamepad,
						.Kind = GamepadEventKind::Button,
						.Button = static_cast<GamepadButton>(index),
						.Pressed = pressed,
					});
				}
				for (size_t index = 0; index < GamepadAxisCount; ++index)
				{
					const GamepadAxis axis = static_cast<GamepadAxis>(index);
					const float value = GamepadAxisValueFromGlfw(axis, state.axes[index]);
					if (value == previous.Axes[index])
						continue;
					previous.Axes[index] = value;
					Queue.emplace_back(GamepadEvent{
						.Gamepad = gamepad,
						.Kind = GamepadEventKind::Axis,
						.Axis = axis,
						.Value = value,
					});
				}
			}
		}

		void Deliver()
		{
			Queue.swap(Delivering);
			LastResizeIndex.reset();
			IsDelivering = true;
			for (Event& event : Delivering)
			{
				if (Callback)
					Callback(event);
				if (PendingCallback.has_value())
				{
					Callback = std::move(*PendingCallback);
					PendingCallback.reset();
				}
			}
			IsDelivering = false;
			Delivering.clear();
		}

		// Every member of Window but the move operations and the destructor needs a live Window on the main thread.
		static void CheckUsable(const Impl* impl, std::string_view function)
		{
			ENGINE_CORE_ASSERT(impl != nullptr, "Window::{} called on a moved-from Window", function);
			ENGINE_CORE_ASSERT(GlfwLibrary::IsMainThread(), "Window::{} called off the main thread or after GLFW shut down", function);
		}

		static Impl& FromHandle(GLFWwindow* handle)
		{
			return *static_cast<Impl*>(glfwGetWindowUserPointer(handle));
		}

		static void OnClose(GLFWwindow* handle)
		{
			// The application decides through the WindowCloseEvent (an editor may ask to save first), so GLFW's own flag
			// stays clear.
			glfwSetWindowShouldClose(handle, GLFW_FALSE);
			FromHandle(handle).Queue.emplace_back(WindowCloseEvent{});
		}

		static void OnWindowSize(GLFWwindow* handle, int width, int height)
		{
			Impl& impl = FromHandle(handle);
			impl.Width = Utils::ToSize(width);
			impl.Height = Utils::ToSize(height);
			impl.QueueResize();
		}

		static void OnFramebufferSize(GLFWwindow* handle, int width, int height)
		{
			Impl& impl = FromHandle(handle);
			impl.FramebufferWidth = Utils::ToSize(width);
			impl.FramebufferHeight = Utils::ToSize(height);
			impl.QueueResize();
		}

		static void OnFocus(GLFWwindow* handle, int focused)
		{
			Impl& impl = FromHandle(handle);
			impl.IsFocused = focused == GLFW_TRUE;
			impl.Queue.emplace_back(WindowFocusEvent{ .Focused = impl.IsFocused });
		}

		static void OnIconify(GLFWwindow* handle, int iconified)
		{
			// No event of its own: IsMinimized reports it, and where the OS also empties the framebuffer (Windows) the
			// framebuffer callback queues a WindowResizeEvent of 0x0.
			FromHandle(handle).IsIconified = iconified == GLFW_TRUE;
		}

		static void OnKey(GLFWwindow* handle, int key, int /*scancode*/, int action, int mods)
		{
			// GLFW_KEY_UNKNOWN (-1) is a key GLFW has no code for.
			if (key <= 0 || key >= static_cast<int>(KeyCodeCount) || KeyToString(static_cast<Key>(key)).empty())
				return;
			const std::optional<ButtonAction> buttonAction = Utils::ToButtonAction(action);
			if (!buttonAction.has_value())
				return;
			FromHandle(handle).Queue.emplace_back(KeyEvent{
				.KeyCode = static_cast<Key>(key),
				.Action = *buttonAction,
				.Modifiers = Utils::ToKeyModifiers(mods),
			});
		}

		static void OnChar(GLFWwindow* handle, unsigned int codepoint)
		{
			FromHandle(handle).Queue.emplace_back(CharEvent{ .Codepoint = codepoint });
		}

		static void OnMouseButton(GLFWwindow* handle, int button, int action, int mods)
		{
			if (button < 0 || button >= static_cast<int>(MouseButtonCount))
				return;
			const std::optional<ButtonAction> buttonAction = Utils::ToButtonAction(action);
			if (!buttonAction.has_value())
				return;
			FromHandle(handle).Queue.emplace_back(MouseButtonEvent{
				.Button = static_cast<MouseButton>(button),
				.Action = *buttonAction,
				.Modifiers = Utils::ToKeyModifiers(mods),
			});
		}

		static void OnCursorPosition(GLFWwindow* handle, double x, double y)
		{
			const glm::vec2 position(static_cast<float>(x), static_cast<float>(y));
			FromHandle(handle).Queue.emplace_back(MouseMoveEvent{ .Position = position });
		}

		static void OnScroll(GLFWwindow* handle, double xOffset, double yOffset)
		{
			// GLFW's horizontal offset is positive to the left on every platform (it negates Win32's WM_MOUSEHWHEEL to match
			// Cocoa and X11); MouseScrollEvent is right-positive. Its vertical offset is already up-positive.
			const glm::vec2 offset(static_cast<float>(0.0 - xOffset), static_cast<float>(yOffset));
			FromHandle(handle).Queue.emplace_back(MouseScrollEvent{ .Offset = offset });
		}

		static void OnDrop(GLFWwindow* handle, int count, const char** paths)
		{
			FileDropEvent drop;
			drop.Paths.reserve(static_cast<size_t>(std::max(count, 0)));
			for (int index = 0; index < count; ++index)
			{
				if (paths[index] != nullptr)
					drop.Paths.emplace_back(paths[index]);
			}
			FromHandle(handle).Queue.emplace_back(std::move(drop));
		}
	};

	Window::Window(Scope<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	Window::~Window() = default;

	Window::Window(Window&& other) noexcept = default;

	Window& Window::operator=(Window&& other) noexcept = default;

	Result<Window> Window::Create(const WindowSpecification& specification)
	{
		constexpr uint32_t MaxSize = static_cast<uint32_t>(std::numeric_limits<int>::max());
		if (specification.Width == 0 || specification.Height == 0 || specification.Width > MaxSize || specification.Height > MaxSize)
		{
			return MakeError(ErrorCode::InvalidArgument, "window '{}' cannot be {}x{}: both sizes must be from 1 to {}",
				specification.Title, specification.Width, specification.Height, MaxSize);
		}
		if (!GlfwLibrary::IsInitialized())
		{
			return MakeError(ErrorCode::InvalidState, "cannot create window '{}': GLFW is not initialized (no ProcessContext)",
				specification.Title);
		}
		ENGINE_CORE_ASSERT(GlfwLibrary::IsMainThread(), "Window::Create called off the main thread");

		glfwDefaultWindowHints();
		// The renderer creates its own Vulkan surface (§8.1).
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, specification.Resizable ? GLFW_TRUE : GLFW_FALSE);

		GLFWmonitor* monitor = nullptr;
		int width = static_cast<int>(specification.Width);
		int height = static_cast<int>(specification.Height);
		if (specification.Fullscreen)
		{
			monitor = glfwGetPrimaryMonitor();
			const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
			if (mode == nullptr)
			{
				return MakeError(ErrorCode::Unsupported, "cannot create full-screen window '{}': no primary monitor ({})",
					specification.Title, GlfwLibrary::TakeErrorDescription());
			}
			// The monitor's current video mode, so that going full screen never switches modes.
			glfwWindowHint(GLFW_RED_BITS, mode->redBits);
			glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
			glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
			glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
			width = mode->width;
			height = mode->height;
		}

		// Discard an earlier error, so that a failure reports its own.
		static_cast<void>(glfwGetError(nullptr));
		GLFWwindow* handle = glfwCreateWindow(width, height, specification.Title.c_str(), monitor, nullptr);
		if (handle == nullptr)
		{
			return MakeError(ErrorCode::Unsupported, "GLFW cannot create window '{}': {}", specification.Title,
				GlfwLibrary::TakeErrorDescription());
		}

		return Window(CreateScope<Impl>(handle, specification.Title));
	}

	void Window::SetEventCallback(WindowEventCallback callback)
	{
		Impl::CheckUsable(m_Impl.get(), "SetEventCallback");
		if (m_Impl->IsDelivering)
			m_Impl->PendingCallback = std::move(callback);
		else
			m_Impl->Callback = std::move(callback);
	}

	void Window::PollEvents()
	{
		Impl::CheckUsable(m_Impl.get(), "PollEvents");
		ENGINE_CORE_ASSERT(!m_Impl->IsDelivering, "Window::PollEvents called from inside the event callback");
		if (m_Impl->IsDelivering)
			return;

		glfwPollEvents();
		m_Impl->PollGamepads();
		m_Impl->Deliver();
	}

	void Window::WaitEventsTimeout(double timeoutSeconds)
	{
		Impl::CheckUsable(m_Impl.get(), "WaitEventsTimeout");
		ENGINE_CORE_ASSERT(!m_Impl->IsDelivering, "Window::WaitEventsTimeout called from inside the event callback");
		if (m_Impl->IsDelivering)
			return;
		const bool isValidTimeout = timeoutSeconds > 0.0 && std::isfinite(timeoutSeconds);
		ENGINE_CORE_ASSERT(isValidTimeout, "Window::WaitEventsTimeout needs a positive, finite timeout (got {})", timeoutSeconds);

		// Events already queued (injected, or from a synchronous callback) are delivered without waiting.
		if (m_Impl->Queue.empty() && isValidTimeout)
			glfwWaitEventsTimeout(timeoutSeconds);
		else
			glfwPollEvents();
		m_Impl->PollGamepads();
		m_Impl->Deliver();
	}

	void Window::InjectEvent(Event event)
	{
		Impl::CheckUsable(m_Impl.get(), "InjectEvent");
		m_Impl->Queue.push_back(std::move(event));
	}

	void Window::Minimize()
	{
		Impl::CheckUsable(m_Impl.get(), "Minimize");
		glfwIconifyWindow(m_Impl->Handle);
	}

	void Window::Restore()
	{
		Impl::CheckUsable(m_Impl.get(), "Restore");
		glfwRestoreWindow(m_Impl->Handle);
	}

	void Window::SetTitle(std::string_view title)
	{
		Impl::CheckUsable(m_Impl.get(), "SetTitle");
		m_Impl->Title = title;
		glfwSetWindowTitle(m_Impl->Handle, m_Impl->Title.c_str());
	}

	const std::string& Window::GetTitle() const
	{
		Impl::CheckUsable(m_Impl.get(), "GetTitle");
		return m_Impl->Title;
	}

	void Window::SetCursorMode(CursorMode mode)
	{
		Impl::CheckUsable(m_Impl.get(), "SetCursorMode");
		const std::optional<int> glfwMode = Utils::ToGlfwCursorMode(mode);
		if (!glfwMode.has_value())
		{
			ENGINE_CORE_ASSERT(false, "Unknown CursorMode {}", std::to_underlying(mode));
			return;
		}

		glfwSetInputMode(m_Impl->Handle, GLFW_CURSOR, *glfwMode);
		// Unaccelerated motion where the OS offers it, for the relative camera motion Locked is for.
		if (glfwRawMouseMotionSupported() == GLFW_TRUE)
			glfwSetInputMode(m_Impl->Handle, GLFW_RAW_MOUSE_MOTION, mode == CursorMode::Locked ? GLFW_TRUE : GLFW_FALSE);
		m_Impl->Cursor = mode;
	}

	CursorMode Window::GetCursorMode() const
	{
		Impl::CheckUsable(m_Impl.get(), "GetCursorMode");
		return m_Impl->Cursor;
	}

	uint32_t Window::GetWidth() const
	{
		Impl::CheckUsable(m_Impl.get(), "GetWidth");
		return m_Impl->Width;
	}

	uint32_t Window::GetHeight() const
	{
		Impl::CheckUsable(m_Impl.get(), "GetHeight");
		return m_Impl->Height;
	}

	uint32_t Window::GetFramebufferWidth() const
	{
		Impl::CheckUsable(m_Impl.get(), "GetFramebufferWidth");
		return m_Impl->FramebufferWidth;
	}

	uint32_t Window::GetFramebufferHeight() const
	{
		Impl::CheckUsable(m_Impl.get(), "GetFramebufferHeight");
		return m_Impl->FramebufferHeight;
	}

	glm::vec2 Window::GetContentScale() const
	{
		Impl::CheckUsable(m_Impl.get(), "GetContentScale");
		glm::vec2 scale(1.0f);
		glfwGetWindowContentScale(m_Impl->Handle, &scale.x, &scale.y);
		return scale;
	}

	bool Window::IsMinimized() const
	{
		Impl::CheckUsable(m_Impl.get(), "IsMinimized");
		return m_Impl->IsMinimized();
	}

	bool Window::IsFocused() const
	{
		Impl::CheckUsable(m_Impl.get(), "IsFocused");
		return m_Impl->IsFocused;
	}

	void* Window::GetNativeHandle() const
	{
		Impl::CheckUsable(m_Impl.get(), "GetNativeHandle");
		return m_Impl->Handle;
	}

	std::string_view CursorModeToString(CursorMode mode)
	{
		switch (mode)
		{
			case CursorMode::Normal: return "Normal";
			case CursorMode::Hidden: return "Hidden";
			case CursorMode::Locked: return "Locked";
		}

		ENGINE_CORE_ASSERT(false, "Unknown CursorMode {}", std::to_underlying(mode));
		return "Unknown";
	}

}
