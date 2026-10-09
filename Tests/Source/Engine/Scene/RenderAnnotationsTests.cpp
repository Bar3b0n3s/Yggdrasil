#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderAnnotations: colliders use snapshot alpha and match rendered meshes" * doctest::skip(true))
		{
			// ADR0016 running play view below alpha one, including a child collider.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("RenderAnnotations: labels bounds and axes are capture-local" * doctest::skip(true))
		{
			// No changes to scene, selection, persistent options or later screenshots.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("RenderAnnotations: plain glyph records need no editor UI or font" * doctest::skip(true))
		{
			// Runtime screenshot and synthetic Renderer snapshot work without Editor dependencies.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderAnnotations: explicit label IDs are independent of editor selection" * doctest::skip(true))
		{
			// Explicit empty list emits none; stale IDs filtered; disabled and pending entities excluded; bounds/axes independent.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
