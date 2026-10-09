#include "TestsPCH.h"
#include "EditorCore/Viewport/EditorViewportState.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorViewportState: scene and game report independent rendered extents" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorViewportState: hiding an image preserves camera aspect but refuses picking" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorViewportState: camera updates are atomic and do not dirty the scene" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorViewportState: options persist without a renderer" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorViewportRect: clicks map to the displayed image and exclude letterbox bars" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorViewportRect: zero size and minimized views reject input" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorViewportRect: resizing uses displayed pixels until the next rendered frame" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}
	}

}
