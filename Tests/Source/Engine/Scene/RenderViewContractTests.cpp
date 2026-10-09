#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderExtraction: directional shadow fields and project quality settings reach the snapshot" * doctest::skip(true))
		{
			// All five directional fields nondefault; ShadowMapSize and SsaoHalfResolution.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("RenderExtraction: PickTable uses canonical UUIDs before culling" * doctest::skip(true))
		{
			// Unique nonzero ids retained through draw sorting and submeshes.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("RenderExtraction: selected UUIDs and overlay flags are copied by value" * doctest::skip(true))
		{
			// Unknown ids ignored; no retained spans or component pointers.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderValidation: lighting warnings respect fallback environment and emissive materials" * doctest::skip(true))
		{
			// Empty/text-only/emissive-only no warning; nonemissive mesh plus black ambient warns; emission cannot illuminate another mesh.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("ProjectValidator: render warnings have stable IDs and no automatic fixes" * doctest::skip(true))
		{
			// In-process project.validate for open and scratch scenes; NoLighting and nine shadowed spots Warning, scene file, unchanged history.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("ProjectValidator: Basic3D illumination has no render errors or false lighting warning" * doctest::skip(true))
		{
			// Sun or effective positive environment suppresses NoLighting; distinguish AudioNoListener Warning from validation errors.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
