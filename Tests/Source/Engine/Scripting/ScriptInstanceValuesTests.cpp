#include "TestsPCH.h"

#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// ScriptEngine::ReadInstanceValues (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 14, W24.2's stream B): the
// graph editor's live values. Skipped until stream B implements it.

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

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptEngine: reading an instance's values between phases runs no Luau and allocates nothing in the VM" * doctest::skip(true))
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
	}

}
