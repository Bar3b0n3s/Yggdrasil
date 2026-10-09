#include "TestsPCH.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ThumbnailCache: cache keys include asset version size and renderer format" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("ThumbnailCache: failed rendering preserves the previous cached image" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("ThumbnailCache: read-only projects use their private cache" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("ThumbnailCache: nonvisual assets use a type icon without a dummy image" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("ThumbnailCache: stale versions and invalid sizes fail without a cache write" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("ThumbnailCache: switching projects with matching asset identifiers never reuses an old image" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: bind distinct project generations with equal handles and versions; assert each result lives under its own cache root");
		}

		TEST_CASE("ThumbnailCache: reset cancels old queued work before another project is bound" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: queue then Reset and bind a new project; assert Pump never invokes the renderer for the old request");
		}

		TEST_CASE("ThumbnailCache: stale request generations fail in request queue and find" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: reopen even the same path with a new generation; assert old requests return Conflict before I/O or cache lookup");
		}

		TEST_CASE("ThumbnailCache: unbound and reused project generations fail without cache access" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: assert unbound operations are InvalidState and zero or reused BindProject generations cannot change state");
		}
	}

}
