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
#include <cstddef>
#include <cstdint>
#include <string_view>

// The ParticleLook golden (M33, Docs/Decisions/0048-m33-particle-look.md decision 8; Roadmap M33's and World Tree brief 6's
// golden images): the golden scaffold Scaffold/GoldenParticleLook.jsonl (IdSeed 3002) writes
// Assets/Scenes/GoldenParticleLook/ParticleLook.scene over a lit floor, with bloom: "Flame", a turning, spinning flipbook
// flame (Flame.png's sixteen frames, blended) whose base meets the floor softly; "Sparks", additive sparks stretched along
// their velocity; "Ring", flat discs expanding on the ground; and "Ember", embers of intensity 14 blooming. Edit scenes
// simulate no particles, so the test steps the scene's emitters through the Particles phase's system for ParticleTicks ticks
// of FeatureTest's 64 Hz, as "Golden: Particles" does, before it renders.
//
// The ParticlesInFog golden (M33's merge after M32, Docs/Decisions/0048-m33-particle-look.md decision 10; Roadmap M33's
// acceptance "particles in fog"): the same scaffold's Assets/Scenes/GoldenParticleLook/ParticlesInFog.scene, a row of
// alpha smoke columns and a row of additive embers at 4, 11 and 20 m from the camera, receding into distance fog ("Haze")
// over a long floor: the far ones take the fog's colour (smoke) or fade into it (embers). The test also renders it with
// and without the fog and the particles, checks that the particles change fewer pixels in fog than in clear air, and
// renders it twice.

namespace Engine {

	namespace {

		constexpr std::string_view ParticleLookGoldenScene = "ParticleLook";
		constexpr std::string_view ParticlesInFogGoldenScene = "ParticlesInFog";
		constexpr float FeatureTestFixedDelta = 1.0f / 64.0f;
		constexpr uint64_t ParticleTicks = 64;

		[[nodiscard]] Status RunParticles(Scene& scene, AssetManager& /*assets*/)
		{
			ParticleSystem particles(FeatureTestFixedDelta);
			for (uint64_t tick = 1; tick <= ParticleTicks; ++tick)
				particles.Step(scene, tick);
			return {};
		}

		// How many pixels of `a` and `b` differ by more than `tolerance` levels in a colour channel.
		[[nodiscard]] size_t CountChangedParticlePixels(const Image& a, const Image& b, int tolerance)
		{
			REQUIRE(a.Pixels.size() == b.Pixels.size());
			size_t changed = 0;
			for (size_t pixel = 0; pixel < a.Pixels.size() / 4; ++pixel)
			{
				int delta = 0;
				for (size_t channel = 0; channel < 3; ++channel)
				{
					const int left = std::to_integer<int>(a.Pixels[pixel * 4 + channel]);
					const int right = std::to_integer<int>(b.Pixels[pixel * 4 + channel]);
					delta = std::max(delta, left > right ? left - right : right - left);
				}
				changed += delta > tolerance ? 1U : 0U;
			}
			return changed;
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

		// Smoke and embers draw in fog, both blends among them, and the fog hides the far particles.
		TEST_CASE("Golden: ParticlesInFog")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const auto capture = [&gpu](bool fog, bool particles)
			{
				Test::GoldenSceneOptions options{ .PrepareScene = &RunParticles };
				options.EditSnapshot = [fog, particles](RenderSnapshot& snapshot)
				{
					snapshot.Fog.Enabled = snapshot.Fog.Enabled && fog;
					if (!particles)
					{
						snapshot.ParticleEmitters.clear();
						snapshot.Particles.clear();
					}
				};
				options.InspectSnapshot = [fog, particles](const RenderSnapshot& snapshot)
				{
					CHECK(snapshot.Fog.Enabled == fog);
					if (!particles)
						return;
					REQUIRE(snapshot.ParticleEmitters.size() == 6);
					for (const RenderParticleBlend blend : { RenderParticleBlend::Alpha, RenderParticleBlend::Additive })
					{
						CHECK(std::ranges::count_if(snapshot.ParticleEmitters, [blend](const ParticleEmitterItem& emitter)
						{
							return emitter.Blend == blend && emitter.Count > 0;
						}) == 3);
					}
				};
				const Result<Image> image = Test::RenderGoldenScene(gpu, ParticlesInFogGoldenScene, options);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				REQUIRE(image->IsValid());
				CHECK(image->Width == 640);
				CHECK(image->Height == 360);
				return *image;
			};
			// The particles' share of the image: the pixels they change over the same view without them. Fog hides the far
			// ones, so they change fewer pixels in fog than in clear air (unfogged particles would stand out of the fog).
			const Image fogged = capture(true, true);
			const size_t inFog = CountChangedParticlePixels(fogged, capture(true, false), 8);
			const size_t inClearAir = CountChangedParticlePixels(capture(false, true), capture(false, false), 8);
			CAPTURE(inFog);
			CAPTURE(inClearAir);
			CHECK(inFog > 1000);
			CHECK(inFog < inClearAir);
			CHECK(capture(true, true).Pixels == fogged.Pixels);
			ENGINE_CHECK_GOLDEN(ParticlesInFogGoldenScene, fogged, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
