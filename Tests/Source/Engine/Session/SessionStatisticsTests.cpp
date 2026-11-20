#include "TestsPCH.h"
#include "Engine/Session/SessionStatistics.h"

#include "Engine/Renderer/RenderStats.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 4): the performance readout. The session's counters
// (SessionStatistics, moved out of Automation/ so that Dist has them), the render part a host adds from its view's
// RenderStats, and Application.GetStats, frame-paced like ParticleEmitter:GetParticleCount.

namespace Engine {

	namespace {

		// A host that reports fixed frame and render observations and keeps every script error.
		class ReadoutHost final : public IPlaySessionHost
		{
		public:
			ScriptEnvironment GetScriptEnvironment() const override { return {}; }
			Status SetScriptCursorMode(CursorMode) override { return {}; }
			CursorMode GetScriptCursorMode() const override { return CursorMode::Normal; }
			void OnScriptError(const ScriptError& error, bool /*fatal*/) override { Errors.push_back(error); }
			void OnScriptPhase(std::string_view, std::string_view) override {}
			void ReadFrameStatistics(ScriptPerformanceStats& stats) const override
			{
				++Reads;
				stats.Fps = 30.0;
				stats.CpuMilliseconds = 12.5;
				AddRenderStatistics(stats, View);
			}
			RenderStats View{};
			mutable uint32_t Reads = 0;
			std::vector<ScriptError> Errors{};
		};

		std::vector<std::string> TraceNames(Scene& scene)
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

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("SessionStatistics: AddRenderStatistics sums the view's passes and copies its light and skin counters")
		{
			RenderStats view;
			ScriptPerformanceStats stats;
			AddRenderStatistics(stats, view);
			CHECK_FALSE(stats.RenderAvailable);
			CHECK(stats.DrawCalls == 0);

			view.Available = true;
			view.GpuAvailable = true;
			view.VisibleLights = 256;
			view.DroppedLights = 44;
			view.ShadowedSpotLights = 3;
			view.ShadowedPointLights = 2;
			view.SkinnedMeshes = 5;
			view.PlanarReflections = 3; // M42
			view.DroppedPlanarReflections = 2;
			view.Passes.push_back({ .Name = "Forward", .CpuMilliseconds = 1.0, .GpuMilliseconds = 2.0, .GpuAvailable = true, .DrawCalls = 10, .Dispatches = 0, .Triangles = 300 });
			view.Passes.push_back({ .Name = "Shadows", .CpuMilliseconds = 1.0, .GpuMilliseconds = 0.5, .GpuAvailable = false, .DrawCalls = 4, .Dispatches = 0, .Triangles = 120 });
			view.Passes.push_back({ .Name = "Skinning", .CpuMilliseconds = 1.0, .GpuMilliseconds = 0.25, .GpuAvailable = true, .DrawCalls = 0, .Dispatches = 5, .Triangles = 0 });
			AddRenderStatistics(stats, view);
			CHECK(stats.RenderAvailable);
			CHECK(stats.GpuAvailable);
			CHECK(stats.VisibleLights == 256);
			CHECK(stats.DroppedLights == 44);
			CHECK(stats.ShadowedLights == 5);
			CHECK(stats.SkinnedMeshes == 5);
			CHECK(stats.PlanarReflections == 3);
			CHECK(stats.DroppedPlanarReflections == 2);
			CHECK(stats.DrawCalls == 14);
			CHECK(stats.Triangles == 420);
			// Only the passes with a GPU sample add to the GPU time.
			CHECK(stats.GpuMilliseconds == doctest::Approx(2.25));
			// M29's light rows and M32's SSR row (ADR 0042's phase-5 ledger, carried at M28's landing): the cap and the cached
			// shadows always, the SSR area only while a completed frame recorded it.
			CHECK(stats.MaxShadedLights == 0);
			CHECK(stats.SsrTiles == 0);
		}

		TEST_CASE("SessionStatistics: AddRenderStatistics copies M29's light rows and M32's SSR area while it is available")
		{
			RenderStats view;
			view.Available = true;
			view.MaxShadedLights = 1024;
			view.CachedPointShadows = 3;
			view.CachedSpotShadows = 2;
			view.SsrTiles = 120;
			view.SsrTracedTiles = 30;
			view.SsrTracedPixels = 1500;
			ScriptPerformanceStats stale;
			AddRenderStatistics(stale, view);
			CHECK(stale.MaxShadedLights == 1024);
			CHECK(stale.CachedPointShadows == 3);
			CHECK(stale.CachedSpotShadows == 2);
			// No completed frame recorded the SSR area: its counts stay 0.
			CHECK(stale.SsrTiles == 0);
			CHECK(stale.SsrTracedTiles == 0);
			CHECK(stale.SsrTracedPixels == 0);

			view.SsrAreaAvailable = true;
			ScriptPerformanceStats stats;
			AddRenderStatistics(stats, view);
			CHECK(stats.SsrTiles == 120);
			CHECK(stats.SsrTracedTiles == 30);
			CHECK(stats.SsrTracedPixels == 1500);
		}

		TEST_CASE("PlaySession: Application.GetStats reads the host's frame statistics and the session's counters outside the fixed step only")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle script(0x7a2801);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Readout.luau", R"(
local Readout = {}
function Readout:OnFixedUpdate()
	if self.FixedDone then return end
	self.FixedDone = true
	local ok, message = pcall(Application.GetStats)
	Scene.CreateEntity("trace:fixed:" .. (if ok then "read" elseif string.find(tostring(message), "fixed-step", 1, true) then "refused" else tostring(message)))
end
function Readout:OnUpdate()
	if self.FrameDone then return end
	self.FrameDone = true
	local stats = Application.GetStats()
	Scene.CreateEntity(string.format("trace:frame:%s:%d:%d:%d:%d:%d:%d:%d:%d", tostring(stats.RenderAvailable), stats.Fps, stats.DrawCalls,
		stats.Triangles, stats.VisibleLights, stats.DroppedLights, stats.Bodies, stats.Entities, if stats.ScriptHeapBytes > 0 then 1 else 0))
end
return Script.Define("Readout", Readout)
)"));
			Entity probe = fixture.GetScene().CreateEntity("Probe");
			ScriptSlot slot{};
			slot.Script.SetHandle(script);
			probe.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
			Entity crate = fixture.GetScene().CreateEntity("Crate");
			RigidBodyComponent body;
			body.Type = BodyType::Static;
			crate.AddComponent<RigidBodyComponent>(body);
			crate.AddComponent<BoxColliderComponent>();

			ReadoutHost host;
			host.View.Available = true;
			host.View.VisibleLights = 7;
			host.View.DroppedLights = 2;
			host.View.Passes.push_back({ .Name = "Forward", .CpuMilliseconds = 1.0, .GpuMilliseconds = 1.0, .GpuAvailable = false, .DrawCalls = 9, .Dispatches = 0, .Triangles = 600 });
			host.View.MaxShadedLights = 512;
			host.View.CachedPointShadows = 4;
			host.View.CachedSpotShadows = 6;
			host.View.SsrAreaAvailable = true;
			host.View.SsrTiles = 80;
			host.View.SsrTracedTiles = 12;
			host.View.SsrTracedPixels = 700;
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 3;
			specification.Serial = 1;
			specification.Host = &host;
			auto created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE(created);
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			session->FixedStep();
			session->FrameUpdate(FrameTime{ .DeltaTime = session->GetFixedDelta(), .UnscaledDeltaTime = session->GetFixedDelta(), .Alpha = 1.0, .FrameIndex = 0 });
			CHECK(host.Errors.empty());
			// The fixed step's read was refused before it reached the host; the frame's read the host's and the session's
			// counters: Probe, Crate and the fixed step's trace entity, one static body.
			CHECK(host.Reads == 1);
			CHECK(TraceNames(session->GetScene()) == std::vector<std::string>{ "fixed:refused", "frame:true:30:9:600:7:2:1:3:1" });

			// Evaluations and test cases read it in either phase (only fixed-step gameplay is refused).
			const auto evaluated = session->GetScripts()->Evaluate("return Application.GetStats().Entities", {});
			REQUIRE(evaluated);
			CHECK(evaluated->Value.Get() == Json(4));
			// M29's light rows and M32's SSR area reach the readout (ADR 0042's phase-5 ledger, carried at M28's landing).
			const auto rows = session->GetScripts()->Evaluate(
				"local s = Application.GetStats() return {s.MaxShadedLights, s.CachedPointShadows, s.CachedSpotShadows, s.SsrTiles, s.SsrTracedTiles, s.SsrTracedPixels}",
				{});
			REQUIRE(rows);
			CHECK(rows->Value.Get() == Json::array({ 512, 4, 6, 80, 12, 700 }));
		}

		TEST_CASE("PlaySession: the performance readout without a host reports the session's counters with no render")
		{
			Test::ScriptTestFixture fixture;
			static_cast<void>(fixture.GetScene().CreateEntity("Only"));
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			auto created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE(created);
			auto session = std::move(*created);
			const SessionStatistics counters = ReadSessionStatistics(*session);
			CHECK(counters.Entities == 1);
			CHECK(counters.Bodies == 0);
			CHECK(counters.ActiveBodies == 0);
			CHECK(counters.Particles == 0);
			const auto evaluated = session->GetScripts()->Evaluate("local s = Application.GetStats() return {s.RenderAvailable, s.Fps, s.DrawCalls, s.Entities}", {});
			REQUIRE(evaluated);
			CHECK(evaluated->Value.Get() == Json::array({ false, 0, 0, 1 }));
		}
	}

}
