#include "TestsPCH.h"
#include "Engine/Scripting/TrackingAllocator.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/Random.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scripting/Sandbox.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <format>
#include <limits>
#include <string>
#include <utility>

namespace Engine {

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Allocator: soft limit raises a script error and the VM stays usable")
		{
			Random random(17);
			SandboxSpecification specification{};
			specification.MemoryLimitMB = 1;
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Scripts/Breach.luau");
			REQUIRE(path.has_value());
			const Status breach = (*sandbox)->RunSource(*path,
				"local retained = string.rep('x', 2 * 1024 * 1024)\nwhile true do retained = retained end");
			REQUIRE_FALSE(breach.has_value());
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Kind == ScriptErrorKind::Memory);
			CHECK(error->Script == "Assets/Scripts/Breach.luau");
			CHECK(error->Message == "script exceeded memory limit 1 MB in Breach.<module top>");
			CHECK_FALSE((*sandbox)->IsStopped());
			auto healthy = VfsPath::Parse("project://Assets/Scripts/Healthy.luau");
			REQUIRE(healthy.has_value());
			CHECK((*sandbox)->RunSource(*healthy, "assert(1 + 1 == 2); return {}").has_value());
			CHECK((*sandbox)->GetMemoryState().SoftBreachCount == 1);
			// ScriptEngine's instance tests additionally assert the faulting instance receives no later callbacks.
		}

		TEST_CASE("Allocator: second breach stops the session with a structured error")
		{
			// Hosted end to end: a first breach in one behaviour disables it and recovers the VM; a second breach in
			// another behaviour on a later tick stops the session with a fatal structured memory error.
			Test::ScriptTestFixtureSpecification specification{};
			specification.Settings.MemoryLimitMB = 2;
			specification.Settings.PauseOnError = false;
			Test::ScriptTestFixture fixture(specification);
			const auto attach = [&fixture](AssetHandle handle, std::string name, std::string source)
			{
				REQUIRE(fixture.AddScript(handle, "Assets/Scripts/" + name + ".luau", std::move(source)));
				const Entity entity = fixture.GetScene().CreateEntity(name);
				entity.AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(handle), .Fields = {}, .ExecutionOrder = 0 });
				return entity.GetUUID();
			};
			const UUID first = attach(AssetHandle(301), "First", R"(local T = {}
function T:OnFixedUpdate()
	if Time.GetTick() == 1 then
		local retained = string.rep("x", 3 * 1024 * 1024)
		for _ = 1, 2 do retained = retained end
	end
end
return Script.Define("First", T))");
			const UUID second = attach(AssetHandle(302), "Second", R"(local T = {}
function T:OnFixedUpdate()
	if Time.GetTick() == 2 then
		local retained = string.rep("y", 3 * 1024 * 1024)
		for _ = 1, 2 do retained = retained end
	end
end
return Script.Define("Second", T))");
			REQUIRE(fixture.Start());
			fixture.Frame.Tick = 1;
			{
				Test::ExpectLog expected(LogLevel::Error, "script exceeded memory limit 2 MB in First.OnFixedUpdate");
				fixture.GetEngine()->FixedUpdate();
				CHECK(expected.GetMatchCount() == 1);
			}
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors[0].Kind == ScriptErrorKind::Memory);
			CHECK(fixture.Errors[0].Entity == first);
			CHECK(fixture.Errors[0].Callback == "OnFixedUpdate");
			CHECK(fixture.Errors[0].Line == 5);
			CHECK_FALSE(fixture.FatalError);
			CHECK_FALSE(fixture.GetEngine()->IsStopped());
			auto memory = fixture.GetEngine()->GetMemoryState();
			CHECK(memory.SoftBreachCount == 1);
			CHECK_FALSE(memory.NeedsRecovery);
			// Recovered: other instances and evaluations keep running; the faulting instance is disabled.
			const auto healthy = fixture.Evaluate("return 1 + 1");
			REQUIRE(healthy.has_value());
			for (const auto& instance : fixture.GetEngine()->GetInstances())
				CHECK(instance.Disabled == (instance.Entity == first));
			fixture.Frame.Tick = 2;
			{
				Test::ExpectLog expected(LogLevel::Error, "script exceeded memory limit 2 MB in Second.OnFixedUpdate");
				fixture.GetEngine()->FixedUpdate();
				CHECK(expected.GetMatchCount() == 1);
			}
			REQUIRE(fixture.Errors.size() == 2);
			const auto& stop = fixture.Errors[1];
			CHECK(stop.Kind == ScriptErrorKind::Memory);
			CHECK(stop.Script == "Assets/Scripts/Second.luau");
			CHECK(stop.Entity == second);
			CHECK(stop.Callback == "OnFixedUpdate");
			CHECK(stop.Line == 5);
			CHECK_FALSE(stop.Traceback.empty());
			CHECK(fixture.FatalError);
			CHECK(fixture.GetEngine()->IsStopped());
			memory = fixture.GetEngine()->GetMemoryState();
			CHECK(memory.SoftBreachCount == 2);
			CHECK(memory.MustStop);
			// Nothing executes after the stop.
			fixture.Frame.Tick = 3;
			fixture.GetEngine()->FixedUpdate();
			CHECK(fixture.Errors.size() == 2);
			CHECK_FALSE(fixture.Evaluate("return 1").has_value());
		}

		TEST_CASE("Allocator: a pending recovery lets nested entries share the one memory fault")
		{
			// The breach happens inside one native call that runs several callbacks (Instantiate of a prefab whose root and
			// child both have behaviours). The later OnCreate is nested in the faulting slice: it shares that memory fault
			// instead of failing with a separate runtime error that would disable an innocent instance.
			Test::ScriptTestFixtureSpecification specification{};
			specification.Settings.MemoryLimitMB = 2;
			specification.Settings.PauseOnError = false;
			Test::ScriptTestFixture fixture(specification);
			REQUIRE(fixture.AddScript(AssetHandle(311), "Assets/Scripts/Leaky.luau", "local T = {}\nfunction T:OnCreate()\n\tself.Big = string.rep('x', 3 * 1024 * 1024)\n\tfor _ = 1, 2 do end\nend\nreturn Script.Define('Leaky', T)"));
			// The innocent OnCreate calls a function before it sets its field: run inside the faulted slice, it would be aborted
			// at that call and leave a half-initialized instance with no error of its own.
			REQUIRE(fixture.AddScript(AssetHandle(312), "Assets/Scripts/Innocent.luau", "local T = {}\nlocal function grid(n) return table.create(n, 0) end\nfunction T:OnCreate() self.Grid = grid(10) end\nreturn Script.Define('Innocent', T)"));
			{
				Test::SceneTestFixture source;
				const Entity root = source.GetScene().CreateEntity("LeakyRoot");
				const Entity child = source.GetScene().CreateEntity("InnocentChild", root);
				root.AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(AssetHandle(311)), .Fields = {}, .ExecutionOrder = 0 });
				child.AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(AssetHandle(312)), .Fields = {}, .ExecutionOrder = 0 });
				const auto prefab = Prefab::CreateFromEntity(root, "LeakyPrefab");
				REQUIRE(prefab.has_value());
				const auto document = prefab->ToJson();
				REQUIRE(document.has_value());
				auto data = CreateRef<PrefabData>();
				data->Document = CreateRef<const Json>(*document);
				fixture.GetAssetManager().Publish(AssetHandle(313), data, "Assets/Prefabs/Leaky.prefab");
			}
			REQUIRE(fixture.Start());
			{
				Test::ExpectLog expected(LogLevel::Error, "script exceeded memory limit 2 MB");
				CHECK_FALSE(fixture.Evaluate("Scene.Instantiate(Assets.Load('Assets/Prefabs/Leaky.prefab'))").has_value());
				CHECK(expected.GetMatchCount() == 1);
			}
			// One fault, one published error, attributed to the faulting OnCreate; no "memory recovery" runtime error.
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors[0].Kind == ScriptErrorKind::Memory);
			CHECK(fixture.Errors[0].Script == "Assets/Scripts/Leaky.luau");
			CHECK(fixture.Errors[0].Callback == "OnCreate");
			CHECK_FALSE(fixture.GetEngine()->IsStopped());
			CHECK_FALSE(fixture.GetEngine()->GetMemoryState().NeedsRecovery);
			bool foundInnocent = false;
			for (const auto& instance : fixture.GetEngine()->GetInstances())
			{
				const auto name = fixture.GetScene().FindEntityByID(instance.Entity).GetName();
				CAPTURE(name);
				CHECK(instance.Disabled == (name == "LeakyRoot"));
				foundInnocent = foundInnocent || name == "InnocentChild";
			}
			CHECK(foundInnocent);
			// The innocent OnCreate did not run inside the faulted slice; the next synchronization after recovery runs it whole.
			const auto pending = fixture.Evaluate("return Scene.FindByName('InnocentChild'):GetScript().Grid == nil");
			REQUIRE(pending.has_value());
			CHECK(pending->Value.Get() == Json(true));
			REQUIRE(fixture.GetEngine()->SynchronizeInstances().has_value());
			const auto created = fixture.Evaluate("local grid = Scene.FindByName('InnocentChild'):GetScript().Grid; return grid ~= nil and #grid == 10");
			REQUIRE_MESSAGE(created.has_value(), (created ? "" : created.error().ToString()));
			CHECK(created->Value.Get() == Json(true));
			CHECK(fixture.Errors.size() == 1);
			// It runs once: a later synchronization finds nothing deferred.
			REQUIRE(fixture.Evaluate("Scene.FindByName('InnocentChild'):GetScript().Grid = 'kept'").has_value());
			REQUIRE(fixture.GetEngine()->SynchronizeInstances().has_value());
			const auto kept = fixture.Evaluate("return Scene.FindByName('InnocentChild'):GetScript().Grid == 'kept'");
			REQUIRE(kept.has_value());
			CHECK(kept->Value.Get() == Json(true));
		}

		TEST_CASE("TrackingAllocator: accounting ignores new-allocation tags and counts a pending breach once")
		{
			auto allocator = TrackingAllocator::Create(1);
			REQUIRE(allocator.has_value());
			constexpr size_t Soft = 1024 * 1024;
			CHECK((*allocator)->GetState().HardLimitBytes == Soft + TrackingAllocator::HeadroomBytes);
			void* block = (*allocator)->Reallocate(nullptr, 123, Soft + 1);
			REQUIRE(block != nullptr);
			CHECK((*allocator)->GetState().UsedBytes == Soft + 1);
			CHECK((*allocator)->CheckInterrupt(0) == ScriptMemoryAction::Continue);
			CHECK((*allocator)->CheckInterrupt(3) == ScriptMemoryAction::Continue);
			CHECK((*allocator)->CheckInterrupt(-1) == ScriptMemoryAction::RecoverInstance);
			CHECK((*allocator)->CheckInterrupt(-1) == ScriptMemoryAction::RecoverInstance);
			CHECK((*allocator)->GetState().SoftBreachCount == 1);
			CHECK((*allocator)->Reallocate(block, Soft + 1, 0) == nullptr);
			REQUIRE((*allocator)->FinishRecovery().has_value());
			CHECK((*allocator)->GetState().UsedBytes == 0);
			CHECK((*allocator)->GetState().PeakBytes == Soft + 1);
			block = (*allocator)->Reallocate(nullptr, 0, Soft + 1);
			REQUIRE(block != nullptr);
			CHECK((*allocator)->CheckInterrupt(-1) == ScriptMemoryAction::StopSession);
			CHECK((*allocator)->Reallocate(block, Soft + 1, 0) == nullptr);
		}

		TEST_CASE("TrackingAllocator: reaching hard limit refuses growth and keeps the old block accounted")
		{
			auto allocator = TrackingAllocator::Create(1);
			REQUIRE(allocator.has_value());
			void* block = (*allocator)->Reallocate(nullptr, 0, 32);
			REQUIRE(block != nullptr);
			const size_t hard = static_cast<size_t>((*allocator)->GetState().HardLimitBytes);
			CHECK((*allocator)->Reallocate(block, 32, hard) == nullptr);
			CHECK((*allocator)->GetState().UsedBytes == 32);
			CHECK((*allocator)->GetState().MustStop);
			CHECK((*allocator)->CheckInterrupt(0) == ScriptMemoryAction::Continue);
			CHECK((*allocator)->CheckInterrupt(-1) == ScriptMemoryAction::StopSession);
			CHECK((*allocator)->Reallocate(block, 32, 0) == nullptr);
		}

		TEST_CASE("TrackingAllocator: failed recovery and size overflow latch terminal memory failure")
		{
			auto allocator = TrackingAllocator::Create(1);
			REQUIRE(allocator.has_value());
			constexpr size_t BreachSize = 1024 * 1024 + 1;
			void* block = (*allocator)->Reallocate(nullptr, 0, BreachSize);
			REQUIRE(block != nullptr);
			CHECK_FALSE((*allocator)->FinishRecovery().has_value());
			CHECK((*allocator)->GetState().MustStop);
			CHECK((*allocator)->Reallocate(block, BreachSize, 0) == nullptr);
			auto overflow = TrackingAllocator::Create(1);
			REQUIRE(overflow.has_value());
			CHECK((*overflow)->Reallocate(nullptr, 0, std::numeric_limits<size_t>::max()) == nullptr);
			CHECK((*overflow)->GetState().UsedBytes == 0);
			CHECK((*overflow)->GetState().MustStop);
			const auto invalid = TrackingAllocator::Create(0);
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
