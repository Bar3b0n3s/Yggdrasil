#include "TestsPCH.h"
#include "EditorCore/Automation/RecoveryMethods.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("RecoveryMethods: project.open recover adopts a validated autosave" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("RecoveryMethods: project.open without recover reports the offer and leaves source intact" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("RecoveryMethods: failed recovery releases the project lock and stays in launcher" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("RecoveryMethods: a read-only editor refuses recovery" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("RecoveryMethods: recovery choice never reopens an already-open project" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: open recover=false and retain its lock; assert another project.open is InvalidState while the injected UI acceptance uses that original open project");
		}
	}

}
