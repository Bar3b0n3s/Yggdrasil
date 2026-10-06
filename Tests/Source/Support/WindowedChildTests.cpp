#include "TestsPCH.h"

#include "Support/WindowedChild.h"

#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Window.h"
#include "Support/TestOptions.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		// The targets below run only in windowed child processes (WindowedChild.h); they are permanently skipped here.
		TEST_CASE("WindowedChild: the target runs in a windowed process"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			CHECK(GlfwLibrary::GetMode() == WindowMode::Windowed);
			CHECK(GlfwLibrary::GetPlatform() != GlfwPlatform::Null);
			CHECK(Test::GetTestOptions().WindowedChild == "WindowedChild: the target runs in a windowed process");
			const Result<Window> window = Window::Create({ .Title = "Windowed child", .Width = 200, .Height = 100 });
			CHECK(window.has_value());
		}

		TEST_CASE("WindowedChild: the failing target" * doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			CHECK_MESSAGE(false, "this windowed child fails by design");
		}

		TEST_CASE("Tests: windowed child process runs a windowed case")
		{
			ENGINE_CHECK_WINDOWED_CHILD("WindowedChild: the target runs in a windowed process");
		}

		TEST_CASE("WindowedChild: a failing case makes the child exit 1 with doctest's report")
		{
			const Result<Test::WindowedChildResult> child = Test::RunWindowedChild("WindowedChild: the failing target");
			REQUIRE(child.has_value());
			CHECK(child->ExitCode == 1);
			CHECK(child->StandardOutput.contains("this windowed child fails by design"));
			CHECK_FALSE(Test::DescribeWindowedChildFailure("WindowedChild: the failing target").empty());
		}

		TEST_CASE("WindowedChild: a name that selects no test case exits 2")
		{
			const Result<Test::WindowedChildResult> child = Test::RunWindowedChild("WindowedChild: no such target");
			REQUIRE(child.has_value());
			CHECK(child->ExitCode == 2);
		}
	}

}
