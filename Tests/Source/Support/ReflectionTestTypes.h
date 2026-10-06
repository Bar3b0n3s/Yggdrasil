#pragma once

#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/VariantValue.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Reflected types that exercise every FieldType, for the Reflection unit tests (independent of the built-in components,
// so a Reflection test fails for a Reflection bug only).

namespace Engine {

	namespace Test {

		// Registry enum "TestShape".
		enum class TestShape : uint8_t
		{
			Box,
			Sphere,
			Capsule
		};

		// Registry struct "TestInner".
		struct TestInner
		{
			float Weight = 1.0f; // Min 0
			std::string Label = "Inner";
		};

		// Registry struct "TestAllFields": one field of every FieldType, in FieldType order, registered in member order.
		struct TestAllFields
		{
			bool Flag = true;
			int32_t Count = 3; // Min -10, Max 10
			uint32_t Size = 4; // Max 100
			float Mass = 1.0f; // Min 0.001
			glm::vec2 Offset = glm::vec2(0.0f);
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec4 Plane = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Tint = glm::vec3(1.0f); // Color3
			glm::vec4 Glow = glm::vec4(1.0f); // Color4
			glm::bvec3 Locks = glm::bvec3(false);
			std::string Name = "Sample";
			UUID Target;                            // EntityRef
			TypedAssetHandle<AssetType::Mesh> Mesh; // AssetRef, filter "Mesh"
			TestShape Shape = TestShape::Box;       // Enum
			std::vector<std::string> Labels;        // Array
			TestInner Inner;                        // Struct
			std::map<std::string, float> Scores;    // Map
			VariantValue Extra;                     // Variant (free-form)
			glm::vec3 Scale = glm::vec3(1.0f);      // MinMagnitude 1e-4
		};

		// Registry component "TestComponent": a component with a Variant map resolved through ResolveContext::Schemas (the
		// shape of ScriptComponent) and a type-level validator (Mass > 0 when Shape is Sphere) with its Generate hook.
		struct TestComponent
		{
			TypedAssetHandle<AssetType::Script> Owner;
			std::map<std::string, VariantValue> Overrides;
			float Mass = 1.0f;
			TestShape Shape = TestShape::Box;
		};

		// Registers TestShape, TestInner, TestAllFields and TestComponent (without a scene host) into `registry`, which the
		// caller freezes.
		void RegisterReflectionTestTypes(TypeRegistry& registry);

	}

}
