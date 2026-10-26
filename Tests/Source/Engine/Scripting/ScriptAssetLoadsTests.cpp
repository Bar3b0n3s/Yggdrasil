#include "TestsPCH.h"
#include "Engine/Scripting/ScriptAssetLoads.h"

#include "Support/Fuzz.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// The literal Assets.Load scan (Docs/Decisions/0030-m19-contract.md decision 19, stream D).

namespace Engine {

	namespace {

		// The literal and computed loads of the first case, the globals table's uses and the scan's other constructs: the
		// fuzz case's seed corpus.
		constexpr std::string_view LoaderSource = R"(local Loader = {}
local clip = Assets.Load("Assets/Audio/Beep.sfx")
local mesh = (Assets.Load("Assets/Models/Ship.glb#mesh:0:Hull") :: AssetRef)
local coin = Assets.Load([[engine://Audio/Coin]])
local font = Assets.Load(`00000000000001c1`)
local name = "Level"
local level = Assets.Load("Assets/Scenes/" .. name .. ".scene")
local load = Assets.Load
local alias = Assets
local typed: typeof(Assets.Load) = load
local path = Assets.GetPath(clip)
do
	local Assets = { Load = function(_: string) return nil end }
	Assets.Load("Assets/Shadowed.sfx")
end
return Loader
)";

		constexpr std::string_view GlobalsSource = R"(local a = _G.Assets.Load("engine://Audio/Coin")
local b = _G["Assets"].Load("Assets/Audio/Beep.sfx")
local c = _G.Assets
local d = _G["Ass" .. "ets"]
local e = rawget(_G, "Assets")
local f = _G
_G.print("x")
local g = _G["print"]
local h = _G.Assets:Load()
)";

		constexpr std::string_view OtherSource = R"(local name = "Beep"
local a = Assets:Load()
local b = Assets.Load()
local c = Assets["Load"]("x")
local d = Assets.Load(`Assets/Audio/{name}.sfx`)
local e = Assets.Load("Assets/Audio/Beep.sfx", 2)
local f = if name then Assets.Load("a") else Assets.Load(name)
local t: { Load: typeof(Assets.Load) } = { Load = Assets.Load }
type Loader = typeof(Assets)
return { a, b, c, d, e, f, t }
)";

	}

	TEST_SUITE("Scripting")
	{
		// Decision 19: a call of the global Assets.Load with one string literal is a literal load, in source order with
		// its location; every other use of Assets.Load, or of the global Assets other than by a field name, is a computed
		// load; a local named Assets and type annotations are not loads; a source that does not parse is CompileFailed at
		// its line.
		TEST_CASE("ScriptAssetLoads: literal Assets.Load references are found with their locations and every other use is computed")
		{
			const Result<std::vector<ScriptAssetLoad>> loads = FindScriptAssetLoads(LoaderSource, "Assets/Scripts/Loader.luau");
			REQUIRE_MESSAGE(loads.has_value(), loads.error().ToString());
			const std::vector<ScriptAssetLoad> expected = {
				{ .Reference = "Assets/Audio/Beep.sfx", .Literal = true, .Line = 2, .Column = 14 },
				{ .Reference = "Assets/Models/Ship.glb#mesh:0:Hull", .Literal = true, .Line = 3, .Column = 15 },
				{ .Reference = "engine://Audio/Coin", .Literal = true, .Line = 4, .Column = 14 },
				{ .Reference = "00000000000001c1", .Literal = true, .Line = 5, .Column = 14 },
				{ .Reference = {}, .Literal = false, .Line = 7, .Column = 15 },
				{ .Reference = {}, .Literal = false, .Line = 8, .Column = 14 },
				{ .Reference = {}, .Literal = false, .Line = 9, .Column = 15 },
			};
			CHECK(*loads == expected);

			// No Assets.Load at all, and Assets used by field names only.
			const Result<std::vector<ScriptAssetLoad>> none = FindScriptAssetLoads("local valid = Assets.IsValid(nil); return valid", "Assets/Scripts/None.luau");
			REQUIRE(none.has_value());
			CHECK(none->empty());

			// A method call passes Assets itself, and a call with no argument has no reference: both are computed.
			const Result<std::vector<ScriptAssetLoad>> other = FindScriptAssetLoads("local a = Assets:Load()\nlocal b = Assets.Load()\nlocal c = Assets[\"Load\"](\"x\")", "Assets/Scripts/Other.luau");
			REQUIRE(other.has_value());
			REQUIRE(other->size() == 3);
			for (const ScriptAssetLoad& load : *other)
			{
				CAPTURE(load.Line);
				CHECK_FALSE(load.Literal);
				CHECK(load.Reference.empty());
			}

			// The sandbox keeps _G: _G.Assets and _G["Assets"] are the global Assets, so their literal loads are found; _G used
			// any other way may reach Assets, so it is computed, while another field of _G is no use (the review of W19.1: a
			// load through _G was neither, so the export pruned what it named).
			const Result<std::vector<ScriptAssetLoad>> globals = FindScriptAssetLoads(GlobalsSource, "Assets/Scripts/Globals.luau");
			REQUIRE_MESSAGE(globals.has_value(), globals.error().ToString());
			const std::vector<ScriptAssetLoad> expectedGlobals = {
				{ .Reference = "engine://Audio/Coin", .Literal = true, .Line = 1, .Column = 11 },
				{ .Reference = "Assets/Audio/Beep.sfx", .Literal = true, .Line = 2, .Column = 11 },
				{ .Reference = {}, .Literal = false, .Line = 3, .Column = 11 },
				{ .Reference = {}, .Literal = false, .Line = 4, .Column = 11 },
				{ .Reference = {}, .Literal = false, .Line = 5, .Column = 18 },
				{ .Reference = {}, .Literal = false, .Line = 6, .Column = 11 },
				{ .Reference = {}, .Literal = false, .Line = 9, .Column = 11 },
			};
			CHECK(*globals == expectedGlobals);

			const Result<std::vector<ScriptAssetLoad>> broken = FindScriptAssetLoads("local x = \nlocal y = Assets.Load(\"a\")", "Assets/Scripts/Broken.luau");
			REQUIRE_FALSE(broken.has_value());
			CHECK(broken.error().GetCode() == ErrorCode::CompileFailed);
			CHECK(broken.error().GetLocation().Line == 2);
		}

		// Decision 19: a source that is not UTF-8 is refused before it is parsed, naming the chunk.
		TEST_CASE("ScriptAssetLoads: a source that is not valid UTF-8 is a Validation error")
		{
			const std::string source = "local clip = Assets.Load(\"\xff\")";
			const Result<std::vector<ScriptAssetLoad>> loads = FindScriptAssetLoads(source, "Assets/Scripts/Bytes.luau");
			REQUIRE_FALSE(loads.has_value());
			CHECK(loads.error().GetCode() == ErrorCode::Validation);
			CHECK(loads.error().GetLocation().File == "Assets/Scripts/Bytes.luau");
		}

		// Decision 19's scan reads every packaged script of an export and, with PruneUnreferenced, of project.validate: a
		// reader of external input (ReviewChecklist), whose visitor of Luau's tree (literal and interpolated strings, _G,
		// index expressions, a tree the parser recovered from errors) is fuzzed here and in the hardening stage
		// (Tests/Data/Fuzz/Targets.json, ScriptAssetLoads). The oracle: an error is CompileFailed located at a line, or
		// Validation; a result lists loads located at line and column 1 or later, in source order, a literal one with its
		// reference and a computed one without.
		TEST_CASE("ScriptAssetLoads: 10,000 seeded mutations of Luau source never crash and always return Result" * doctest::test_suite(Test::FuzzSuite))
		{
			const std::array<std::string_view, 3> seeds = { LoaderSource, GlobalsSource, OtherSource };
			const Test::FuzzRun run = Test::RunFuzzCase(0x5A1D, 10000, [&seeds](Random& random, uint64_t /*index*/)
			{
				std::string mutated(seeds[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(seeds.size()) - 1))]);
				Test::MutateText(mutated, random);
				const Result<std::vector<ScriptAssetLoad>> loads = FindScriptAssetLoads(mutated, "Assets/Scripts/Fuzz.luau");
				if (!loads.has_value())
				{
					const ErrorCode code = loads.error().GetCode();
					CHECK((code == ErrorCode::CompileFailed || code == ErrorCode::Validation));
					if (code == ErrorCode::CompileFailed)
						CHECK(loads.error().GetLocation().Line >= 1);
					return false;
				}
				for (size_t index = 0; index < loads->size(); ++index)
				{
					const ScriptAssetLoad& load = (*loads)[index];
					CAPTURE(index);
					CHECK(load.Line >= 1);
					CHECK(load.Column >= 1);
					if (!load.Literal)
						CHECK(load.Reference.empty());
					if (index > 0)
						CHECK(std::tie((*loads)[index - 1].Line, (*loads)[index - 1].Column) <= std::tie(load.Line, load.Column));
				}
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Accepted > 0);
				CHECK(run.Counts.Rejected > 0);
			}
		}
	}

}
