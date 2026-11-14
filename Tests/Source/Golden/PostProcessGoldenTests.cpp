#include "TestsPCH.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/TemporalHistory.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <string_view>

// The post-processing goldens of M26 (R-AEX, R-LUT, R-TAA and R-SSR; Docs/Decisions/0037-m26-contract.md decisions 10, 15,
// 17, 18 and 25), each the scene of a golden scaffold of its own, rendered through Test::RenderGoldenScene (a ViewportCapture,
// so a temporal scene renders the warm-up from a reset history, decision 10):
//   - AutoExposure (Scaffold/GoldenAutoExposure.jsonl, IdSeed 2406): a dim room under the Studio environment at 5 % and a
//     faint sun, with a bright emissive lamp, exposed by ExposureMode Auto;
//   - Lut (Scaffold/GoldenLut.jsonl, IdSeed 2407): red, green and blue spheres under the Studio environment, graded by the
//     Warm.cube fixture (red lifted, blue lowered) imported into Assets/Luts/GoldenLut;
//   - Taa (Scaffold/GoldenTaa.jsonl, IdSeed 2408): a fan of thin poles and a turned block against a dark background,
//     anti-aliased by TAA;
//   - Ssr (Scaffold/GoldenSsr.jsonl, IdSeed 2409): a mirror-like metal floor under the Studio environment reflecting a red
//     block, a red tower and a blue ball through SSR, the environment where a ray misses.

namespace Engine {

	namespace {

		constexpr std::string_view AutoExposureGoldenScene = "AutoExposure";
		constexpr std::string_view LutGoldenScene = "Lut";
		constexpr std::string_view TaaGoldenScene = "Taa";
		constexpr std::string_view SsrGoldenScene = "Ssr";

		// The golden scene `name` rendered as every golden is, after `edit`, with `inspect` observing the final snapshot.
		[[nodiscard]] Image CapturePostProcessScene(Test::HeadlessGpuFixture& gpu, std::string_view name, const std::function<void(RenderSnapshot&)>& edit = {},
			const std::function<void(const RenderSnapshot&)>& inspect = {})
		{
			const Test::GoldenSceneOptions options{ .EditSnapshot = edit, .InspectSnapshot = inspect };
			const Result<Image> image = Test::RenderGoldenScene(gpu, name, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 640);
			CHECK(image->Height == 360);
			return *image;
		}

		// The largest difference of one colour channel between `a` and `b` at `pixel`.
		[[nodiscard]] int PixelDelta(const Image& a, const Image& b, size_t pixel)
		{
			int delta = 0;
			for (size_t channel = 0; channel < 3; ++channel)
			{
				const int left = std::to_integer<int>(a.Pixels[pixel * 4 + channel]);
				const int right = std::to_integer<int>(b.Pixels[pixel * 4 + channel]);
				delta = std::max(delta, left > right ? left - right : right - left);
			}
			return delta;
		}

		// How many pixels of `a` and `b` differ by more than `tolerance` levels in a colour channel.
		[[nodiscard]] size_t CountChangedPixels(const Image& a, const Image& b, int tolerance)
		{
			REQUIRE(a.Pixels.size() == b.Pixels.size());
			size_t changed = 0;
			for (size_t pixel = 0; pixel < a.Pixels.size() / 4; ++pixel)
				changed += PixelDelta(a, b, pixel) > tolerance ? 1U : 0U;
			return changed;
		}

		// The mean display-encoded level of each colour channel of `image`.
		[[nodiscard]] std::array<double, 3> MeanLevels(const Image& image)
		{
			std::array<double, 3> sums{};
			const size_t pixels = image.Pixels.size() / 4;
			for (size_t pixel = 0; pixel < pixels; ++pixel)
			{
				for (size_t channel = 0; channel < 3; ++channel)
					sums[channel] += std::to_integer<int>(image.Pixels[pixel * 4 + channel]);
			}
			for (double& sum : sums)
				sum /= static_cast<double>(pixels);
			return sums;
		}

		[[nodiscard]] double MeanLevel(const Image& image)
		{
			const std::array<double, 3> levels = MeanLevels(image);
			return (levels[0] + levels[1] + levels[2]) / 3.0;
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		// Auto exposure meters the dim room and lifts it far above its manual exposure at ExposureEV 0, the lamp's highlight
		// left out of the metered window; the capture warms up from a reset history, so a second one repeats it exactly.
		TEST_CASE("Golden: AutoExposure")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image automatic = CapturePostProcessScene(gpu, AutoExposureGoldenScene, {}, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Post.ExposureMode == RenderExposureMode::Auto);
				CHECK(snapshot.Post.AntiAliasing == RenderAntiAliasing::Fxaa);
				CHECK(NeedsTemporalWarmup(snapshot));
			});
			const Image manual = CapturePostProcessScene(gpu, AutoExposureGoldenScene, [](RenderSnapshot& snapshot)
			{
				snapshot.Post.ExposureMode = RenderExposureMode::Manual;
			});
			// Mean display-encoded levels: about 110 metered, about 61 at the manual exposure.
			const double automaticLevel = MeanLevel(automatic);
			const double manualLevel = MeanLevel(manual);
			CAPTURE(automaticLevel);
			CAPTURE(manualLevel);
			CHECK(automaticLevel > manualLevel + 40.0);
			CHECK(CapturePostProcessScene(gpu, AutoExposureGoldenScene).Pixels == automatic.Pixels);
			ENGINE_CHECK_GOLDEN(AutoExposureGoldenScene, automatic, gpu.GetDevice().GetInfo().DeviceClass);
		}

		// The warm grade lifts red and lowers blue against the ungraded image; the built-in identity LUT changes the image only
		// by its sampling's and 16-bit storage's rounding (decision 15), so no pixel moves by more than one level. The identity
		// comparison renders without anti-aliasing, since FXAA's edge test can turn a one-level difference into a larger one.
		TEST_CASE("Golden: Lut")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image graded = CapturePostProcessScene(gpu, LutGoldenScene, {}, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Post.Lut.IsValid());
				CHECK(snapshot.Post.Lut != BuiltinAssetHandles::IdentityLut);
				CHECK(snapshot.Post.LutIntensity == 1.0f);
				CHECK(snapshot.Post.AntiAliasing == RenderAntiAliasing::Fxaa);
				CHECK_FALSE(NeedsTemporalWarmup(snapshot));
			});
			const Image ungraded = CapturePostProcessScene(gpu, LutGoldenScene, [](RenderSnapshot& snapshot)
			{
				snapshot.Post.Lut = AssetHandle();
			});
			const Image ungradedAliased = CapturePostProcessScene(gpu, LutGoldenScene, [](RenderSnapshot& snapshot)
			{
				snapshot.Post.Lut = AssetHandle();
				snapshot.Post.AntiAliasing = RenderAntiAliasing::None;
			});
			const Image identityAliased = CapturePostProcessScene(gpu, LutGoldenScene, [](RenderSnapshot& snapshot)
			{
				snapshot.Post.Lut = BuiltinAssetHandles::IdentityLut;
				snapshot.Post.AntiAliasing = RenderAntiAliasing::None;
			});
			const std::array<double, 3> gradedLevels = MeanLevels(graded);
			const std::array<double, 3> ungradedLevels = MeanLevels(ungraded);
			CAPTURE(gradedLevels[0]);
			CAPTURE(gradedLevels[2]);
			CAPTURE(ungradedLevels[0]);
			CAPTURE(ungradedLevels[2]);
			CHECK(gradedLevels[0] > ungradedLevels[0] + 4.0);
			CHECK(gradedLevels[2] < ungradedLevels[2] - 4.0);
			CHECK(CountChangedPixels(identityAliased, ungradedAliased, 1) == 0);
			CHECK(CapturePostProcessScene(gpu, LutGoldenScene).Pixels == graded.Pixels);
			ENGINE_CHECK_GOLDEN(LutGoldenScene, graded, gpu.GetDevice().GetInfo().DeviceClass);
		}

		// TAA resolves the poles' and the block's edges differently from FXAA and from no anti-aliasing; the capture renders the
		// warm-up from a reset history, so it is byte-identical after it (the acceptance).
		TEST_CASE("Golden: Taa")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image resolved = CapturePostProcessScene(gpu, TaaGoldenScene, {}, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Post.AntiAliasing == RenderAntiAliasing::Taa);
				CHECK(snapshot.Post.ExposureMode == RenderExposureMode::Manual);
				CHECK(NeedsTemporalWarmup(snapshot));
			});
			for (const RenderAntiAliasing other : { RenderAntiAliasing::None, RenderAntiAliasing::Fxaa })
			{
				CAPTURE(static_cast<int>(other));
				const Image image = CapturePostProcessScene(gpu, TaaGoldenScene, [other](RenderSnapshot& snapshot)
				{
					snapshot.Post.AntiAliasing = other;
				});
				const size_t changed = CountChangedPixels(resolved, image, 8);
				CAPTURE(changed);
				CHECK(changed > 500);
			}
			CHECK(CapturePostProcessScene(gpu, TaaGoldenScene).Pixels == resolved.Pixels);
			ENGINE_CHECK_GOLDEN(TaaGoldenScene, resolved, gpu.GetDevice().GetInfo().DeviceClass);
		}

		// SSR replaces the floor's environment reflection by the blocks and the ball it traces, and a second capture repeats
		// the first exactly.
		TEST_CASE("Golden: Ssr")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image reflected = CapturePostProcessScene(gpu, SsrGoldenScene, {}, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Post.SsrEnabled);
				CHECK(snapshot.Post.AntiAliasing == RenderAntiAliasing::Fxaa);
				CHECK_FALSE(NeedsTemporalWarmup(snapshot));
			});
			const Image unreflected = CapturePostProcessScene(gpu, SsrGoldenScene, [](RenderSnapshot& snapshot)
			{
				snapshot.Post.SsrEnabled = false;
			});
			const size_t changed = CountChangedPixels(reflected, unreflected, 8);
			CAPTURE(changed);
			CHECK(changed > 2000);
			CHECK(CapturePostProcessScene(gpu, SsrGoldenScene).Pixels == reflected.Pixels);
			ENGINE_CHECK_GOLDEN(SsrGoldenScene, reflected, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
