#include "TestsPCH.h"

#include "Engine/Renderer/TemporalHistory.h"

#include "Engine/Core/UUID.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <set>
#include <utility>

// The temporal-state section's view history (M26, C26.1, R-TAA-VEL, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 10
// and 11, stream X-C): skipped until stream X-C implements it.

namespace Engine {

	namespace {

		// A Lit snapshot with TAA, a camera at `cameraX` and two mesh items, the second moving with `offset`.
		RenderSnapshot MakeSnapshot(float cameraX, float offset)
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.View = glm::translate(glm::mat4(1.0f), glm::vec3(-cameraX, 0.0f, -5.0f));
			snapshot.Camera.ViewportWidth = 320;
			snapshot.Camera.ViewportHeight = 180;
			snapshot.Post.AntiAliasing = RenderAntiAliasing::Taa;
			MeshDrawItem still;
			still.Entity = UUID(0xA1);
			MeshDrawItem moving;
			moving.Entity = UUID(0xB2);
			moving.World = glm::translate(glm::mat4(1.0f), glm::vec3(offset, 0.0f, 0.0f));
			snapshot.Meshes.push_back(still);
			snapshot.Meshes.push_back(moving);
			return snapshot;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("TemporalHistory: the jitter is the Halton (2, 3) sequence centred on the pixel" * doctest::skip(true))
		{
			// Halton index 1: (1/2, 1/3); index 2: (1/4, 2/3); minus 0.5.
			CHECK(GetTaaJitter(0) == glm::vec2(0.0f, 1.0f / 3.0f - 0.5f));
			CHECK(GetTaaJitter(1) == glm::vec2(-0.25f, 2.0f / 3.0f - 0.5f));
			CHECK(GetTaaJitter(TaaJitterSequenceLength) == GetTaaJitter(0));
			std::set<std::pair<float, float>> distinct;
			for (uint32_t frame = 0; frame < TaaJitterSequenceLength; ++frame)
			{
				const glm::vec2 jitter = GetTaaJitter(frame);
				CHECK(jitter.x > -0.5f);
				CHECK(jitter.x < 0.5f);
				CHECK(jitter.y > -0.5f);
				CHECK(jitter.y < 0.5f);
				distinct.insert({ jitter.x, jitter.y });
			}
			CHECK(distinct.size() == TaaJitterSequenceLength);
		}

		TEST_CASE("TemporalHistory: a view's history fills the previous camera and worlds by entity, and a reset or a cut empties them" * doctest::skip(true))
		{
			RenderViewHistory history;
			RenderSnapshot first = MakeSnapshot(0.0f, 0.0f);
			history.Apply(first, 0.016f);
			// The first frame is a reset: no previous data, no delta, frame 0's jitter.
			CHECK_FALSE(first.Camera.PreviousViewProjection.has_value());
			CHECK_FALSE(first.Meshes[0].PreviousWorld.has_value());
			CHECK(first.Camera.DeltaTime == 0.0f);
			CHECK(first.Camera.Jitter == GetTaaJitter(0));

			RenderSnapshot second = MakeSnapshot(1.0f, 2.0f);
			history.Apply(second, 0.016f);
			REQUIRE(second.Camera.PreviousViewProjection.has_value());
			CHECK(*second.Camera.PreviousViewProjection == first.Camera.Projection * first.Camera.View);
			REQUIRE(second.Meshes[1].PreviousWorld.has_value());
			CHECK(*second.Meshes[1].PreviousWorld == glm::mat4(1.0f));
			CHECK(second.Camera.DeltaTime == 0.016f);
			CHECK(second.Camera.Jitter == GetTaaJitter(1));
			CHECK(history.GetFrame() == 1);

			// A reset empties the next frame's history; so does a camera cut (another camera entity).
			history.Reset();
			RenderSnapshot afterReset = MakeSnapshot(1.0f, 2.0f);
			history.Apply(afterReset, 0.016f);
			CHECK_FALSE(afterReset.Camera.PreviousViewProjection.has_value());
			CHECK(afterReset.Camera.Jitter == GetTaaJitter(0));
			RenderSnapshot cut = MakeSnapshot(1.0f, 2.0f);
			cut.Camera.Entity = UUID(0xC3);
			history.Apply(cut, 0.016f);
			CHECK_FALSE(cut.Camera.PreviousViewProjection.has_value());

			// Without TAA, or in pixel-art mode, nothing is jittered.
			RenderSnapshot pixelArt = MakeSnapshot(1.0f, 2.0f);
			pixelArt.Camera.Entity = UUID(0xC3);
			pixelArt.Output.PixelArt = true;
			history.Apply(pixelArt, 0.016f);
			CHECK(pixelArt.Camera.Jitter == glm::vec2(0.0f));
		}

		TEST_CASE("TemporalHistory: captures warm up only with TAA or auto-exposure in a Lit view" * doctest::skip(true))
		{
			RenderSnapshot snapshot = MakeSnapshot(0.0f, 0.0f);
			CHECK(NeedsTemporalWarmup(snapshot));
			snapshot.Output.PixelArt = true;
			CHECK_FALSE(NeedsTemporalWarmup(snapshot));
			// Pixel-art mode ignores auto-exposure too (M18's exposure 1, decision 10).
			snapshot.Post.ExposureMode = RenderExposureMode::Auto;
			CHECK_FALSE(NeedsTemporalWarmup(snapshot));
			// Auto-exposure alone warms up outside pixel-art mode, in a Lit view only.
			snapshot.Output.PixelArt = false;
			snapshot.Post.AntiAliasing = RenderAntiAliasing::Fxaa;
			CHECK(NeedsTemporalWarmup(snapshot));
			snapshot.DebugView = RenderDebugView::Albedo;
			CHECK_FALSE(NeedsTemporalWarmup(snapshot));
			snapshot = MakeSnapshot(0.0f, 0.0f);
			snapshot.Post.AntiAliasing = RenderAntiAliasing::Fxaa;
			CHECK_FALSE(NeedsTemporalWarmup(snapshot));
			CHECK(CaptureWarmupFrames == TaaJitterSequenceLength);
		}
	}

}
