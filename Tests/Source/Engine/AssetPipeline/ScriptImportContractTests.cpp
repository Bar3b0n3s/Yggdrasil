#include "TestsPCH.h"

#include "Engine/AssetPipeline/AssetCache.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/IAssetImporter.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("ScriptImport: cached checks preserve complete ranges and distinguish unchecked from clean" * doctest::skip())
		{
			FAIL("M13 contract: persist all range endpoints, fingerprint and root hash; missing provider is unchecked");
		}

		TEST_CASE("ScriptImport: changed checker configuration invalidates cached checks and failed attempts" * doctest::skip())
		{
			FAIL("M13 contract: new declarations/config hash rechecks unchanged source; stale job results cannot publish");
		}

		TEST_CASE("ScriptImport: a failed reimport publishes its diagnostics while retaining the last good artifact" * doctest::skip())
		{
			FAIL("M13 contract: latest source hash and located findings survive Import failure; dry run restores prior state");
		}

		TEST_CASE("ImportContext: handle lookups track moved scenes and invalidate when a missing handle appears" * doctest::skip())
		{
			FAIL("M13 contract: handle is authoritative; record misses, distinguish path queries and round-trip the manifest");
		}
	}

}
