#include "TestsPCH.h"
#include "Engine/Scripting/ScriptAssetLoads.h"

#include <string>
#include <string_view>
#include <vector>

// The literal Assets.Load scan (Docs/Decisions/0030-m19-contract.md decision 19, stream D).

namespace Engine {

	TEST_SUITE("Scripting")
	{
		// Decision 19: a call of the global Assets.Load with one string literal is a literal load, in source order with
		// its location; every other use of Assets.Load, or of the global Assets other than by a field name, is a computed
		// load; a local named Assets and type annotations are not loads; a source that does not parse is CompileFailed at
		// its line.
		TEST_CASE("ScriptAssetLoads: literal Assets.Load references are found with their locations and every other use is computed")
		{
			constexpr std::string_view Source = R"(local Loader = {}
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
			const Result<std::vector<ScriptAssetLoad>> loads = FindScriptAssetLoads(Source, "Assets/Scripts/Loader.luau");
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
			constexpr std::string_view Globals = R"(local a = _G.Assets.Load("engine://Audio/Coin")
local b = _G["Assets"].Load("Assets/Audio/Beep.sfx")
local c = _G.Assets
local d = _G["Ass" .. "ets"]
local e = rawget(_G, "Assets")
local f = _G
_G.print("x")
local g = _G["print"]
local h = _G.Assets:Load()
)";
			const Result<std::vector<ScriptAssetLoad>> globals = FindScriptAssetLoads(Globals, "Assets/Scripts/Globals.luau");
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
	}

}
