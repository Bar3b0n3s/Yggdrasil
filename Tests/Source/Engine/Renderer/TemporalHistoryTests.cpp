#include "TestsPCH.h"

#include "Engine/Renderer/TemporalHistory.h"

#include "Engine/Core/UUID.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <limits>
#include <set>
#include <utility>

// The temporal-state section's view history (M26, C26.1, R-TAA-VEL, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 10
// and 11, stream X-C): the jitter sequence, the history's previous camera and worlds, and the captures' warm-up rule; and
// M33's particles' previous positions (Docs/Decisions/0048-m33-particle-look.md decision 6).

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
		TEST_CASE("TemporalHistory: the jitter is the Halton (2, 3) sequence centred on the pixel")
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

		TEST_CASE("TemporalHistory: a view's history fills the previous camera and worlds by entity, and a reset or a cut empties them")
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

		TEST_CASE("TemporalHistory: captures warm up only with TAA or auto-exposure in a Lit view")
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
			// The renderer's temporal passes never run in the wireframe or without a camera, so neither warms up.
			snapshot = MakeSnapshot(0.0f, 0.0f);
			snapshot.Flags = RenderViewFlags::Wireframe;
			CHECK_FALSE(NeedsTemporalWarmup(snapshot));
			snapshot = MakeSnapshot(0.0f, 0.0f);
			snapshot.HasCamera = false;
			CHECK_FALSE(NeedsTemporalWarmup(snapshot));
		}

		// Stream X-C's rules beside the contract's: items of one entity match in order and an item the previous frame did not
		// see has no previous world; a previous world that is not finite is not used; a delta time that is negative or not
		// finite is 0; a snapshot without a camera resets the next frame too; and a camera of another projection kind or
		// viewport size is a cut.
		TEST_CASE("TemporalHistory: items match by entity in order, and what cannot be continued starts afresh")
		{
			RenderViewHistory history;
			RenderSnapshot first = MakeSnapshot(0.0f, 1.0f);
			MeshDrawItem twin;
			twin.Entity = UUID(0xB2);
			twin.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 7.0f, 0.0f));
			first.Meshes.push_back(twin);
			first.Meshes[0].World = glm::mat4(std::numeric_limits<float>::infinity());
			history.Apply(first, 0.016f);

			RenderSnapshot second = MakeSnapshot(0.0f, 3.0f);
			MeshDrawItem newcomer;
			newcomer.Entity = UUID(0xD4);
			second.Meshes.push_back(twin);
			second.Meshes.push_back(newcomer);
			history.Apply(second, -1.0f);
			CHECK_FALSE(second.Meshes[0].PreviousWorld.has_value()); // its previous world was not finite
			REQUIRE(second.Meshes[1].PreviousWorld.has_value());
			CHECK(*second.Meshes[1].PreviousWorld == glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
			REQUIRE(second.Meshes[2].PreviousWorld.has_value());
			CHECK(*second.Meshes[2].PreviousWorld == twin.World);
			CHECK_FALSE(second.Meshes[3].PreviousWorld.has_value());
			CHECK(second.Camera.DeltaTime == 0.0f);
			RenderSnapshot nonFinite = MakeSnapshot(0.0f, 3.0f);
			history.Apply(nonFinite, std::numeric_limits<float>::quiet_NaN());
			CHECK(nonFinite.Camera.DeltaTime == 0.0f);
			CHECK(nonFinite.Camera.PreviousViewProjection.has_value());

			// No camera: a reset, and so is the next frame.
			RenderSnapshot cameraless = MakeSnapshot(0.0f, 3.0f);
			cameraless.HasCamera = false;
			history.Apply(cameraless, 0.016f);
			CHECK_FALSE(cameraless.Camera.PreviousViewProjection.has_value());
			RenderSnapshot after = MakeSnapshot(0.0f, 3.0f);
			history.Apply(after, 0.016f);
			CHECK_FALSE(after.Camera.PreviousViewProjection.has_value());
			CHECK_FALSE(after.Meshes[1].PreviousWorld.has_value());

			// Another projection kind or viewport size is a cut.
			RenderSnapshot orthographic = MakeSnapshot(0.0f, 3.0f);
			orthographic.Camera.ProjectionKind = RenderProjection::Orthographic;
			history.Apply(orthographic, 0.016f);
			CHECK_FALSE(orthographic.Camera.PreviousViewProjection.has_value());
			RenderSnapshot resized = orthographic;
			resized.Camera.ViewportWidth = 640;
			history.Apply(resized, 0.016f);
			CHECK_FALSE(resized.Camera.PreviousViewProjection.has_value());
			RenderSnapshot continued = resized;
			history.Apply(continued, 0.016f);
			CHECK(continued.Camera.PreviousViewProjection.has_value());
			CHECK(continued.Camera.DeltaTime == 0.016f);
		}

		// M33 (decision 6): what a particle's motion vectors start from is where the view drew it last, matched by its
		// emitter and its serial, so a particle that died, a newcomer and another emitter's particle of the same serial never
		// borrow a position, and a reset forgets them all.
		TEST_CASE("TemporalHistory: a particle's previous position is its drawn position in the view's last render, matched by emitter and serial")
		{
			const auto addEmitter = [](RenderSnapshot& snapshot, UUID entity, std::initializer_list<std::pair<uint64_t, float>> particles)
			{
				snapshot.ParticleEmitters.push_back(ParticleEmitterItem{ .First = static_cast<uint32_t>(snapshot.Particles.size()),
					.Count = static_cast<uint32_t>(particles.size()),
					.Entity = entity });
				for (const auto& [serial, x] : particles)
					snapshot.Particles.push_back(
						ParticleItem{ .Position = glm::vec3(x, 1.0f, 0.0f), .Size = 0.25f, .Rotation = 0.5f, .Velocity = glm::vec3(0.0f, 2.0f, 0.0f), .Serial = serial });
			};
			RenderViewHistory history;
			RenderSnapshot first = MakeSnapshot(0.0f, 0.0f);
			addEmitter(first, UUID(0xE1), { { 4, 1.0f }, { 5, 2.0f }, { 7, 3.0f } });
			addEmitter(first, UUID(0xE2), { { 5, 50.0f } });
			history.Apply(first, 0.016f);
			for (const ParticleItem& particle : first.Particles)
				CHECK_FALSE(particle.Previous.has_value()); // the history's first frame is a reset

			// Serial 4 died and 8 is new; the second emitter's serial 5 is its own.
			RenderSnapshot second = MakeSnapshot(0.0f, 0.0f);
			addEmitter(second, UUID(0xE2), { { 5, 51.0f }, { 6, 60.0f } });
			addEmitter(second, UUID(0xE1), { { 5, 2.5f }, { 7, 3.5f }, { 8, 0.0f } });
			history.Apply(second, 0.016f);
			REQUIRE(second.Particles.size() == 5);
			REQUIRE(second.Particles[0].Previous.has_value());
			CHECK(second.Particles[0].Previous->Position == glm::vec3(50.0f, 1.0f, 0.0f));
			CHECK_FALSE(second.Particles[1].Previous.has_value());
			REQUIRE(second.Particles[2].Previous.has_value());
			CHECK(second.Particles[2].Previous->Position == glm::vec3(2.0f, 1.0f, 0.0f));
			// The previous draw carries the size, angle and velocity too, from which the previous corners are laid.
			CHECK(second.Particles[2].Previous->Size == 0.25f);
			CHECK(second.Particles[2].Previous->Rotation == 0.5f);
			CHECK(second.Particles[2].Previous->Velocity == glm::vec3(0.0f, 2.0f, 0.0f));
			REQUIRE(second.Particles[3].Previous.has_value());
			CHECK(second.Particles[3].Previous->Position == glm::vec3(3.0f, 1.0f, 0.0f));
			CHECK_FALSE(second.Particles[4].Previous.has_value());

			// The same snapshot again (a capture's warm-up) draws no motion: each particle's previous position is its own.
			RenderSnapshot again = second;
			history.Apply(again, 0.0f);
			for (const ParticleItem& particle : again.Particles)
			{
				REQUIRE(particle.Previous.has_value());
				CHECK(particle.Previous->Position == particle.Position);
			}

			// A reset forgets every particle, and a position that was not finite is no previous position.
			RenderSnapshot broken = again;
			broken.Particles[0].Position = glm::vec3(std::numeric_limits<float>::infinity());
			history.Apply(broken, 0.016f);
			RenderSnapshot afterBroken = again;
			history.Apply(afterBroken, 0.016f);
			CHECK_FALSE(afterBroken.Particles[0].Previous.has_value());
			CHECK(afterBroken.Particles[1].Previous.has_value());
			history.Reset();
			RenderSnapshot reset = again;
			history.Apply(reset, 0.016f);
			for (const ParticleItem& particle : reset.Particles)
				CHECK_FALSE(particle.Previous.has_value());
		}

		// M33 (decision 6, the review's finding 3): an emitter's state made again (a disable and enable, a re-added
		// ParticleEmitter) numbers its particles from 0 again; its new incarnation keeps them from taking the positions of its
		// predecessor's particles of the same serials, even across renders that never saw the gap.
		TEST_CASE("TemporalHistory: a particle state made again never takes its predecessor's positions, its serials restarting")
		{
			const auto makeSnapshot = [](uint64_t incarnation, float x)
			{
				RenderSnapshot snapshot = MakeSnapshot(0.0f, 0.0f);
				snapshot.ParticleEmitters.push_back(ParticleEmitterItem{ .First = 0, .Count = 2, .Entity = UUID(0xE3), .Incarnation = incarnation });
				snapshot.Particles.push_back(ParticleItem{ .Position = glm::vec3(x, 0.0f, 0.0f), .Serial = 0 });
				snapshot.Particles.push_back(ParticleItem{ .Position = glm::vec3(x, 1.0f, 0.0f), .Serial = 1 });
				return snapshot;
			};
			RenderViewHistory history;
			RenderSnapshot first = makeSnapshot(1, 5.0f);
			history.Apply(first, 0.016f);
			RenderSnapshot same = makeSnapshot(1, 6.0f);
			history.Apply(same, 0.016f);
			REQUIRE(same.Particles[0].Previous.has_value());
			CHECK(same.Particles[0].Previous->Position == glm::vec3(5.0f, 0.0f, 0.0f));
			// The state was made again between renders: serials 0 and 1 are new particles.
			RenderSnapshot remade = makeSnapshot(2, -3.0f);
			history.Apply(remade, 0.016f);
			CHECK_FALSE(remade.Particles[0].Previous.has_value());
			CHECK_FALSE(remade.Particles[1].Previous.has_value());
			RenderSnapshot after = makeSnapshot(2, -2.0f);
			history.Apply(after, 0.016f);
			REQUIRE(after.Particles[1].Previous.has_value());
			CHECK(after.Particles[1].Previous->Position == glm::vec3(-3.0f, 1.0f, 0.0f));
		}
	}

}
