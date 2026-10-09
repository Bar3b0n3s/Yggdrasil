#include "TestsPCH.h"
#include "EditorCore/EditorActions.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("CommandHistory: UI and agent commands interleave in one history" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorActions: UI writes are attributed to ui and never agent" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorActions: invalid params and read-only writes fail before enqueue" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorActions: pending user actions cancel through the shared handler" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorActions: queued actions execute outside ImGui traversal" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}
	}

}
