#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Scripting/Sandbox.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include <vector>

namespace Engine {

	static void CheckQuatScript(Test::ScriptTestFixture& fixture, std::string_view source)
	{
		const auto result = fixture.Evaluate(source);
		if (!result)
			INFO(result.error().ToString());
		REQUIRE(result.has_value());
		CHECK(result->Value.Get() == Json(true));
	}

	namespace Test {

		void RunQuatBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			REQUIRE(fixture.Start().has_value());
			CheckQuatScript(fixture, R"(
local function near(a, b)
	assert(vector.magnitude(a - b) < 1e-5, tostring(a) .. " ~= " .. tostring(b))
end
local q = Quat.New(1, 2, 3, 4)
assert(q.x == 1 and q.y == 2 and q.z == 3 and q.w == 4)
q.x = 2; q.y = 3; q.z = 4; q.w = 5
assert(q.x == 2 and q.y == 3 and q.z == 4 and q.w == 5)
local normalized = q:Normalized()
assert(math.abs(normalized.x^2 + normalized.y^2 + normalized.z^2 + normalized.w^2 - 1) < 1e-6)
local identity = Quat.Identity()
assert(identity.x == 0 and identity.y == 0 and identity.z == 0 and identity.w == 1)
local product = q * q:Inverse()
assert(math.abs(product.x) < 1e-6 and math.abs(product.y) < 1e-6 and math.abs(product.z) < 1e-6)
assert(math.abs(product.w - 1) < 1e-6)
local euler = Quat.FromEuler(30, 40, 50)
near(euler:ToEuler(), vector.create(30, 40, 50))
local quarter = Quat.AngleAxis(90, vector.create(0, 5, 0))
near(quarter:Rotate(vector.create(0, 0, -1)), vector.create(-1, 0, 0))
near(quarter * vector.create(0, 0, -1), vector.create(-1, 0, 0))
local halfway = Quat.Slerp(identity, quarter, 0.5)
near(halfway:Rotate(vector.create(0, 0, -1)), vector.create(-0.7071067811865475, 0, -0.7071067811865475))
local look = Quat.LookRotation(vector.create(1, 0, 0))
near(look * vector.create(0, 0, -1), vector.create(1, 0, 0))
return true
)");
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			size_t members = 0;
			for (const auto& counter : coverage->Members)
			{
				if (counter.Owner != "Quat")
					continue;
				CAPTURE(counter.Member);
				if (counter.Kind == ScriptApiMemberKind::Property)
				{
					CHECK(counter.Reads > 0);
					CHECK(counter.Writes > 0);
				}
				else
					CHECK(counter.Calls > 0);
				++members;
			}
			CHECK(members == 15);
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("QuatBindings: constructors methods operators and fields have counted calls")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunQuatBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("QuatBindings: Euler order and composition follow the engine right-handed convention")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckQuatScript(fixture, R"(
local function near(a, b)
	assert(vector.magnitude(a - b) < 2e-5)
end
local x = Quat.AngleAxis(90, vector.create(1, 0, 0))
local y = Quat.AngleAxis(90, vector.create(0, 1, 0))
local z = Quat.AngleAxis(90, vector.create(0, 0, 1))
local forward = vector.create(0, 0, -1)
near(x * forward, vector.create(0, 1, 0))
near(y * forward, vector.create(-1, 0, 0))
near(z * vector.create(1, 0, 0), vector.create(0, 1, 0))
near((x * y) * forward, vector.create(-1, 0, 0))
near((y * x) * forward, vector.create(0, 1, 0))
local fromEuler = Quat.FromEuler(25, 70, -35)
local composed = Quat.AngleAxis(70, vector.create(0, 1, 0))
	* Quat.AngleAxis(25, vector.create(1, 0, 0))
	* Quat.AngleAxis(-35, vector.create(0, 0, 1))
near(fromEuler * forward, composed * forward)
near(fromEuler * vector.create(1, 0, 0), composed * vector.create(1, 0, 0))
for _, angles in {vector.create(90, 40, 30), vector.create(-90, -60, 10), vector.create(170, -220, 530)} do
	local original = Quat.FromEuler(angles.x, angles.y, angles.z)
	local extracted = original:ToEuler()
	assert(extracted.x >= -90 and extracted.x <= 90)
	assert(extracted.y > -180 and extracted.y <= 180 and extracted.z > -180 and extracted.z <= 180)
	local roundTrip = Quat.FromEuler(extracted.x, extracted.y, extracted.z)
	near(original * forward, roundTrip * forward)
	near(original * vector.create(1, 0, 0), roundTrip * vector.create(1, 0, 0))
end
return true
)");
		}

		TEST_CASE("QuatBindings: LookRotation aligns negative Z and projects the requested up direction")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckQuatScript(fixture, R"(
local forward = vector.create(0, 0, -1)
local up = vector.create(0, 1, 0)
local identity = Quat.LookRotation(forward)
assert(vector.magnitude(identity * forward - forward) < 1e-6)
assert(vector.magnitude(identity * up - up) < 1e-6)
local tilted = Quat.LookRotation(vector.create(0, 0, -5), vector.create(2, 0, 4))
assert(vector.magnitude(tilted * forward - forward) < 1e-6)
assert(vector.magnitude(tilted * up - vector.create(1, 0, 0)) < 1e-6)
local diagonal = Quat.LookRotation(vector.create(3, 4, -5), up)
assert(vector.magnitude(diagonal * forward - vector.normalize(vector.create(3, 4, -5))) < 1e-6)
return true
)");
		}

		TEST_CASE("QuatBindings: Slerp follows the shortest arc and normalizes nonunit inputs")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckQuatScript(fixture, R"(
local forward = vector.create(0, 0, -1)
local a = Quat.New(0, 0, 0, 2)
local b = Quat.AngleAxis(90, vector.create(0, 1, 0))
local negativeB = Quat.New(-b.x, -b.y, -b.z, -b.w)
local half = Quat.Slerp(a, negativeB, 0.5)
assert(vector.magnitude(half * forward - vector.create(-0.70710678, 0, -0.70710678)) < 1e-6)
assert(vector.magnitude(Quat.Slerp(a, b, -10) * forward - forward) < 1e-6)
assert(vector.magnitude(Quat.Slerp(a, b, 10) * forward - b * forward) < 1e-6)
assert(vector.magnitude(Quat.Slerp(b, negativeB, 0.3) * forward - b * forward) < 1e-6)
local almost = Quat.AngleAxis(0.0001, vector.create(1, 0, 0))
local close = Quat.Slerp(Quat.Identity(), almost, 0.5)
assert(math.abs(close.x^2 + close.y^2 + close.z^2 + close.w^2 - 1) < 1e-6)
assert(vector.magnitude(Quat.New(0, 0, 0, 20):Rotate(vector.create(3, 4, 5)) - vector.create(3, 4, 5)) < 1e-6)
return true
)");
		}

		TEST_CASE("QuatBindings: local edits remain writable in read-only eval without host mutation")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			REQUIRE(fixture.Start().has_value());
			CheckQuatScript(fixture, R"(
local original = Quat.Identity()
local normalized = original:Normalized()
normalized.x = 1; normalized.y = 2; normalized.z = 3; normalized.w = 4
assert(original.x == 0 and original.y == 0 and original.z == 0 and original.w == 1)
assert(normalized.x == 1 and normalized.y == 2 and normalized.z == 3 and normalized.w == 4)
assert(not pcall(function() normalized.x = 0/0 end))
assert(normalized.x == 1)
assert(not pcall(function() original.w = 0 end))
assert(original.w == 1)
assert(not pcall(function() normalized.unknown = 10 end))
assert(not pcall(function() Quat.New = false end))
return true
)");
			CHECK(fixture.ExternalMutations.empty());
			for (const auto& group : fixture.GetApi().GetTypes())
			{
				if (group.Name != "Quat")
					continue;
				for (const auto& member : group.Members)
				{
					CAPTURE(member.Name);
					CHECK(member.Options.Environments == ScriptApiEnvironment::All);
					CHECK(member.Options.Modes == RunModes::All);
					CHECK_FALSE(member.Options.Mutates);
					CHECK_FALSE(member.Options.SetterMutates);
					CHECK_FALSE(member.Description.empty());
				}
			}
		}

		TEST_CASE("QuatBindings: zero nonfinite malformed and unrepresentable values are rejected")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			CheckQuatScript(fixture, R"(
local invalid = {
	function() Quat.New(0, 0, 0, 0) end,
	function() Quat.New("0", 0, 0, 1) end,
	function() Quat.New(math.huge, 0, 0, 1) end,
	function() Quat.New(1e308, 0, 0, 1) end,
	function() Quat.AngleAxis(0/0, vector.create(1, 0, 0)) end,
	function() Quat.AngleAxis(30, vector.zero) end,
	function() Quat.LookRotation(vector.zero) end,
	function() Quat.LookRotation(vector.create(0, 1, 0)) end,
	function() Quat.LookRotation(vector.create(0, 0, -1), vector.zero) end,
	function() Quat.FromEuler(0, math.huge, 0) end,
	function() Quat.Slerp(Quat.Identity(), Color.New(1, 1, 1), 0.5) end,
	function() Quat.Slerp(Quat.Identity(), Quat.Identity(), math.huge) end,
	function() return Quat.New(1e38, 1e38, 1e38, 1e38) * Quat.New(1e38, 1e38, 1e38, 1e38) end,
	function() return Quat.New(0, 0, 0, 1e-38) * Quat.New(0, 0, 0, 1e-38) end,
	function() return Quat.Identity() * 2 end,
	function() return Quat.Identity():Rotate(vector.create(math.huge, 0, 0)) end,
}
for index, fn in invalid do
	assert(not pcall(fn), "invalid case accepted: " .. index)
end
local small = Quat.New(1e-38, 0, 0, 1e-38):Normalized()
local large = Quat.New(1e38, 0, 0, 1e38):Normalized()
assert(math.abs(small.x - large.x) < 1e-6 and math.abs(small.w - large.w) < 1e-6)
return true
)");
		}

		TEST_CASE("QuatBindings: pure rotations work at load time and report located runtime errors")
		{
			auto sandbox = Sandbox::Create({ .Mode = SandboxMode::LoadTime, .ReadOnly = true });
			REQUIRE(sandbox.has_value());
			const auto good = (*sandbox)->Evaluate(
				"local q = Quat.Identity(); q.x = 1; return math.abs(q:Normalized().x - 0.70710678) < 1e-6", {});
			REQUIRE(good.has_value());
			CHECK(good->Value.Get() == Json(true));
			auto path = VfsPath::Parse("project://Assets/QuatErrors.luau");
			REQUIRE(path.has_value());
			CHECK_FALSE((*sandbox)->Evaluate("local q = Quat.Identity()\nq.w = 0", *path).has_value());
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Script == "Assets/QuatErrors.luau");
			CHECK(error->Line == 2);
			CHECK(error->Message.find("nonzero") != std::string::npos);
		}
	}

}
