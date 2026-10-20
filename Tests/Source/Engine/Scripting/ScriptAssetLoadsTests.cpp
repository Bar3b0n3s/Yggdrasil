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
		TEST_CASE("ScriptAssetLoads: literal Assets.Load references are found with their locations and every other use is computed" * doctest::skip(true))
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

			const Result<std::vector<ScriptAssetLoad>> broken = FindScriptAssetLoads("local x = \nlocal y = Assets.Load(\"a\")", "Assets/Scripts/Broken.luau");
			REQUIRE_FALSE(broken.has_value());
			CHECK(broken.error().GetCode() == ErrorCode::CompileFailed);
			CHECK(broken.error().GetLocation().Line == 2);
		}
	}

}
