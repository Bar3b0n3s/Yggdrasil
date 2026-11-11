#include "TestsPCH.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

// The SkinnedPose golden (M23, C-SKEL and C-MORPH; Docs/Decisions/0034-m23-contract.md decision 33): the golden scaffold
// Scaffold/GoldenSkinnedPose.jsonl (IdSeed 2108) imports Skinned.gltf into Assets/Models/GoldenSkinnedPose and writes
// Assets/Scenes/GoldenSkinnedPose/SkinnedPose.scene: the strip "Posed" playing its Wave clip at tick 24 of FeatureTest's
// 64 Hz (its second joint turned and the clip's Bulge weight at 0.75) and the strip "Morphed" at its rest pose with the
// SkinnedMesh's own Bulge weight 1, lit. Edit scenes draw skinned meshes in their bind pose, so the test poses the
// scene's Animators as the editor's animation preview does (PoseAnimator at their layers' times) before it renders.

namespace Engine {

	namespace {

		constexpr std::string_view SkinnedPoseGoldenScene = "SkinnedPose";

		// Poses every Animator of `scene` at its layers' times, as the editor's animation preview does.
		[[nodiscard]] Status PoseAnimators(Scene& scene, AssetManager& assets)
		{
			std::vector<UUID> animators;
			scene.ForEachCanonical([&animators](Entity entity)
			{
				if (entity.HasComponent<AnimatorComponent>())
					animators.push_back(entity.GetUUID());
			});
			for (const UUID id : animators)
				ENGINE_TRY(PoseAnimator(scene.FindEntityByID(id), assets));
			return {};
		}

		// The scene rendered as every golden is, its Animators posed or not, then `inspect`.
		[[nodiscard]] Image CaptureSkinnedPose(Test::HeadlessGpuFixture& gpu, bool posed, const std::function<void(const RenderSnapshot&)>& inspect = {})
		{
			Test::GoldenSceneOptions options{ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Post.AntiAliasing == RenderAntiAliasing::Fxaa);
			},
				.InspectSnapshot = inspect };
			if (posed)
				options.PrepareScene = &PoseAnimators;
			const Result<Image> image = Test::RenderGoldenScene(gpu, SkinnedPoseGoldenScene, options);
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
		// Both strips are deformed: the posed one by its palette and the clip's morph weight, the morphed one by its own
		// weight over a rest palette; the posed image differs clearly from the bind pose, and a second capture repeats the
		// first exactly.
		TEST_CASE("Golden: SkinnedPose")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image posed = CaptureSkinnedPose(gpu, true, [](const RenderSnapshot& snapshot)
			{
				size_t skinned = 0;
				for (const MeshDrawItem& item : snapshot.Meshes)
				{
					if (item.JointPalette.empty())
						continue;
					++skinned;
					CHECK(item.JointPalette.size() == 2);
					REQUIRE_FALSE(item.MorphWeights.empty());
					CHECK(item.MorphWeights[0] > 0.5f);
				}
				CHECK(skinned == 2);
			});
			const Image bindPose = CaptureSkinnedPose(gpu, false, [](const RenderSnapshot& snapshot)
			{
				for (const MeshDrawItem& item : snapshot.Meshes)
					CHECK(item.JointPalette.empty());
			});
			const size_t changed = CountChangedPixels(posed, bindPose);
			CAPTURE(changed);
			CHECK(changed > 1000);
			CHECK(CaptureSkinnedPose(gpu, true).Pixels == posed.Pixels);
			ENGINE_CHECK_GOLDEN(SkinnedPoseGoldenScene, posed, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
