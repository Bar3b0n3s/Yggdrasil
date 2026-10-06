#include "TestsPCH.h"

#include "Engine/Platform/GlfwLibrary.h"

#include "Engine/Platform/Window.h"
#include "Support/DeathTest.h"

namespace Engine {

	// Every Tests process, death-test children included, starts with GLFW initialized by the Tests main.
	ENGINE_DEATH_TEST("Platform/GlfwInitializedTwice")
	{
		static_cast<void>(GlfwLibrary::Initialize({ .Mode = WindowMode::Headless }));
	}

	ENGINE_DEATH_TEST("Platform/GlfwShutdownWithLiveWindow")
	{
		const Result<Window> window = Window::Create({ .Title = "Still open", .Width = 64, .Height = 64 });
		if (window.has_value())
			GlfwLibrary::Shutdown();
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("GlfwLibrary: the Tests process runs GLFW headless on the null platform")
		{
			// The Tests main's ProcessContext initialized GLFW once, headless (Architecture §15.2).
			CHECK(GlfwLibrary::IsInitialized());
			CHECK(GlfwLibrary::GetMode() == WindowMode::Headless);
			CHECK(GlfwLibrary::GetPlatform() == GlfwPlatform::Null);
		}

		TEST_CASE("GlfwLibrary: WindowModeToString and GlfwPlatformToString name every value")
		{
			CHECK(WindowModeToString(WindowMode::Windowed) == "Windowed");
			CHECK(WindowModeToString(WindowMode::Headless) == "Headless");
			CHECK(GlfwPlatformToString(GlfwPlatform::None) == "None");
			CHECK(GlfwPlatformToString(GlfwPlatform::Null) == "Null");
			CHECK(GlfwPlatformToString(GlfwPlatform::Win32) == "Win32");
			CHECK(GlfwPlatformToString(GlfwPlatform::Cocoa) == "Cocoa");
			CHECK(GlfwPlatformToString(GlfwPlatform::X11) == "X11");
		}

		TEST_CASE("GlfwLibrary: initializing twice without Shutdown asserts")
		{
			ENGINE_CHECK_DEATH("Platform/GlfwInitializedTwice", "GLFW is already initialized");
		}

		TEST_CASE("GlfwLibrary: shutting down while a Window is alive is fatal")
		{
			ENGINE_CHECK_DEATH("Platform/GlfwShutdownWithLiveWindow", "GlfwLibrary::Shutdown with 1 live Window(s)");
		}

		TEST_CASE("GlfwLibrary: every live Window is counted")
		{
			const uint32_t before = GlfwLibrary::GetWindowCount();
			{
				Result<Window> first = Window::Create({ .Title = "First", .Width = 32, .Height = 32 });
				Result<Window> second = Window::Create({ .Title = "Second", .Width = 32, .Height = 32 });
				REQUIRE(first.has_value());
				REQUIRE(second.has_value());
				CHECK(GlfwLibrary::GetWindowCount() == before + 2);

				// Moving a Window moves its registration with it; assigning over one destroys the window it held.
				Window moved = std::move(*first);
				CHECK(GlfwLibrary::GetWindowCount() == before + 2);
				moved = std::move(*second);
				CHECK(GlfwLibrary::GetWindowCount() == before + 1);
			}
			CHECK(GlfwLibrary::GetWindowCount() == before);
		}
	}

}
