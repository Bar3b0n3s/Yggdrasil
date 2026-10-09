#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderStats: screenshot sampling cannot replace displayed view timings" * doctest::skip(true))
		{
			// Independent scene/game/capture histories.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("RenderStats: unavailable and delayed samples preserve source frame identity" * doctest::skip(true))
		{
			// GPU frame never relabelled as CPU frame; no fabricated zeros.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("RenderStats: every enabled pass is reported in fixed pass order" * doctest::skip(true))
		{
			// Disabled passes absent; repeated frames replace counters.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("RenderStats: enabled GPU passes have completed timings under both API caps" * doctest::skip(true))
		{
			// Publish delayed query frame; no hardware performance threshold.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Renderer: logs the 1080p frame time as a warning-only measurement" * doctest::skip(true))
		{
			// Performance record only; never fail on FPS or elapsed duration.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderRecordingContext: subpasses report independent CPU times and recorded counters" * doctest::skip(true))
		{
			// Manual clock; repeated names sum, counters saturate; error/empty/disabled paths close scopes; null GPU allowed.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderStats: an unavailable collection cannot relabel an earlier successful frame" * doctest::skip(true))
		{
			// Publish success N, unavailable, then success N+2; absent scope stays unavailable. Parent owns profiler failure injection.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderStats: toggling passes preserves CPU and GPU source-frame separation" * doctest::skip(true))
		{
			// PublishCpu changes enabled pass set; delayed old GPU names never become current-frame CPU counters.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("DebugViews: overdraw counts accepted fragments before depth rejection" * doctest::skip(true))
		{
			// Overlapping quads at differing depths; Mask discarded holes and Blend nonzero alpha counted; exact palette, no overlays.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
