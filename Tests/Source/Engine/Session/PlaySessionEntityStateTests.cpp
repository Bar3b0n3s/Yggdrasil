#include "TestsPCH.h"
#include "Engine/Session/PlaySession.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Support/SceneTestFixture.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

// Entity state across an unflushed disable (M35, Docs/Decisions/0050-m35-pause-entity-state.md decision 8; World Tree's
// brief 13, its first item): the brief's sequence in a play session, where a destroy flush ends every tick.

namespace Engine {

	namespace {

		// The names of the session scene's "trace:" entities, in canonical order, without the prefix.
		std::vector<std::string> SessionTrace(Scene& scene)
		{
			std::vector<std::string> names;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const Entity entity = scene.FindEntityByID(id);
				if (entity && entity.GetName().starts_with("trace:"))
					names.push_back(entity.GetName().substr(6));
			}
			return names;
		}

		// A prefab of one entity whose Script slot holds `script`, published at Assets/Brute.prefab.
		void PublishBrutePrefab(Test::ScriptTestFixture& fixture, AssetHandle handle, AssetHandle script)
		{
			Test::SceneTestFixture source;
			const Entity root = source.GetScene().CreateEntity("Brute");
			root.AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(script), .Fields = {}, .ExecutionOrder = 0 });
			const auto prefab = Prefab::CreateFromEntity(root, "Brute");
			REQUIRE(prefab.has_value());
			const auto document = prefab->ToJson();
			REQUIRE(document.has_value());
			auto data = CreateRef<PrefabData>();
			data->Document = CreateRef<const Json>(*document);
			fixture.GetAssetManager().Publish(handle, data, "Assets/Brute.prefab");
		}

	}

	TEST_SUITE("Session")
	{
		// The brief's sequence: in one fixed step a script instantiates a prefab, switches the new entity off and on again
		// and calls a method of its behaviour that writes its fields. Before M35 the destroy flush of that tick tore the
		// instance down and the next start flush started a new one on fresh fields ("start:Inactive").
		TEST_CASE("ScriptInstances: an entity instantiated, switched off and on and given a state in one tick starts with that state")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle brute(0x350101), spawner(0x350102), prefab(0x350103);
			REQUIRE(fixture.AddScript(brute, "Assets/Scripts/Brute.luau", R"(
local B = {}
function B:OnCreate()
	self.State = "Inactive"
	Scene.CreateEntity("trace:create")
end
function B:OnStart() Scene.CreateEntity("trace:start:" .. self.State) end
function B:OnDisable() Scene.CreateEntity("trace:disable") end
function B:OnEnable() Scene.CreateEntity("trace:enable") end
function B:OnDestroy() Scene.CreateEntity("trace:destroy") end
function B:Spawn(state: string) self.State = state end
return Script.Define("Brute", B)
)"));
			REQUIRE(fixture.AddScript(spawner, "Assets/Scripts/Spawner.luau", R"(
local S = {}
function S:OnFixedUpdate()
	if Time.GetTick() == 2 then
		local enemy = Scene.Instantiate(Assets.Load("Assets/Brute.prefab"))
		enemy:SetActive(false)
		enemy:SetActive(true)
		enemy:GetScript("Brute"):Spawn("Dropping")
	end
end
return Script.Define("Spawner", S)
)"));
			PublishBrutePrefab(fixture, prefab, brute);
			ScriptSlot slot{};
			slot.Script.SetHandle(spawner);
			fixture.GetScene().CreateEntity("Spawner").AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 13;
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			PlaySession& session = **started;
			session.SetExtractionEnabled(false);
			for (int tick = 0; tick < 5; ++tick)
				session.Tick();
			CHECK(SessionTrace(session.GetScene()) == std::vector<std::string>{ "create", "disable", "enable", "start:Dropping" });
			CHECK(session.GetScriptErrors().GetErrors().empty());
		}
	}

}
