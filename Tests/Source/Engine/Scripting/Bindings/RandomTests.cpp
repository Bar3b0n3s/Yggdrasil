#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Core/Random.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <vector>

namespace Engine {

	namespace Test {

		void RunRandomBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local localRandom = Random.New(731)
localRandom:Seed(17)
Random.Seed(17)
assert(localRandom:Integer(-9007199254740991, 9007199254740991) == Random.Integer(-9007199254740991, 9007199254740991))
assert(localRandom:Number(-1.7e308, 1.7e308) == Random.Number(-1.7e308, 1.7e308))
assert(localRandom:Bool() == Random.Bool())
assert(localRandom:Choice({"a", "b", "c"}) == Random.Choice({"a", "b", "c"}))
local left, right = {1,2,3,4,5,6,7,8}, {1,2,3,4,5,6,7,8}
assert(localRandom:Shuffle(left) == left and Random.Shuffle(right) == right)
for index, value in left do assert(value == right[index]) end
local direction = localRandom:UnitVector()
assert(direction == Random.UnitVector())
assert(math.abs(vector.magnitude(direction) - 1) < 1e-6)
table.sort(left)
for index, value in left do assert(index == value) end
assert(localRandom:Choice(table.freeze({false})) == false)
assert(#localRandom:Shuffle({}) == 0)
assert(localRandom:Number(2, 2) == 2 and localRandom:Integer(2, 2) == 2)
assert(localRandom:Bool(1) and not localRandom:Bool(0))
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
				if (counter.Owner == "Random" || counter.Owner == "RandomGenerator")
				{
					CAPTURE(counter.Owner);
					CAPTURE(counter.Member);
					CHECK(counter.Calls > 0);
				}
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("RandomBindings: shared draws match the engine generator exactly")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
Random.Seed(731)
return {Random.Integer(-100, 100), Random.Number(), Random.Number(-2, 3), Random.Bool(0.3), Random.Number(7), Random.Number(nil, 4)}
)");
			REQUIRE(result.has_value());
			Random expected(731);
			Json values = Json::array();
			values.push_back(expected.RangeInt(-100, 100));
			values.push_back(expected.RangeDouble(0.0, 1.0));
			values.push_back(expected.RangeDouble(-2.0, 3.0));
			values.push_back(expected.NextBool(0.3));
			values.push_back(expected.RangeDouble(0.0, 7.0));
			values.push_back(expected.RangeDouble(0.0, 4.0));
			CHECK(result->Value.Get() == values);
			CHECK(fixture.GetRandom().GetState() == expected.GetState());
			CHECK(fixture.ExternalMutations.size() == 1);
		}

		TEST_CASE("RandomBindings: local streams reproduce shared APIs without sharing state")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunRandomBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("RandomBindings: invalid input never advances the shared stream or partially shuffles")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			const auto before = fixture.GetRandom().GetState();
			const auto result = fixture.Evaluate(R"(
local invalid = {
	function() Random.Seed(-1) end,
	function() Random.Seed(9007199254740992) end,
	function() Random.New(0.5) end,
	function() Random.Integer(2, 1) end,
	function() Random.Integer(-9007199254740992, 0) end,
	function() Random.Integer(0, 0.5) end,
	function() Random.Number(2, 1) end,
	function() Random.Number(math.huge) end,
	function() Random.Bool(-0.1) end,
	function() Random.Bool(1.1) end,
	function() Random.Bool(0/0) end,
	function() Random.Choice({}) end,
	function() Random.Choice({[2] = 5}) end,
	function() Random.Choice({[1] = 2, [3] = 4}) end,
	function() Random.Choice({[1] = 2, extra = 4}) end,
	function() Random.Shuffle(table.freeze({1, 2})) end,
	function() Random.Shuffle({[1.5] = 2}) end,
}
for index, fn in invalid do assert(not pcall(fn), tostring(index)) end
local hole = {[1] = 10, [3] = 30}
assert(not pcall(Random.Shuffle, hole))
return hole[1] == 10 and hole[2] == nil and hole[3] == 30
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.GetRandom().GetState() == before);
			CHECK(fixture.ExternalMutations.empty());
		}

		TEST_CASE("RandomBindings: read-only evaluation permits local streams and refuses every shared mutation")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			REQUIRE(fixture.Start().has_value());
			const auto before = fixture.GetRandom().GetState();
			const auto result = fixture.Evaluate(R"(
local array = {1, 2, 3}
for _, fn in {
	function() Random.Seed(5) end,
	function() Random.Integer(0, 1) end,
	function() Random.Number() end,
	function() Random.Bool() end,
	function() Random.Choice(array) end,
	function() Random.Shuffle(array) end,
	function() Random.UnitVector() end,
} do assert(not pcall(fn)) end
assert(array[1] == 1 and array[2] == 2 and array[3] == 3)
local random = Random.New(5)
random:Seed(731)
assert(random:Integer(0, 0) == 0)
assert(random:Number(0, 0) == 0)
assert(random:Bool(1))
assert(random:Choice({4}) == 4)
random:Shuffle(array)
assert(math.abs(vector.magnitude(random:UnitVector()) - 1) < 1e-6)
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.GetRandom().GetState() == before);
			CHECK(fixture.ExternalMutations.empty());
		}
	}

}
