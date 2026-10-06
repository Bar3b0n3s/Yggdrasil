#include "EnginePCH.h"
#include "Engine/Platform/GlfwLibrary.h"

// M2 contract stub (Roadmap rule 3): stream A (window and input) implements GLFW initialization. Until then Initialize
// fails with Unsupported and the library reports itself uninitialized.

namespace Engine {

	Status GlfwLibrary::Initialize(const GlfwLibrarySpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GlfwLibrary::Initialize is not implemented yet");
	}

	void GlfwLibrary::Shutdown()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool GlfwLibrary::IsInitialized()
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	GlfwPlatform GlfwLibrary::GetPlatform()
	{
		ENGINE_CONTRACT_STUB();
		return GlfwPlatform::None;
	}

	WindowMode GlfwLibrary::GetMode()
	{
		ENGINE_CONTRACT_STUB();
		return WindowMode::Windowed;
	}

	uint32_t GlfwLibrary::GetWindowCount()
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::string_view WindowModeToString(WindowMode /*mode*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string_view GlfwPlatformToString(GlfwPlatform /*platform*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
