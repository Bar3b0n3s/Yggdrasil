#include "TestsPCH.h"

#include "Engine/Animation/AnimationPose.h"

#include "Engine/Core/DetMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

// A skeleton's pose: sampling, the script-driven blend of layers, joint and skin matrices and the pose digest (M23, C23.3,
// C-SKEL, C-MORPH, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md decision 26, stream B). Skipped until stream B implements
// them.

namespace Engine {

	namespace {

		glm::quat RotationZ(double degrees)
		{
			const double half = degrees * 3.14159265358979323846 / 360.0;
			return glm::quat(static_cast<float>(DetMath::Cos(half)), 0.0f, 0.0f, static_cast<float>(DetMath::Sin(half)));
		}

		// A two-joint arm: the shoulder at the origin and the elbow one unit up, with the inverse binds of that pose.
		SkeletonData MakeArm()
		{
			SkeletonData skeleton;
			skeleton.Joints.push_back({ .Name = "Shoulder" });
			skeleton.Joints.push_back({ .Name = "Elbow", .Parent = 0, .Translation = { 0.0f, 1.0f, 0.0f }, .InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)) });
			return skeleton;
		}

		// A one-second clip that turns the elbow from 0 to 90 degrees about +Z (linear), steps the shoulder up by 0.5 at 0.5 s
		// and keys the face's weight (cubic spline, flat tangents) from 0 to 1.
		AnimationClipData MakeWave()
		{
			const glm::quat end = RotationZ(90.0);
			AnimationClipData clip;
			clip.Duration = 1.0f;
			clip.Channels.push_back(AnimationChannel{ .Target = "Elbow", .Path = AnimationPath::Rotation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 1.0f }, .Values = { 0.0f, 0.0f, 0.0f, 1.0f, end.x, end.y, end.z, end.w } });
			clip.Channels.push_back(AnimationChannel{ .Target = "Shoulder", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Step, .Times = { 0.0f, 0.5f }, .Values = { 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f } });
			clip.Channels.push_back(AnimationChannel{ .Target = "Face", .Path = AnimationPath::Weights, .Interpolation = AnimationInterpolation::CubicSpline, .WeightCount = 1, .Times = { 0.0f, 1.0f }, .Values = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } });
			clip.Channels.push_back(AnimationChannel{ .Target = "Tail", .Path = AnimationPath::Rotation, .Interpolation = AnimationInterpolation::Step, .Times = { 0.0f }, .Values = { 0.0f, 0.0f, 1.0f, 0.0f } });
			return clip;
		}

	}

	TEST_SUITE("Animation")
	{
		// glTF 2.0 Appendix C: Step holds the earlier key, Linear interpolates (rotations along the shorter arc), CubicSpline is
		// the Hermite spline of the keys' values and tangents scaled by the interval; outside the keys the ends hold. A channel
		// whose joint the skeleton lacks ("Tail") is ignored, and a sampled path is marked keyed.
		TEST_CASE("AnimationPose: keys interpolate step, linear and cubic-spline as glTF defines them" * doctest::skip(true))
		{
			const SkeletonData arm = MakeArm();
			const AnimationClipData wave = MakeWave();
			AnimationPose pose = MakeRestPose(arm);
			REQUIRE(pose.Joints.size() == 2);
			CHECK(pose.Joints[1].Translation == glm::vec3(0.0f, 1.0f, 0.0f));
			CHECK(pose.Keyed == std::vector<uint8_t>{ 0, 0 });

			SampleAnimationClip(wave, arm, 0.25f, pose);
			const glm::quat quarter = RotationZ(22.5);
			CHECK(std::abs(pose.Joints[1].Rotation.z - quarter.z) < 1e-6f);
			CHECK(std::abs(pose.Joints[1].Rotation.w - quarter.w) < 1e-6f);
			CHECK(pose.Joints[0].Translation == glm::vec3(0.0f));
			CHECK(pose.Keyed[0] == AnimationPoseTranslationBit);
			CHECK(pose.Keyed[1] == AnimationPoseRotationBit);
			REQUIRE(pose.Morphs.size() == 1);
			CHECK(pose.Morphs[0].Target == "Face");
			// The Hermite spline with flat tangents at t = 0.25: 3 t^2 - 2 t^3 = 0.15625.
			REQUIRE(pose.Morphs[0].Weights.size() == 1);
			CHECK(std::abs(pose.Morphs[0].Weights[0] - 0.15625f) < 1e-6f);

			SampleAnimationClip(wave, arm, 0.5f, pose);
			CHECK(pose.Joints[0].Translation == glm::vec3(0.0f, 0.5f, 0.0f));
			// Past the clip's end the last keys hold.
			SampleAnimationClip(wave, arm, 7.0f, pose);
			CHECK(std::abs(pose.Joints[1].Rotation.z - RotationZ(90.0).z) < 1e-6f);
			CHECK(pose.Morphs[0].Weights[0] == 1.0f);
		}

		// Script-driven blending: layer 1 at weight 0.5 over layer 0 halves the elbow's turn; a layer keys only its own paths,
		// so the shoulder layer 0 moved stays moved; weight 0 leaves the result and weight 1 copies the layer exactly.
		TEST_CASE("AnimationPose: layers blend in order by their weights over the rest pose, only on the paths they key" * doctest::skip(true))
		{
			const SkeletonData arm = MakeArm();
			AnimationPose result = MakeRestPose(arm);
			REQUIRE(result.Joints.size() == 2);
			REQUIRE(result.Keyed.size() == 2);
			AnimationPose walk = MakeRestPose(arm);
			walk.Joints[0].Translation = glm::vec3(0.0f, 0.5f, 0.0f);
			walk.Keyed[0] = AnimationPoseTranslationBit;
			BlendAnimationPose(result, walk, 1.0f);
			CHECK(result.Joints[0].Translation == walk.Joints[0].Translation);

			AnimationPose wave = MakeRestPose(arm);
			wave.Joints[1].Rotation = RotationZ(90.0);
			wave.Keyed[1] = AnimationPoseRotationBit;
			AnimationPose unchanged = result;
			BlendAnimationPose(unchanged, wave, 0.0f);
			CHECK(unchanged == result);
			BlendAnimationPose(result, wave, 0.5f);
			CHECK(result.Joints[0].Translation == glm::vec3(0.0f, 0.5f, 0.0f));
			const glm::quat half = RotationZ(45.0);
			// Normalized linear interpolation: the half turn's direction, unit length.
			CHECK(std::abs(result.Joints[1].Rotation.z / result.Joints[1].Rotation.w - half.z / half.w) < 1e-3f);
			CHECK(std::abs(glm::length(result.Joints[1].Rotation) - 1.0f) < 1e-6f);
			CHECK(result.Keyed[1] == AnimationPoseRotationBit);
		}

		TEST_CASE("AnimationPose: joint model transforms compose parent before child, and skin matrices apply the inverse binds" * doctest::skip(true))
		{
			const SkeletonData arm = MakeArm();
			AnimationPose pose = MakeRestPose(arm);
			// At rest every skin matrix is the identity: JointModel * InverseBind.
			const std::vector<glm::mat4> rest = ComputeJointModelTransforms(arm, pose);
			REQUIRE(rest.size() == 2);
			CHECK(rest[1] == glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
			for (const glm::mat4& skin : ComputeSkinMatrices(arm, rest))
				CHECK(skin == glm::mat4(1.0f));
			// Turning the shoulder 90 degrees about +Z carries the elbow from (0, 1, 0) to (-1, 0, 0).
			pose.Joints[0].Rotation = RotationZ(90.0);
			const std::vector<glm::mat4> turned = ComputeJointModelTransforms(arm, pose);
			const glm::vec4 elbow = turned[1] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			CHECK(std::abs(elbow.x - -1.0f) < 1e-6f);
			CHECK(std::abs(elbow.y) < 1e-6f);
			const std::vector<glm::mat4> skin = ComputeSkinMatrices(arm, turned);
			// A vertex bound at the elbow's rest position follows the elbow.
			const glm::vec4 bound = skin[1] * glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
			CHECK(std::abs(bound.x - -1.0f) < 1e-6f);
		}

		// Roadmap M23's acceptance: a sampled pose hashes equal in all three configurations. The digest of the wave clip's pose
		// at 0.3 s over the arm, with the layer of 0.5 s blended at 0.25 on top, is committed from Debug by stream B and checked
		// in Release by this case; FeatureTest's Animation suite checks the same digest in the exported Dist run through
		// Test.GetPoseDigest (decision 31). DetMath and IEEE-exact arithmetic make it one value everywhere.
		TEST_CASE("AnimationPose: a sampled pose hashes to its committed digest in every configuration" * doctest::skip(true))
		{
			// Stream B commits the digest its implementation gives in Debug, and confirms it in Release.
			constexpr uint64_t ExpectedDigest = 0;
			const SkeletonData arm = MakeArm();
			const AnimationClipData wave = MakeWave();
			AnimationPose pose = MakeRestPose(arm);
			AnimationPose first = MakeRestPose(arm);
			SampleAnimationClip(wave, arm, 0.3f, first);
			BlendAnimationPose(pose, first, 1.0f);
			AnimationPose second = MakeRestPose(arm);
			SampleAnimationClip(wave, arm, 0.5f, second);
			BlendAnimationPose(pose, second, 0.25f);
			const std::vector<glm::mat4> model = ComputeJointModelTransforms(arm, pose);
			REQUIRE(model.size() == 2);
			const uint64_t digest = ComputeAnimationPoseDigest(model, pose.Morphs);
			CHECK(digest != 0);
			CHECK(digest == ExpectedDigest);
			// The digest covers the matrices and the morph weights.
			std::vector<glm::mat4> moved = model;
			moved[1][3][0] += 1.0f;
			CHECK(ComputeAnimationPoseDigest(moved, pose.Morphs) != digest);
			CHECK(ComputeAnimationPoseDigest(model, {}) != digest);
		}
	}

}
