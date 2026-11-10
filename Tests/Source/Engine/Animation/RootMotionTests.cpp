#include "TestsPCH.h"

#include "Engine/Animation/RootMotion.h"

#include "Engine/Core/DetMath.h"

#include <cmath>

// Root motion (M23, C23.3, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md decision 26, stream B): the root-motion joint's
// planar motion taken out of the pose.

namespace Engine {

	namespace {

		glm::quat RotationY(double degrees)
		{
			const double half = degrees * 3.14159265358979323846 / 360.0;
			return glm::quat(static_cast<float>(DetMath::Cos(half)), 0.0f, static_cast<float>(DetMath::Sin(half)), 0.0f);
		}

		// A one-second walk: the hips move from z = 0 to z = 2 and bob from y = 1 to y = 1.5, turning 90 degrees about +Y.
		AnimationClipData MakeWalk()
		{
			const glm::quat turned = RotationY(90.0);
			AnimationClipData clip;
			clip.Duration = 1.0f;
			clip.Channels.push_back(AnimationChannel{ .Target = "Hips", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 1.0f }, .Values = { 0.0f, 1.0f, 0.0f, 0.0f, 1.5f, 2.0f } });
			clip.Channels.push_back(AnimationChannel{ .Target = "Hips", .Path = AnimationPath::Rotation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 1.0f }, .Values = { 0.0f, 0.0f, 0.0f, 1.0f, turned.x, turned.y, turned.z, turned.w } });
			return clip;
		}

		SkeletonData MakeHips()
		{
			SkeletonData skeleton;
			skeleton.Joints.push_back({ .Name = "Hips", .Translation = { 0.0f, 1.0f, 0.0f } });
			return skeleton;
		}

	}

	TEST_SUITE("Animation")
	{
		// The delta is the planar part only (X and Z, and the twist about +Y), across a wrap the pieces to the end and from the
		// start; the hold keeps the hips at the clip's start in X, Z and yaw while their bob stays in the pose.
		TEST_CASE("RootMotion: the planar delta of the root-motion joint follows the clip across a wrap, and the hold keeps the mesh at the start")
		{
			const AnimationClipData walk = MakeWalk();
			const SkeletonData hips = MakeHips();
			const RootMotionDelta half = ComputeRootMotion(walk, hips, AnimationTimeStep{ .PreviousTime = 0.25f, .Time = 0.75f });
			CHECK(half.Translation.x == 0.0f);
			CHECK(half.Translation.y == 0.0f);
			CHECK(std::abs(half.Translation.z - 1.0f) < 1e-6f);
			CHECK(std::abs(half.Rotation.y - RotationY(45.0).y) < 1e-5f);
			// From 0.75 across the end to 0.25: 0.5 m to the end and 0.5 m from the start.
			const RootMotionDelta wrapped = ComputeRootMotion(walk, hips, AnimationTimeStep{ .PreviousTime = 0.75f, .Time = 0.25f, .Wraps = 1 });
			CHECK(std::abs(wrapped.Translation.z - 1.0f) < 1e-6f);
			// Two wraps add one whole cycle: 2 m more.
			const RootMotionDelta twice = ComputeRootMotion(walk, hips, AnimationTimeStep{ .PreviousTime = 0.75f, .Time = 0.25f, .Wraps = 2 });
			CHECK(std::abs(twice.Translation.z - 3.0f) < 1e-5f);

			AnimationPose pose = MakeRestPose(hips);
			REQUIRE(pose.Joints.size() == 1);
			SampleAnimationClip(walk, hips, 0.5f, pose);
			HoldRootMotion(walk, hips, pose);
			CHECK(pose.Joints[0].Translation.x == 0.0f);
			CHECK(pose.Joints[0].Translation.z == 0.0f);
			CHECK(std::abs(pose.Joints[0].Translation.y - 1.25f) < 1e-6f);
			CHECK(std::abs(pose.Joints[0].Rotation.y) < 1e-6f);

			// Blending deltas as poses blend: weight 1 copies, 0.5 halves the translation.
			constexpr uint8_t BothPaths = AnimationPoseTranslationBit | AnimationPoseRotationBit;
			RootMotionDelta blended;
			BlendRootMotion(blended, half, BothPaths, 1.0f);
			CHECK(blended == half);
			RootMotionDelta halved;
			BlendRootMotion(halved, half, BothPaths, 0.5f);
			CHECK(std::abs(halved.Translation.z - 0.5f) < 1e-6f);
		}

		// A layer blends only the root paths its clip keys, as poses blend only keyed paths: a turn layer keying only the
		// rotation leaves the walk's translation whole, and a layer keying neither contributes nothing. The root-motion joint
		// below a static joint (the importer's "Rig" node, turned 90 degrees about +Y) moves in the Animator entity's space,
		// and the hold keeps its model position and yaw at the clip's start.
		TEST_CASE("RootMotion: a layer blends only the root paths its clip keys, and a root-motion joint below a static joint moves in model space")
		{
			const RootMotionDelta walk{ .Translation = { 0.0f, 0.0f, 1.0f } };
			const RootMotionDelta turn{ .Rotation = RotationY(90.0) };
			RootMotionDelta turned = walk;
			BlendRootMotion(turned, turn, AnimationPoseRotationBit, 0.5f);
			CHECK(turned.Translation == walk.Translation);
			CHECK(std::abs(turned.Rotation.y - RotationY(45.0).y) < 1e-5f);
			RootMotionDelta untouched = walk;
			BlendRootMotion(untouched, turn, 0, 1.0f);
			CHECK(untouched == walk);

			SkeletonData rig;
			rig.Joints.push_back({ .Name = "Rig", .Translation = { 0.0f, 0.5f, 0.0f }, .Rotation = RotationY(90.0) });
			rig.Joints.push_back({ .Name = "Hips", .Parent = 0, .Translation = { 0.0f, 1.0f, 0.0f } });
			rig.RootMotionJoint = 1;
			const AnimationClipData clip = MakeWalk();
			// The hips move 1 m along their parent's +Z between 0.25 and 0.75 s, which the rig's turn carries to model +X, and
			// turn 45 degrees about +Y.
			const RootMotionDelta delta = ComputeRootMotion(clip, rig, AnimationTimeStep{ .PreviousTime = 0.25f, .Time = 0.75f });
			CHECK(std::abs(delta.Translation.x - 1.0f) < 1e-5f);
			CHECK(std::abs(delta.Translation.z) < 1e-5f);
			CHECK(delta.Translation.y == 0.0f);
			CHECK(std::abs(delta.Rotation.y - RotationY(45.0).y) < 1e-5f);

			AnimationPose pose = MakeRestPose(rig);
			REQUIRE(pose.Joints.size() == 2);
			SampleAnimationClip(clip, rig, 0.5f, pose);
			HoldRootMotion(clip, rig, pose);
			// The rig stays as it rests; the hips keep their bob (1.25 at 0.5 s) at the clip's start in X, Z and yaw.
			CHECK(pose.Joints[0].Translation == rig.Joints[0].Translation);
			CHECK(pose.Joints[0].Rotation == rig.Joints[0].Rotation);
			CHECK(std::abs(pose.Joints[1].Translation.x) < 1e-5f);
			CHECK(std::abs(pose.Joints[1].Translation.y - 1.25f) < 1e-5f);
			CHECK(std::abs(pose.Joints[1].Translation.z) < 1e-5f);
			CHECK(std::abs(pose.Joints[1].Rotation.y) < 1e-5f);
		}

		// W23.3's review: a clip may key a joint's scale to 0 (rigs hide parts that way), so the root-motion joint's parent
		// model transform can be singular; the hold then keeps the sampled translation instead of dividing by a zero
		// determinant, and the pose, its model transforms and its skin matrices stay finite. The rotation is still held.
		TEST_CASE("RootMotion: a root-motion joint below a joint a clip scales to nothing keeps a finite pose")
		{
			SkeletonData rig;
			rig.Joints.push_back({ .Name = "Rig", .Translation = { 0.0f, 0.5f, 0.0f } });
			rig.Joints.push_back({ .Name = "Hips", .Parent = 0, .Translation = { 0.0f, 1.0f, 0.0f } });
			rig.RootMotionJoint = 1;
			AnimationClipData clip = MakeWalk();
			clip.Channels.push_back(AnimationChannel{ .Target = "Rig", .Path = AnimationPath::Scale, .Interpolation = AnimationInterpolation::Step, .Times = { 0.0f }, .Values = { 0.0f, 0.0f, 0.0f } });

			AnimationPose pose = MakeRestPose(rig);
			REQUIRE(pose.Joints.size() == 2);
			SampleAnimationClip(clip, rig, 0.5f, pose);
			const glm::vec3 sampled = pose.Joints[1].Translation;
			HoldRootMotion(clip, rig, pose);
			CHECK(pose.Joints[1].Translation == sampled);
			CHECK(std::abs(pose.Joints[1].Rotation.y) < 1e-5f);
			for (const glm::mat4& matrix : ComputeSkinMatrices(rig, ComputeJointModelTransforms(rig, pose)))
			{
				for (glm::length_t column = 0; column < 4; ++column)
				{
					for (glm::length_t row = 0; row < 4; ++row)
						CHECK(std::isfinite(matrix[column][row]));
				}
			}
		}
	}

}
