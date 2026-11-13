#include "TestsPCH.h"

#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/ParticleEmitterComponent.h"
#include "Engine/Scene/Components/ParticleStateComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/ParticleSimulation.h"
#include "Engine/Scene/ParticleSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptTestHost.h"
#include "Support/ScriptTestFixture.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The particle script surface (M26, C-PART; Docs/Decisions/0037-m26-contract.md decision 26, stream P-A): ParticleEmitter:Emit,
// Clear and GetParticleCount, which reach the emitter's runtime state, and Test.GetParticleDigest, the seeded particle hash a
// suite compares across runs. The Particles phase that spawns what Emit queues is ParticleSystem::Step. The count is refused on
// the fixed step, whose result it would make depend on frame pacing (W26.1's review).

namespace Engine {

	namespace {

		// A test host that only collects and runs a suite's cases, keeping the script errors it is told of.
		class ParticleCaseHost final : public IScriptTestHost
		{
		public:
			void Report(const ScriptTestReport& /*report*/) override {}
			void OnScriptError(const ScriptError& error, bool /*fatal*/) override { Errors.push_back(error); }
			void OnExpectedScriptError(const ScriptError& /*error*/) override {}
			Status InjectInput(const Json& /*event*/) override { return {}; }
			Status CaptureScreenshot(std::string_view /*name*/) override { return {}; }
			Status BeginAudioCapture(uint32_t /*ticks*/) override { return {}; }
			bool IsAudioCaptureReady() const override { return false; }
			Result<AudioLevels> EndAudioCapture() override { return MakeError(ErrorCode::Unsupported, "No audio in this case fixture"); }
			void CancelAudioCapture() override {}
			Status ReloadScript(AssetHandle /*script*/) override { return MakeError(ErrorCode::Unsupported, "No editor in this case fixture"); }
			uint64_t ComputeStateHash() const override { return 0; }
			ScriptExtractionState GetLastExtraction() const override { return {}; }
			std::optional<glm::vec3> GetExtractedPosition(UUID /*entity*/) const override { return std::nullopt; }
			void OnDebugBreak() override {}

			std::vector<ScriptError> Errors{};
		};

		void CheckParticleScript(Test::ScriptTestFixture& fixture, std::string_view source)
		{
			const Result<ScriptEvaluation> result = fixture.Evaluate(source);
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
		}

		// The acceptance case's work in `mode`, whose API coverage it adds to `snapshots` ("ScriptApi: every registered
		// function, method, property, operator and constructor has a test").
		void ParticleCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			Scene& scene = fixture.GetScene();
			Entity sparks = scene.CreateEntity("Sparks");
			sparks.AddComponent<ParticleEmitterComponent>(ParticleEmitterComponent{ .Rate = 0.0f, .MaxParticles = 64, .Seed = 3 });
			static_cast<void>(scene.CreateEntity("Plain"));
			REQUIRE(fixture.Start());
			const uint64_t revision = scene.GetRevision();

			// Emit queues for the next Particles phase (nothing is alive yet, and no digest before the phase anchors the state);
			// a count outside 1 to 4096, or not a whole number, raises and queues nothing.
			CheckParticleScript(fixture, R"(
local entity = Scene.FindByName("Sparks")
local sparks = entity.ParticleEmitter
assert(sparks:GetParticleCount() == 0)
assert(Test.GetParticleDigest(entity) == nil)
assert(Test.GetParticleDigest(Scene.FindByName("Plain")) == nil)
sparks:Emit(5)
sparks:Emit(2)
for _, count in {0, 4097, -1, 1.5} do
	assert(not pcall(function() sparks:Emit(count) end))
end
assert(sparks:GetParticleCount() == 0)
assert(Test.GetParticleDigest(entity) == nil)
return true
)");
			REQUIRE(sparks.HasComponent<ParticleStateComponent>());
			CHECK(sparks.GetComponent<ParticleStateComponent>().State.PendingEmits == 7);
			CHECK_FALSE(sparks.GetComponent<ParticleStateComponent>().LastTick.has_value());
			// Presentation only: no simulated state changed, so neither the revision nor an external mutation is reported.
			CHECK(scene.GetRevision() == revision);
			CHECK(fixture.ExternalMutations.empty());

			// The phase spawns the queued particles; the digest is ComputeParticleDigest of the state, and Clear empties it.
			ParticleSystem particles(1.0f / 60.0f);
			particles.Step(scene, 1);
			REQUIRE(sparks.GetComponent<ParticleStateComponent>().State.Particles.size() == 7);
			const uint64_t digest = ComputeParticleDigest(sparks.GetComponent<ParticleStateComponent>().State);
			CheckParticleScript(fixture, std::format(R"(
local entity = Scene.FindByName("Sparks")
assert(entity.ParticleEmitter:GetParticleCount() == 7)
assert(Test.GetParticleDigest(entity) == "{:016x}")
entity.ParticleEmitter:Emit(3)
entity.ParticleEmitter:Clear()
assert(entity.ParticleEmitter:GetParticleCount() == 0)
local gone = Scene.CreateEntity("Gone")
gone:AddComponent("ParticleEmitter")
gone:Destroy()
assert(Test.GetParticleDigest(gone) == nil)
return true
)",
											 digest));
			CHECK(sparks.GetComponent<ParticleStateComponent>().State.PendingEmits == 0);
			CHECK(sparks.GetComponent<ParticleStateComponent>().State.Particles.empty());
			CHECK(fixture.Errors.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	namespace Test {

		void RunParticlesBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			ParticleCoverage(mode, snapshots);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptParticles: Emit queues particles for the next phase, Clear kills them, and the count and digest read them")
		{
			std::vector<ScriptApiCoverage> snapshots;
			ParticleCoverage(RunModes::Editor, snapshots);
		}

		// Emit and Clear write the emitter's runtime state, so a read-only evaluation (an edit scene's, script.eval outside
		// play) refuses them and edit scenes never hold particles; reading the count is allowed.
		TEST_CASE("ScriptParticles: a read-only evaluation refuses Emit and Clear and reads the count")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			Entity sparks = fixture.GetScene().CreateEntity("Sparks");
			sparks.AddComponent<ParticleEmitterComponent>();
			REQUIRE(fixture.Start());
			CheckParticleScript(fixture, R"(
local sparks = Scene.FindByName("Sparks").ParticleEmitter
assert(not pcall(function() sparks:Emit(1) end))
assert(not pcall(function() sparks:Clear() end))
assert(sparks:GetParticleCount() == 0)
return true
)");
			CHECK_FALSE(sparks.HasComponent<ParticleStateComponent>());
		}

		// W26.1's review (decision 26): the Particles phase advances the particles in the frame phase, so a count read on the
		// fixed step would depend on how frames grouped the ticks, and gameplay that branched on it on the state hash.
		// GetParticleCount raises in gameplay code of the fixed step (OnFixedUpdate, and a task resumed there), naming the
		// callbacks that may read it, and reads in the frame phase's OnUpdate and OnLateUpdate, and in a test case whatever
		// phase it runs in.
		TEST_CASE("ScriptParticles: GetParticleCount raises in a fixed-step callback and reads in the frame phase and in test cases")
		{
			ParticleCaseHost host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const AssetHandle reader(0x260001);
			const AssetHandle suite(0x260002);
			REQUIRE(fixture.AddScript(reader, "Assets/Scripts/Reader.luau", R"(local T = {}
local function Read(self, phase)
	local ok, result = pcall(function() return self.Entity.ParticleEmitter:GetParticleCount() end)
	if ok then
		table.insert(self.Reads, phase .. ":" .. tostring(result))
	elseif string.find(tostring(result), "OnUpdate", 1, true) then
		table.insert(self.Reads, phase .. ":refused")
	else
		table.insert(self.Reads, phase .. ":" .. tostring(result))
	end
end
function T:OnStart()
	self.Reads = {}
	Task.Spawn(function()
		Task.WaitTicks(1)
		Read(self, "task")
	end)
end
function T:OnFixedUpdate() Read(self, "fixed") end
function T:OnUpdate() Read(self, "update") end
function T:OnLateUpdate() Read(self, "late") end
return Script.Define("Reader", T))"));
			REQUIRE(fixture.AddScript(suite, "Assets/Tests/Particles.luau", R"(return Test.Suite("Particles", function()
	Test.Case("Count", function()
		assert(Scene.FindByName("Sparks").ParticleEmitter:GetParticleCount() == 7)
	end)
end))"));
			Scene& scene = fixture.GetScene();
			const Entity sparks = scene.CreateEntity("Sparks");
			sparks.AddComponent<ParticleEmitterComponent>(ParticleEmitterComponent{ .Rate = 0.0f, .MaxParticles = 64, .Seed = 3 });
			ScriptSlot slot{};
			slot.Script.SetHandle(reader);
			sparks.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
			REQUIRE(fixture.Start());
			REQUIRE(QueueParticleEmit(sparks, 7).has_value());
			ParticleSystem particles(1.0f / 60.0f);
			particles.Step(scene, 1);
			REQUIRE(sparks.GetComponent<ParticleStateComponent>().State.Particles.size() == 7);

			// Tick 1's fixed step (OnFixedUpdate, then the task that waited for it), then the frame phase.
			ScriptEngine& engine = *fixture.GetEngine();
			fixture.Frame.Phase = InputPhase::Step;
			fixture.Frame.Tick = 1;
			engine.FixedUpdate();
			engine.ResumeTasks();
			fixture.Frame.Phase = InputPhase::Frame;
			engine.Update();
			engine.LateUpdate();
			const Result<ScriptEvaluation> reads = fixture.Evaluate("return self.Reads", sparks.GetUUID());
			REQUIRE_MESSAGE(reads.has_value(), (reads ? std::string() : reads.error().ToString()));
			CHECK(reads->Value.Get() == Json::array({ "fixed:refused", "task:refused", "update:7", "late:7" }));
			CHECK(fixture.Errors.empty());

			// A test case reads it on the fixed step too: suite code is not the simulation's.
			fixture.Frame.Phase = InputPhase::Step;
			const Result<ScriptCollectedSuite> collected = engine.CollectSuite(suite, host);
			REQUIRE_MESSAGE(collected.has_value(), (collected ? std::string() : collected.error().ToString()));
			REQUIRE(collected->Cases.size() == 1);
			const Result<ScriptReference> thread = engine.StartCase(collected->Cases.front().Function);
			REQUIRE_MESSAGE(thread.has_value(), (thread ? std::string() : thread.error().ToString()));
			const Result<ScriptCaseResume> ran = engine.ResumeCase(*thread);
			REQUIRE_MESSAGE(ran.has_value(), (ran ? std::string() : ran.error().ToString()));
			CHECK(ran->State == ScriptCaseState::Completed);
			CHECK_FALSE(ran->Error.has_value());
			engine.CancelCase(*thread);
			CHECK(host.Errors.empty());
		}
	}

}
