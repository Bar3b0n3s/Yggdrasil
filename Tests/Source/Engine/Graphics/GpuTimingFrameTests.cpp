#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("GpuProfiler: delayed samples retain the identity of their recorded frame" * doctest::skip(true))
		{
			FAIL("M9 contract: use nonconsecutive host frame IDs to distinguish slots from frame identities");
		}

		TEST_CASE("GpuProfiler: an unavailable collection after a success exposes no relabelled samples" * doctest::skip(true))
		{
			FAIL("M9 contract: exercise a previously unused slot after collecting a successful frame");
		}
	}

}
