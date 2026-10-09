#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Picking: retains the submitted table when a later snapshot reuses PickIds" * doctest::skip(true))
		{
			// A later table must not remap an old pixel.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Picking: stale revisions and click sequences are echoed for host rejection" * doctest::skip(true))
		{
			// No Scene reference or UUID recycled handle lookup in renderer.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Picking: resize cancels requests without mapping in-flight memory" * doctest::skip(true))
		{
			// Same-size resize preserves generation; actual resize cancels.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Picking: unpolled cancellations do not exhaust the staging pool" * doctest::skip(true))
		{
			// Cancel MaxPendingPicks, retire, then request again; bounded tombstones.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Picking: full queue and out-of-range pixels return located errors" * doctest::skip(true))
		{
			// No implicit replacement or GPU wait.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Picking: returns the UUID at known pixels" * doctest::skip(true))
		{
			// Known quadrants, background id zero, masked holes, both projections.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Picking: polling never waits and does not map unfinished submissions" * doctest::skip(true))
		{
			// Frame N cannot resolve before N+2; completion controlled by submission/query state.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Picking: unsubmitted frames and mismatched image identities are refused" * doctest::skip(true))
		{
			// Record frame N; old submitted N-1/table cannot satisfy N request. Match frame, revision and generation.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Picking: camera scene and unavailable-view changes invalidate pending clicks" * doctest::skip(true))
		{
			// Host calls CancelPicks and publishes the new generation; stale miss never clears current selection.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
