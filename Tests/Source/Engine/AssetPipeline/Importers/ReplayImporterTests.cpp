#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/ReplayImporter.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("ReplayImporter: scene handle resolves after the readable path becomes stale" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Import a Replay whose Scene.Handle names a moved scene and Scene.Path is stale; resolve from the import's "
				 "immutable handle snapshot, record the scene dependency and cache lookup; reject unknown/non-Scene handles");
		}

		TEST_CASE("ReplayImporter: strict source validation reports precise pointers before cooking" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Use ReplayFromText strictUnknowns=true; reject unknown members, unsupported versions, malformed input events, "
				 "nonfinite parameters, invalid bounds/order/header/hash with exact JSON pointers and no partial artifact");
		}

		TEST_CASE("ReplayImporter: every expectation compiles through the shared compiler without execution" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Cook expressions at tick zero, repeated equal ticks and FinalTick through ScriptCompiler; preserve authored "
				 "order and nested ScriptData bytecode/ABI/source maps; expression referencing Scene does not execute or load it");
		}

		TEST_CASE("ReplayImporter: expectation syntax errors map to the authored expression" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Compile multiline UTF-8/CRLF invalid Expect source; report the real .replay and /Expect/<index>/Luau with "
				 "decoded-string line/byte column after subtracting wrapper offsets exactly once; suffix errors anchor at "
				 "authored EOF. Never report fake .luau files or embedded positions as physical JSON lines; retain previous asset");
		}

		TEST_CASE("ReplayImporter: expression and complete chunk forms retain exact authored attribution" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Import bare comparison and function-call expressions, existing return chunks, local-plus-return chunks and "
				 "leading comments; expression recognition consumes all input and adds one prefix line only to expressions. "
				 "No double return, source execution or synthetic require access; invalid forms fail. Cooked origins identify "
				 "real replay path, pointer and distinct same-tick indices; runtime locations survive a Dist round trip");
		}

		TEST_CASE("ReplayImporter: worker imports are deterministic and produce one Replay artifact" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("After main-thread process initialization compare repeated/parallel imports of identical bytes; one artifact "
				 "at metadata.Handle, no sub-assets/settings/dependency ownership; Dist loader needs no compiler");
		}
	}

}
