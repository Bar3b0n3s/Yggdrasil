#include "TestsPCH.h"

#include "Engine/Asset/SkeletonData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>

// Skeletons (M23, C23.3, C-SKEL; Docs/Decisions/0034-m23-contract.md decision 24): the cooked format version 1, its validation
// and its fuzz target.

namespace Engine {

	namespace {

		// A three-joint arm: a root, a child and a grandchild, each one unit along +Y, with the inverse binds of that pose.
		SkeletonData MakeArm()
		{
			SkeletonData skeleton;
			for (int32_t index = 0; index < 3; ++index)
			{
				SkeletonJoint joint;
				joint.Name = std::array<const char*, 3>{ "Shoulder", "Elbow", "Wrist" }[static_cast<size_t>(index)];
				joint.Parent = index - 1;
				joint.Translation = index == 0 ? glm::vec3(0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
				joint.InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -static_cast<float>(index), 0.0f));
				skeleton.Joints.push_back(joint);
			}
			return skeleton;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("SkeletonData: the artifact names its type and format version, and its loader is registered")
		{
			CHECK(SkeletonData::StaticType == AssetType::Skeleton);
			CHECK(SkeletonData::FormatVersion == 1);
			CHECK(MaxSkeletonJoints == MaxMeshSkinJoints);
			CHECK(GetCookedFormatVersion(AssetType::Skeleton) == SkeletonData::FormatVersion);
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			REQUIRE(loaders.Find(AssetType::Skeleton) != nullptr);
			// The cooked header is checked before the payload: another version is refused as such.
			const std::byte payload[1] = {};
			const Result<AssetRef<SkeletonData>> newer = LoadCookedSkeleton(WriteCookedArtifact(AssetType::Skeleton, SkeletonData::FormatVersion + 1, 3, payload));
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("SkeletonData: a payload round-trips its joints, and the lookup finds a joint by its exact name")
		{
			// The shoulder taken as a static joint above the skin's root, the elbow (what the importer writes for a rig under
			// an intermediate node).
			SkeletonData arm = MakeArm();
			arm.RootMotionJoint = 1;
			REQUIRE(ValidateSkeletonData(arm).has_value());
			const Buffer payload = SerializeSkeletonPayload(arm);
			// uint32 count, uint32 root-motion joint, then per joint: name (4 + bytes), parent 4, translation 12, rotation 16,
			// scale 12, inverse bind 64.
			CHECK(payload.size() == 4 + 4 + (4 + 8) + 108 + (4 + 5) + 108 + (4 + 5) + 108);
			const Result<SkeletonData> read = DeserializeSkeletonPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Joints == arm.Joints);
			CHECK(read->RootMotionJoint == 1);
			CHECK(SerializeSkeletonPayload(*read) == payload);

			const Result<AssetRef<SkeletonData>> loaded = LoadCookedSkeleton(CookSkeleton(arm, 3));
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK((*loaded)->Joints == arm.Joints);
			CHECK((*loaded)->RootMotionJoint == 1);

			CHECK(FindSkeletonJoint(arm, "Elbow") == 1u);
			CHECK_FALSE(FindSkeletonJoint(arm, "elbow").has_value());
			CHECK_FALSE(FindSkeletonJoint(arm, "Hand").has_value());
		}

		TEST_CASE("SkeletonData: joints that break the format's rules are refused, naming the joint")
		{
			const auto refused = [](const SkeletonData& skeleton)
			{
				const Status status = ValidateSkeletonData(skeleton);
				return !status.has_value() && status.error().GetCode() == ErrorCode::Validation;
			};
			CHECK(refused(SkeletonData{}));
			SkeletonData forward = MakeArm();
			forward.Joints[1].Parent = 2; // a parent after its child
			CHECK(refused(forward));
			SkeletonData duplicate = MakeArm();
			duplicate.Joints[2].Name = "Elbow";
			CHECK(refused(duplicate));
			SkeletonData unnamed = MakeArm();
			unnamed.Joints[0].Name.clear();
			CHECK(refused(unnamed));
			SkeletonData denormal = MakeArm();
			denormal.Joints[1].Rotation = glm::quat(2.0f, 0.0f, 0.0f, 0.0f);
			CHECK(refused(denormal));
			SkeletonData flat = MakeArm();
			flat.Joints[1].Scale = glm::vec3(1.0f, 0.0f, 1.0f);
			CHECK(refused(flat));
			SkeletonData projective = MakeArm();
			projective.Joints[2].InverseBind[0][3] = 0.5f;
			CHECK(refused(projective));
			SkeletonData infinite = MakeArm();
			infinite.Joints[0].Translation.x = std::numeric_limits<float>::infinity();
			CHECK(refused(infinite));
			SkeletonData noRoot = MakeArm();
			noRoot.RootMotionJoint = 3; // the joint count
			CHECK(refused(noRoot));
			SkeletonData tooMany;
			tooMany.Joints.resize(MaxSkeletonJoints + 1);
			for (size_t index = 0; index < tooMany.Joints.size(); ++index)
				tooMany.Joints[index].Name = "Joint " + std::to_string(index);
			CHECK(refused(tooMany));

			Buffer truncated = SerializeSkeletonPayload(MakeArm());
			REQUIRE_FALSE(truncated.empty());
			truncated.pop_back();
			const Result<SkeletonData> read = DeserializeSkeletonPayload(truncated);
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::Parse);
		}

		TEST_CASE("SkeletonData: 10,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			const Buffer payload = SerializeSkeletonPayload(MakeArm());
			REQUIRE(DeserializeSkeletonPayload(payload).has_value());
			const Test::FuzzRun run = Test::RunFuzzCase(0x5E1E, 10000, [&payload](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = payload;
				Test::MutateBytes(mutated, random);
				// A payload the reader accepts is a valid skeleton.
				const Result<SkeletonData> read = DeserializeSkeletonPayload(mutated);
				if (!read.has_value())
					return false;
				CHECK(ValidateSkeletonData(*read).has_value());
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}
	}

}
