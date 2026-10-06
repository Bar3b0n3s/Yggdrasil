#include "TestsPCH.h"

#include "Engine/Platform/GlfwLibrary.h"

namespace Engine {

	TEST_SUITE("Platform")
	{
		TEST_CASE("GlfwLibrary: the Tests process runs GLFW headless on the null platform" * doctest::skip(true))
		{
			// The Tests main's ProcessContext initialized GLFW once, headless (Architecture §15.2).
			CHECK(GlfwLibrary::IsInitialized());
			CHECK(GlfwLibrary::GetMode() == WindowMode::Headless);
			CHECK(GlfwLibrary::GetPlatform() == GlfwPlatform::Null);
		}

		TEST_CASE("GlfwLibrary: WindowModeToString and GlfwPlatformToString name every value" * doctest::skip(true))
		{
			CHECK(WindowModeToString(WindowMode::Windowed) == "Windowed");
			CHECK(WindowModeToString(WindowMode::Headless) == "Headless");
			CHECK(GlfwPlatformToString(GlfwPlatform::None) == "None");
			CHECK(GlfwPlatformToString(GlfwPlatform::Null) == "Null");
			CHECK(GlfwPlatformToString(GlfwPlatform::Win32) == "Win32");
			CHECK(GlfwPlatformToString(GlfwPlatform::Cocoa) == "Cocoa");
			CHECK(GlfwPlatformToString(GlfwPlatform::X11) == "X11");
		}
	}

}
