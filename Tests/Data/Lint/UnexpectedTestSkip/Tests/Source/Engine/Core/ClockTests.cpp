#include "TestsPCH.h"

#include "Support/TestOptions.h"

#include <string_view>

namespace Engine {

	namespace dt = doctest;

	struct ClockFixture
	{
		double Now = 0.0;
	};

	static const auto SkippedDecorator = doctest::skip(true); // Seeded defect: a skip outside any test decorator.

	TEST_SUITE("Core")
	{
		// The allowed control: a child-process target, its decorator expression spanning two lines.
		TEST_CASE("Clock: the hanging target blocks until it is killed" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
		}

		// The second control: a fully qualified suite, with a comment between the decorators.
		TEST_CASE("Clock: the crashing target ends the process"
			* doctest::test_suite(Engine::Test::ChildTargetSuite) // a child-process target
			* doctest::skip())
		{
		}

		TEST_CASE("Clock: ticks advance by the fixed delta" * doctest::skip(true)) // Seeded defect: a contract skip.
		{
		}

		// Seeded defect: the child-target suite is only named in a comment, so this is an ordinary skipped test.
		TEST_CASE("Clock: pausing stops the ticks" // * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
		}

		// Seeded defect: a test case name that spells the suite does not make a child target.
		TEST_CASE("Clock: Test::ChildTargetSuite is only text here" * doctest::skip(true))
		{
		}

		// Seeded defect: doctest named through a namespace alias, in a fixture test case.
		TEST_CASE_FIXTURE(ClockFixture, "Clock: resuming continues the ticks" * dt::skip(true))
		{
			CHECK(Now == 0.0);
		}

		// Controls: a string and a comment that spell a skip are not one: doctest::skip(true).
		TEST_CASE("Clock: a string that spells a skip is not one")
		{
			CHECK(std::string_view("* doctest::skip(true)").size() == 21);
		}

		TEST_CASE("Clock: the shared decorator skips this case" * SkippedDecorator)
		{
		}
	}

}
