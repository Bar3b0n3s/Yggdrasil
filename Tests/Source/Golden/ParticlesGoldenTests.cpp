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
#include <functional>
#include <string_view>

// The Particles golden (M26, C-PART; Docs/Decisions/0037-m26-contract.md decisions 7 and 25): the golden scaffold
// Scaffold/GoldenParticles.jsonl (IdSeed 2402) writes Assets/Scenes/GoldenParticles/Particles.scene over a lit floor: the
// alpha emitter "Smoke", grey puffs rising from a cone, growing and fading, its base behind a block, and the additive emitter
// "Embers", a burst and a stream of sparks from a sphere, rising and cooling. Edit scenes simulate no particles (decision
// 5), so the test steps the scene's emitters through the Particles phase's system for ParticleTicks ticks of FeatureTest's
// 64 Hz, one step a tick as a lockstep play session does (GoldenSceneOptions::PrepareScene, which writes only their runtime
// state), before it renders.

namespace Engine {

	namespace {

		constexpr std::string_view ParticlesGoldenScene = "Particles";
		// FeatureTest's Simulation.FixedHz, and the ticks the emitters run before the capture (one second).
		constexpr float FeatureTestFixedDelta = 1.0f / 64.0f;
		constexpr uint64_t ParticleTicks = 64;

		// Steps every emitter of `scene` for ParticleTicks ticks, as a lockstep session's Particles phase does.
		[[nodiscard]] Status RunParticles(Scene& scene, AssetManager& /*assets*/)
		{
			ParticleSystem particles(FeatureTestFixedDelta);
			for (uint64_t tick = 1; tick <= ParticleTicks; ++tick)
				particles.Step(scene, tick);
			return {};
		}

		// The scene rendered as every golden is, its emitters stepped or not, then `inspect`.
		[[nodiscard]] Image CaptureParticles(Test::HeadlessGpuFixture& gpu, bool simulated, const std::function<void(const RenderSnapshot&)>& inspect = {})
		{
			Test::GoldenSceneOptions options{ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Post.AntiAliasing == RenderAntiAliasing::Fxaa);
			},
				.InspectSnapshot = inspect };
			if (simulated)
				options.PrepareScene = &RunParticles;
			const Result<Image> image = Test::RenderGoldenScene(gpu, ParticlesGoldenScene, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 640);
			CHECK(image->Height == 360);
			return *image;
		}

		// How many pixels of `a` and `b` differ by more than 4/255 in a colour channel.
		[[nodiscard]] size_t CountChangedPixels(const Image& a, const Image& b)
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
				changed += delta > 4 ? 1U : 0U;
			}
			return changed;
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		// Both emitters draw, one alpha-blended and one additive, with the particles the stepped simulation left alive; the
		// edit scene alone draws none, so the stepped image differs clearly from it, and a second capture repeats the first
		// exactly (the simulation is seeded).
		TEST_CASE("Golden: Particles")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image particles = CaptureParticles(gpu, true, [](const RenderSnapshot& snapshot)
			{
				REQUIRE(snapshot.ParticleEmitters.size() == 2);
				CHECK(snapshot.ParticleEmitters[0].Blend != snapshot.ParticleEmitters[1].Blend);
				for (const ParticleEmitterItem& emitter : snapshot.ParticleEmitters)
				{
					CAPTURE(emitter.Entity);
					CHECK(emitter.Count > 20);
				}
			});
			const Image still = CaptureParticles(gpu, false, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.ParticleEmitters.empty());
				CHECK(snapshot.Particles.empty());
			});
			const size_t changed = CountChangedPixels(particles, still);
			CAPTURE(changed);
			CHECK(changed > 1000);
			CHECK(CaptureParticles(gpu, true).Pixels == particles.Pixels);
			ENGINE_CHECK_GOLDEN(ParticlesGoldenScene, particles, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
