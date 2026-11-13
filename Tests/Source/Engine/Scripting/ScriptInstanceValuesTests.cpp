#include "TestsPCH.h"

#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ScriptEngine::ReadInstanceValues (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 14, W24.2's stream B): the
// graph editor's live values, read between phases without running Luau or allocating in the VM.

namespace Engine {

	namespace {

		// A behaviour shaped as the transpiler writes one: its OnCreate makes the live-value table, and each fixed step writes
		// two values there, as a graph's nodes do.
		constexpr std::string_view CounterSource = R"(--!strict
local Counter = {}
Counter.Fields = {
	Step = Field.Number(2),
}
function Counter.OnCreate(self: any)
	self.__GraphLive = {}
	self.Opened = false
	self.Count = 0
end
function Counter.OnFixedUpdate(self: any, dt: number)
	self.Count += 1
	self.__GraphLive["4.Result"] = self.Count * self.Step
	self.__GraphLive["5.Value"] = vector.create(1, 2, 3)
end
return Script.Define("Counter", Counter)
)";

		const ScriptInstanceValue* FindValue(const std::vector<ScriptInstanceValue>& values, std::string_view key)
		{
			const auto found = std::ranges::find(values, key, &ScriptInstanceValue::Key);
			return found != values.end() ? &*found : nullptr;
		}

		// A behaviour whose OnCreate gives its instance and its live-value table one value of every kind the read spells.
		constexpr std::string_view KindsSource = R"(--!strict
local Kinds = {}
function Kinds.OnCreate(self: any)
	self.Text = "door"
	self.Broken = "\255"
	self.Ratio = 0.25
	self.NotANumber = 0 / 0
	self.Huge = math.huge
	self.Direction = vector.create(1, 0.5, -2)
	self.Tint = Color.New(1, 0.5, 0, 1)
	self.Turn = Quat.New(0, 0, 0, 1)
	self.Itself = Assets.Load("6a7a000000000003")
	self.Place = self.Entity.Transform
	self.List = { 1, 2 }
	self.Callback = function() end
	self[1] = "a number key"
	self[true] = "a boolean key"
	self.__GraphLive = {}
	self.__GraphLive["7.Result"] = self.Direction
	self.__GraphLive["8.Value"] = self.Tint
	self.__GraphLive[9] = "a number key"
end
return Script.Define("Kinds", Kinds)
)";

		// A behaviour whose live-value table is no table, and one with more live values than a list holds.
		constexpr std::string_view NoTableSource = R"(--!strict
local NoTable = {}
function NoTable.OnCreate(self: any)
	self.__GraphLive = 5
end
return Script.Define("NoTable", NoTable)
)";

		std::string MakeManySource()
		{
			return std::format(R"(--!strict
local Many = {{}}
function Many.OnCreate(self: any)
	self.__GraphLive = {{}}
	for index = 0, {} do
		self.__GraphLive[string.format("%05d.Value", index)] = index
	end
end
return Script.Define("Many", Many)
)",
				MaxScriptInstanceValues + 9);
		}

		// A behaviour that asks the host for an entity at each fixed step, so the host's request runs inside a script call.
		constexpr std::string_view SpawnerSource = R"(--!strict
local Spawner = {}
function Spawner.OnFixedUpdate(self: any, dt: number)
	Scene.CreateEntity("Spawned")
end
return Script.Define("Spawner", Spawner)
)";

		void AttachScript(Test::ScriptTestFixture& fixture, UUID id, std::string_view name, AssetHandle script)
		{
			Entity entity = fixture.GetScene().CreateEntityWithID(id, std::string(name));
			ScriptComponent component;
			component.Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(script), .Fields = {}, .ExecutionOrder = 0 });
			entity.AddComponent<ScriptComponent>(component);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptEngine: reading an instance's values between phases runs no Luau and allocates nothing in the VM")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle counter(0x6a7a000000000002ull);
			REQUIRE(fixture.AddScript(counter, "Assets/Scripts/Counter.luau", std::string(CounterSource)));
			Entity entity = fixture.GetScene().CreateEntityWithID(UUID(10), "Door");
			ScriptComponent component;
			component.Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(counter), .Fields = {}, .ExecutionOrder = 0 });
			entity.AddComponent<ScriptComponent>(component);
			REQUIRE(fixture.Start());
			ScriptEngine& engine = *fixture.GetEngine();
			engine.FixedUpdate();
			engine.FixedUpdate();

			const ScriptMemoryState before = engine.GetMemoryState();
			const Result<ScriptInstanceValues> values = engine.ReadInstanceValues(UUID(10), 0);
			REQUIRE_MESSAGE(values.has_value(), values.error().ToString());
			// Nothing ran and nothing was allocated in the VM, so the session goes on as it would have.
			CHECK(engine.GetMemoryState().UsedBytes == before.UsedBytes);
			CHECK_FALSE(values->Truncated);

			// The instance's own members, byte-wise: its Entity, its field, its state and the live-value table itself.
			std::vector<std::string> keys;
			for (const ScriptInstanceValue& member : values->Members)
				keys.push_back(member.Key);
			CHECK(keys == std::vector<std::string>{ "Count", "Entity", "Opened", "Step", "__GraphLive" });
			CHECK(FindValue(values->Members, "Count")->Value == VariantValue(Json(2)));
			CHECK(FindValue(values->Members, "Count")->TypeName == "number");
			CHECK(FindValue(values->Members, "Opened")->Value == VariantValue(Json(false)));
			CHECK(FindValue(values->Members, "Entity")->TypeName == "Entity");
			CHECK(FindValue(values->Members, "Entity")->Value == VariantValue(Json(UUID(10).ToString())));
			CHECK(FindValue(values->Members, "__GraphLive")->TypeName == "table");
			CHECK(FindValue(values->Members, "__GraphLive")->Value.IsNull());

			// The live values, keyed as the transpiler keys them.
			REQUIRE(values->LiveValues.size() == 2);
			CHECK(FindValue(values->LiveValues, "4.Result")->Value == VariantValue(Json(4)));
			CHECK(FindValue(values->LiveValues, "5.Value")->TypeName == "vector");
			CHECK(FindValue(values->LiveValues, "5.Value")->Value == VariantValue(Json::array({ 1, 2, 3 })));

			// The session goes on unchanged: the next step counts on.
			engine.FixedUpdate();
			const Result<ScriptInstanceValues> after = engine.ReadInstanceValues(UUID(10), 0);
			REQUIRE(after.has_value());
			CHECK(FindValue(after->Members, "Count")->Value == VariantValue(Json(3)));

			// No instance is (entity, slot 1).
			CHECK(engine.ReadInstanceValues(UUID(10), 1).error().GetCode() == ErrorCode::NotFound);
			CHECK(engine.ReadInstanceValues(UUID(99), 0).error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("ScriptEngine: an instance's values read in reflection's JSON spelling with their type names, and string keys alone count")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle kinds(0x6a7a000000000003ull);
			REQUIRE(fixture.AddScript(kinds, "Assets/Scripts/Kinds.luau", std::string(KindsSource)));
			AttachScript(fixture, UUID(20), "Kinds", kinds);
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Errors.empty());
			ScriptEngine& engine = *fixture.GetEngine();

			const ScriptMemoryState before = engine.GetMemoryState();
			const Result<ScriptInstanceValues> values = engine.ReadInstanceValues(UUID(20), 0);
			REQUIRE_MESSAGE(values.has_value(), values.error().ToString());
			CHECK(engine.GetMemoryState().UsedBytes == before.UsedBytes);

			// Every string-keyed member, byte-wise ("Broken" before "Callback", "__GraphLive" after the capitals); the number
			// and boolean keys are left out.
			std::vector<std::string> keys;
			for (const ScriptInstanceValue& member : values->Members)
				keys.push_back(member.Key);
			CHECK(keys
				== std::vector<std::string>{ "Broken", "Callback", "Direction", "Entity", "Huge", "Itself", "List", "NotANumber", "Place", "Ratio", "Text",
					"Tint", "Turn", "__GraphLive" });
			const auto member = [&values](std::string_view key)
			{
				const ScriptInstanceValue* found = FindValue(values->Members, key);
				REQUIRE_MESSAGE(found != nullptr, std::string(key));
				return *found;
			};
			CHECK(member("Text").TypeName == "string");
			CHECK(member("Text").Value == VariantValue(Json("door")));
			CHECK(member("Ratio").TypeName == "number");
			CHECK(member("Ratio").Value == VariantValue(Json(0.25)));
			CHECK(member("Direction").TypeName == "vector");
			CHECK(member("Direction").Value == VariantValue(Json::array({ 1.0, 0.5, -2.0 })));
			CHECK(member("Tint").TypeName == "Color");
			CHECK(member("Tint").Value == VariantValue(Json::array({ 1.0, 0.5, 0.0, 1.0 })));
			CHECK(member("Turn").TypeName == "Quat");
			CHECK(member("Turn").Value == VariantValue(Json::array({ 0.0, 0.0, 0.0, 1.0 })));
			CHECK(member("Itself").TypeName == "AssetRef");
			CHECK(member("Itself").Value == VariantValue(Json(kinds.ToString())));
			CHECK(member("Entity").TypeName == "Entity");
			CHECK(member("Entity").Value == VariantValue(Json(UUID(20).ToString())));
			// A value of another type, a string that is not UTF-8 and a non-finite number: named, with no value.
			CHECK(member("Place").TypeName == "Transform");
			CHECK(member("List").TypeName == "table");
			CHECK(member("Callback").TypeName == "function");
			CHECK(member("Broken").TypeName == "string");
			CHECK(member("NotANumber").TypeName == "number");
			CHECK(member("Huge").TypeName == "number");
			for (const std::string_view key : { "Place", "List", "Callback", "Broken", "NotANumber", "Huge", "__GraphLive" })
				CHECK_MESSAGE(member(key).Value.IsNull(), std::string(key));

			// The live values, string keys alone.
			REQUIRE(values->LiveValues.size() == 2);
			CHECK(values->LiveValues[0].Key == "7.Result");
			CHECK(values->LiveValues[0].Value == VariantValue(Json::array({ 1.0, 0.5, -2.0 })));
			CHECK(values->LiveValues[1].Key == "8.Value");
			CHECK(values->LiveValues[1].TypeName == "Color");
			CHECK_FALSE(values->Truncated);
		}

		TEST_CASE("ScriptEngine: a live-value table that is no table gives no live values, and a list keeps its byte-wise first keys up to its limit")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle noTable(0x6a7a000000000004ull);
			const AssetHandle many(0x6a7a000000000005ull);
			REQUIRE(fixture.AddScript(noTable, "Assets/Scripts/NoTable.luau", std::string(NoTableSource)));
			REQUIRE(fixture.AddScript(many, "Assets/Scripts/Many.luau", MakeManySource()));
			AttachScript(fixture, UUID(30), "NoTable", noTable);
			AttachScript(fixture, UUID(31), "Many", many);
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Errors.empty());
			ScriptEngine& engine = *fixture.GetEngine();

			const Result<ScriptInstanceValues> plain = engine.ReadInstanceValues(UUID(30), 0);
			REQUIRE(plain.has_value());
			CHECK(plain->LiveValues.empty());
			REQUIRE(plain->Members.size() == 2); // Entity and the member that is no table
			CHECK(plain->Members[1].Key == "__GraphLive");
			CHECK(plain->Members[1].Value == VariantValue(Json(5)));
			CHECK_FALSE(plain->Truncated);

			const ScriptMemoryState before = engine.GetMemoryState();
			const Result<ScriptInstanceValues> cut = engine.ReadInstanceValues(UUID(31), 0);
			REQUIRE(cut.has_value());
			CHECK(engine.GetMemoryState().UsedBytes == before.UsedBytes);
			CHECK(cut->Truncated);
			REQUIRE(cut->LiveValues.size() == MaxScriptInstanceValues);
			CHECK(cut->LiveValues.front().Key == "00000.Value");
			CHECK(cut->LiveValues.back().Key == std::format("{:05}.Value", MaxScriptInstanceValues - 1));
			CHECK(cut->LiveValues.back().Value == VariantValue(Json(MaxScriptInstanceValues - 1)));
			CHECK(std::ranges::is_sorted(cut->LiveValues, {}, &ScriptInstanceValue::Key));
		}

		TEST_CASE("ScriptEngine: an instance's values are not read inside a script call or after the engine stops")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle spawner(0x6a7a000000000006ull);
			REQUIRE(fixture.AddScript(spawner, "Assets/Scripts/Spawner.luau", std::string(SpawnerSource)));
			AttachScript(fixture, UUID(40), "Spawner", spawner);
			REQUIRE(fixture.Start());
			ScriptEngine& engine = *fixture.GetEngine();
			std::optional<Result<ScriptInstanceValues>> inside;
			fixture.OnHostRequest = [&engine, &inside](std::string_view)
			{
				inside = engine.ReadInstanceValues(UUID(40), 0);
			};
			engine.FixedUpdate();
			fixture.OnHostRequest = {};
			REQUIRE(inside.has_value());
			REQUIRE_FALSE(inside->has_value());
			CHECK(inside->error().GetCode() == ErrorCode::InvalidState);
			CHECK(fixture.Errors.empty());

			// Between phases the same instance reads, and after Stop nothing does.
			CHECK(engine.ReadInstanceValues(UUID(40), 0).has_value());
			engine.Stop();
			CHECK(engine.ReadInstanceValues(UUID(40), 0).error().GetCode() == ErrorCode::InvalidState);
		}
	}

}
