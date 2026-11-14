#include "TestsPCH.h"

#include "Engine/Platform/PlatformWindowSupport.h"

#include "Engine/Platform/GlfwLibrary.h"
#include "Support/TestOptions.h"
#include "Support/WindowedChild.h"

#include <doctest/doctest.h>

// Platform windows' platform layer (M24, C24.1, E-VIEWPORTS; Docs/Decisions/0035-m24-contract.md decision 17, stream V). Windows
// backends only (ADR 0028 decision 10): a windowed process on Windows may show platform windows; a headless one may not, so
// headless editors, their screenshots and the editor-UI goldens keep the docked layout; Linux and macOS refuse them, which the
// portability stage compiles and no machine runs.

namespace Engine {

	TEST_SUITE("Platform")
	{
		// Runs only in a windowed child process (Support/WindowedChild.h): GLFW's native platform.
		TEST_CASE("PlatformWindowSupport: a windowed process on Windows may show platform windows"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			REQUIRE(GlfwLibrary::GetPlatform() != GlfwPlatform::Null);
#if defined(ENGINE_PLATFORM_WINDOWS)
			const Status supported = CheckPlatformWindowSupport();
			CHECK_MESSAGE(supported.has_value(), supported.error().ToString());
#else
			CHECK(CheckPlatformWindowSupport().error().GetCode() == ErrorCode::Unsupported);
#endif
		}

		TEST_CASE("PlatformWindowSupport: a headless process refuses platform windows and a windowed one on Windows allows them")
		{
			// The Tests process is headless (GLFW's null platform): no OS windows, so platform windows are refused there.
			REQUIRE(GlfwLibrary::GetPlatform() == GlfwPlatform::Null);
			const Status headless = CheckPlatformWindowSupport();
			REQUIRE_FALSE(headless.has_value());
			CHECK(headless.error().GetCode() == ErrorCode::Unsupported);
			ENGINE_CHECK_WINDOWED_CHILD("PlatformWindowSupport: a windowed process on Windows may show platform windows");
		}
	}

}
