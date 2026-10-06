#include "EnginePCH.h"
#include "Engine/Platform/Window.h"

// M2 contract stub (Roadmap rule 3): stream A (window and input) implements the GLFW window. Until then Create fails
// with Unsupported, so no Window exists and the remaining members are never reached.

namespace Engine {

	struct Window::Impl
	{
	};

	Window::Window(Scope<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	Window::~Window() = default;

	Window::Window(Window&& other) noexcept = default;

	Window& Window::operator=(Window&& other) noexcept = default;

	Result<Window> Window::Create(const WindowSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Window::Create is not implemented yet");
	}

	void Window::SetEventCallback(WindowEventCallback /*callback*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Window::PollEvents()
	{
		ENGINE_CONTRACT_STUB();
	}

	void Window::WaitEventsTimeout(double /*timeoutSeconds*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Window::InjectEvent(Event /*event*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Window::Minimize()
	{
		ENGINE_CONTRACT_STUB();
	}

	void Window::Restore()
	{
		ENGINE_CONTRACT_STUB();
	}

	void Window::SetTitle(std::string_view /*title*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	const std::string& Window::GetTitle() const
	{
		ENGINE_CONTRACT_STUB();
		static const std::string EmptyTitle;
		return EmptyTitle;
	}

	void Window::SetCursorMode(CursorMode /*mode*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	CursorMode Window::GetCursorMode() const
	{
		ENGINE_CONTRACT_STUB();
		return CursorMode::Normal;
	}

	uint32_t Window::GetWidth() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t Window::GetHeight() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t Window::GetFramebufferWidth() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t Window::GetFramebufferHeight() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	glm::vec2 Window::GetContentScale() const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(1.0f);
	}

	bool Window::IsMinimized() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool Window::IsFocused() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void* Window::GetNativeHandle() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::string_view CursorModeToString(CursorMode /*mode*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
