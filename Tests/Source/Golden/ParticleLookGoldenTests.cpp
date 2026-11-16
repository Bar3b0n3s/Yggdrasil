#include "TestsPCH.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/ParticleSystem.h"
#include "Engine/Scene/Scene.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

// The ParticleLook golden (M33, Docs/Decisions/0048-m33-particle-look.md decision 8; Roadmap M33's and World Tree brief 6's
// golden images): the golden scaffold Scaffold/GoldenParticleLook.jsonl (IdSeed 3002) writes
// Assets/Scenes/GoldenParticleLook/ParticleLook.scene over a lit floor, with bloom: "Flame", a turning, spinning flipbook
// flame (Flame.png's sixteen frames, blended) whose base meets the floor softly; "Sparks", additive sparks stretched along
// their velocity; "Ring", flat discs expanding on the ground; and "Ember", embers of intensity 14 blooming. Edit scenes
// simulate no particles, so the test steps the scene's emitters through the Particles phase's system for ParticleTicks ticks
// of FeatureTest's 64 Hz, as "Golden: Particles" does, before it renders.

namespace Engine {

	namespace {

		constexpr std::string_view ParticleLookGoldenScene = "ParticleLook";
		constexpr float FeatureTestFixedDelta = 1.0f / 64.0f;
		constexpr uint64_t ParticleTicks = 64;

		[[nodiscard]] Status RunParticles(Scene& scene, AssetManager& /*assets*/)
		{
			ParticleSystem particles(FeatureTestFixedDelta);
			for (uint64_t tick = 1; tick <= ParticleTicks; ++tick)
				particles.Step(scene, tick);
			return {};
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		// The four looks draw with their emitters' particles alive, every alignment and a flipbook among them, and a second
		// capture repeats the first exactly (the simulation and its second stream are seeded).
		TEST_CASE("Golden: ParticleLook")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const auto capture = [&gpu](bool inspect)
			{
				Test::GoldenSceneOptions options{ .PrepareScene = &RunParticles };
				if (inspect)
				{
					options.InspectSnapshot = [](const RenderSnapshot& snapshot)
					{
						REQUIRE(snapshot.ParticleEmitters.size() == 4);
						CHECK(std::ranges::any_of(snapshot.ParticleEmitters, [](const ParticleEmitterItem& emitter)
						{
							return emitter.Sheet.IsValid() && emitter.BlendFrames && emitter.SoftDistance > 0.0f;
						}));
						for (const RenderParticleAlignment alignment : { RenderParticleAlignment::Camera, RenderParticleAlignment::Velocity, RenderParticleAlignment::Flat })
						{
							CHECK(std::ranges::any_of(snapshot.ParticleEmitters, [alignment](const ParticleEmitterItem& emitter)
							{
								return emitter.Alignment == alignment && emitter.Count > 0;
							}));
						}
						CHECK(std::ranges::any_of(snapshot.Particles, [](const ParticleItem& particle)
						{
							return particle.Color.r > 1.0f;
						}));
					};
				}
				const Result<Image> image = Test::RenderGoldenScene(gpu, ParticleLookGoldenScene, options);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				REQUIRE(image->IsValid());
				CHECK(image->Width == 640);
				CHECK(image->Height == 360);
				return *image;
			};
			const Image look = capture(true);
			CHECK(capture(false).Pixels == look.Pixels);
			ENGINE_CHECK_GOLDEN(ParticleLookGoldenScene, look, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
