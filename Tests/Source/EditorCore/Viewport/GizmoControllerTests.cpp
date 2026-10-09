#include "TestsPCH.h"
#include "EditorCore/Viewport/GizmoController.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GizmoController: a drag produces one merged undo step" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("GizmoController: escape discards the preview without changing history" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("GizmoController: an agent edit during a drag is preserved and causes Conflict" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("GizmoController: selecting a parent and child applies the world delta once" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("GizmoController: local and world manipulation preserve parent transforms" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("GizmoController: noninvertible parents and shear fail before any write" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("GizmoController: read-only scenes reject a drag and play edits are transient" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("GizmoController: snapping uses metres degrees and scale increments" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("GizmoController: scene close and focus loss discard the preview" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}
	}

}
