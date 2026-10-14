#include "TestsPCH.h"

#include "Support/Fuzz.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"
#include "Support/TestOptions.h"

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

// The fuzz support of the unit and hardening stages (Docs/Decisions/0024-m15-contract.md decisions 13 and 14, stream C).
// The options are process-level state (SetTestOptions); each case restores the run's options before it ends.

namespace Engine {

	namespace {

		// Runs `body` with `options` as the process's test options, then restores the previous ones.
		template<typename Body>
		void WithOptions(const Test::TestOptions& options, Body body)
		{
			const Test::TestOptions previous = Test::GetTestOptions();
			Test::SetTestOptions(options);
			body();
			Test::SetTestOptions(previous);
		}

	}

	TEST_SUITE("Support")
	{
		TEST_CASE("Fuzz: iteration seeds depend only on the case seed and the index" * doctest::skip(true))
		{
			std::set<uint64_t> seeds;
			for (uint64_t index = 0; index < 1000; ++index)
				seeds.insert(Test::GetFuzzIterationSeed(0xC0FFEE, index));
			CHECK(seeds.size() == 1000);
			CHECK(Test::GetFuzzIterationSeed(0xC0FFEE, 7) == Test::GetFuzzIterationSeed(0xC0FFEE, 7));
			CHECK(Test::GetFuzzIterationSeed(0xC0FFEE, 7) != Test::GetFuzzIterationSeed(0xC0FFEF, 7));
			// SplitMix64 of the state caseSeed + (index + 1) * 0x9E3779B97F4A7C15 (Fuzz.h): the first output for seed 0.
			CHECK(Test::GetFuzzIterationSeed(0, 0) == 0xE220A8397B1DCDAFull);
		}

		TEST_CASE("Fuzz: the range honours the acceptance count, the scale, an absolute count and shards without overlap" * doctest::skip(true))
		{
			Test::TestOptions options = Test::GetTestOptions();
			options.FuzzIterations = 0;
			options.FuzzScale = 1.0;
			options.FuzzShardIndex = 0;
			options.FuzzShardCount = 1;
			WithOptions(options, []()
			{
				const Test::FuzzRange range = Test::GetFuzzRange(10000);
				CHECK(range.Begin == 0);
				CHECK(range.End == 10000);
				CHECK(range.Total == 10000);
			});
			options.FuzzScale = 0.1;
			WithOptions(options, []()
			{
				CHECK(Test::GetFuzzRange(10000).Total == 1000);
				CHECK(Test::GetFuzzRange(10000, 2500).Total == 2500); // the minimum wins
			});
			options.FuzzScale = 1.0;
			options.FuzzIterations = 1000000;
			options.FuzzShardCount = 3;
			uint64_t next = 0;
			for (uint32_t shard = 0; shard < 3; ++shard)
			{
				options.FuzzShardIndex = shard;
				WithOptions(options, [&next, shard]()
				{
					CAPTURE(shard);
					const Test::FuzzRange range = Test::GetFuzzRange(10000);
					CHECK(range.Total == 1000000);
					CHECK(range.Begin == next);
					CHECK(range.End - range.Begin == (shard == 0 ? 333334u : 333333u));
					next = range.End;
				});
			}
			CHECK(next == 1000000);

			// --fuzz-index replays one iteration of the same total.
			options.FuzzShardIndex = 0;
			options.FuzzShardCount = 1;
			options.FuzzIndex = 777777;
			WithOptions(options, []()
			{
				const Test::FuzzRange range = Test::GetFuzzRange(10000);
				CHECK(range.Total == 1000000);
				CHECK(range.Begin == 777777);
				CHECK(range.End == 777778);
			});
		}

		TEST_CASE("Fuzz: each run reports one line the hardening stage counts" * doctest::skip(true))
		{
			const Test::ExpectLog line(LogLevel::Info,
				"Fuzz 'PakReader: 10,000 seeded mutations': iterations 0..500 of 1000 (seed 0x00000000000009a4): 12 accepted, 488 rejected");
			Test::ReportFuzzRun("PakReader: 10,000 seeded mutations", 0x9A4, Test::FuzzRange{ .Begin = 0, .End = 500, .Total = 1000 },
				Test::FuzzCounts{ .Accepted = 12, .Rejected = 488 });
			CHECK(line.GetMatchCount() == 1);
		}

		// Whether every listed case exists and ran its iterations is the hardening stage's check (Lib/hardening.py
		// evaluate_fuzz_logs), which sees the run.
		TEST_CASE("Fuzz: Targets.json lists every required reader of external input" * doctest::skip(true))
		{
			const Result<std::string> text = Test::ReadTestDataText("Fuzz/Targets.json");
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			Result<Json> targets = JsonReader::Parse(*text);
			REQUIRE(targets.has_value());
			CHECK((*targets)["Format"] == "FuzzTargets");
			CHECK((*targets)["Version"] == 1);
			// The required readers of decision 13; Targets.json may list more.
			const std::vector<std::string> required = { "AssetMetadata", "AudioClipData", "AudioDecoder", "AutosaveRecovery", "BinaryReader",
				"BuiltinAssetCatalog", "Bindings", "CookedFormat", "DecodePng", "EditorPreferences", "EnvironmentData", "EnvironmentImporter", "FontData",
				"FontImporter", "Framing", "GameManifest", "GltfImporter", "JsonReader", "JsonRpc", "MaterialData", "MeshData", "PakReader", "PakToc",
				"PlayInputEventCodec", "Prefab", "ProjectLock", "ProjectSerializer", "ReplayData", "SceneSerializer", "ScriptCompiler", "ScriptData",
				"ShaderReflection", "SoundEffect", "TextureData", "TextureImporter" };
			const Json& listed = (*targets)["Targets"];
			REQUIRE(listed.is_array());
			for (const std::string& reader : required)
			{
				CAPTURE(reader);
				bool found = false;
				for (const Json& target : listed)
					found = found || (target.is_object() && target.contains("Reader") && target["Reader"] == reader);
				CHECK(found);
			}
			// Every target runs 1,000,000 iterations in the hardening stage: no reader is fuzzed less (decision 13).
			for (const Json& target : listed)
			{
				CAPTURE(target.dump());
				REQUIRE(target.is_object());
				CHECK(target.contains("Iterations"));
				CHECK(target.contains("Case"));
				if (target.contains("Iterations"))
					CHECK(target["Iterations"] == 1000000);
			}
		}
	}

}
