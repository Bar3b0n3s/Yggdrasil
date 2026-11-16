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
#include <span>
#include <string_view>
#include <utility>

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
// over a long floor: the far ones take the fog's colour (smoke) or fade into it (embers). The test also renders the 20 m
// row alone, with and without the fog, each over the same view without particles: unfogged, the far smoke and embers would
// stand out of the grey field as they do from the dark clear air, so their share of the fogged image must be a small
// fraction of their share of the clear one (batch 2's merge review, finding 2); and it renders the scene twice.

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

		// The z below which ParticlesInFog's 20 m row lies (its emitters stand at z = -14, the camera at z = 6; the 11 m row's at
		// z = -5): its particles rise and spread less than a metre in z over their lives.
		constexpr float FarParticleRowZ = -10.0f;

		// Keeps the snapshot's emitters whose particles all lie beyond FarParticleRowZ, with their particles.
		void KeepFarParticles(RenderSnapshot& snapshot)
		{
			decltype(snapshot.ParticleEmitters) emitters(snapshot.ParticleEmitters.get_allocator());
			decltype(snapshot.Particles) particles(snapshot.Particles.get_allocator());
			for (const ParticleEmitterItem& emitter : snapshot.ParticleEmitters)
			{
				REQUIRE(static_cast<size_t>(emitter.First) + emitter.Count <= snapshot.Particles.size());
				const std::span<const ParticleItem> range(snapshot.Particles.data() + emitter.First, emitter.Count);
				if (range.empty() || !std::ranges::all_of(range, [](const ParticleItem& particle)
				{
					return particle.Position.z < FarParticleRowZ;
				}))
					continue;
				ParticleEmitterItem kept = emitter;
				kept.First = static_cast<uint32_t>(particles.size());
				particles.insert(particles.end(), range.begin(), range.end());
				emitters.push_back(kept);
			}
			snapshot.ParticleEmitters = std::move(emitters);
			snapshot.Particles = std::move(particles);
		}

		// The summed absolute difference of `a` and `b` over every pixel's colour channels, in levels.
		[[nodiscard]] uint64_t SumParticleDifference(const Image& a, const Image& b)
		{
			REQUIRE(a.Pixels.size() == b.Pixels.size());
			uint64_t sum = 0;
			for (size_t pixel = 0; pixel < a.Pixels.size() / 4; ++pixel)
			{
				for (size_t channel = 0; channel < 3; ++channel)
				{
					const int left = std::to_integer<int>(a.Pixels[pixel * 4 + channel]);
					const int right = std::to_integer<int>(b.Pixels[pixel * 4 + channel]);
					sum += static_cast<uint64_t>(left > right ? left - right : right - left);
				}
			}
			return sum;
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

		// Smoke and embers draw in fog, both blends among them, and the fog hides the far particles: the 20 m row's share of the
		// fogged image is under a tenth of its share of the image in clear air (about a twentieth on nvidia-62x; with the
		// particles left unfogged, about a quarter).
		TEST_CASE("Golden: ParticlesInFog")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			enum class ParticleRows : uint8_t
			{
				None,
				FarRow,
				All
			};
			const auto capture = [&gpu](bool fog, ParticleRows particles)
			{
				Test::GoldenSceneOptions options{ .PrepareScene = &RunParticles };
				options.EditSnapshot = [fog, particles](RenderSnapshot& snapshot)
				{
					snapshot.Fog.Enabled = snapshot.Fog.Enabled && fog;
					if (particles == ParticleRows::None)
					{
						snapshot.ParticleEmitters.clear();
						snapshot.Particles.clear();
					}
					else if (particles == ParticleRows::FarRow)
						KeepFarParticles(snapshot);
				};
				options.InspectSnapshot = [fog, particles](const RenderSnapshot& snapshot)
				{
					CHECK(snapshot.Fog.Enabled == fog);
					if (particles == ParticleRows::None)
						return;
					// Every row has a smoke column (Alpha) and an ember stream (Additive).
					REQUIRE(snapshot.ParticleEmitters.size() == (particles == ParticleRows::All ? 6U : 2U));
					for (const RenderParticleBlend blend : { RenderParticleBlend::Alpha, RenderParticleBlend::Additive })
					{
						CHECK(std::ranges::count_if(snapshot.ParticleEmitters, [blend](const ParticleEmitterItem& emitter)
						{
							return emitter.Blend == blend && emitter.Count > 0;
						}) == (particles == ParticleRows::All ? 3 : 1));
					}
				};
				const Result<Image> image = Test::RenderGoldenScene(gpu, ParticlesInFogGoldenScene, options);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				REQUIRE(image->IsValid());
				CHECK(image->Width == 640);
				CHECK(image->Height == 360);
				return *image;
			};
			const uint64_t inFog = SumParticleDifference(capture(true, ParticleRows::FarRow), capture(true, ParticleRows::None));
			const uint64_t inClearAir = SumParticleDifference(capture(false, ParticleRows::FarRow), capture(false, ParticleRows::None));
			CAPTURE(inFog);
			CAPTURE(inClearAir);
			CHECK(inClearAir > 100000);
			CHECK(inFog * 10 < inClearAir);
			const Image fogged = capture(true, ParticleRows::All);
			CHECK(capture(true, ParticleRows::All).Pixels == fogged.Pixels);
			ENGINE_CHECK_GOLDEN(ParticlesInFogGoldenScene, fogged, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
