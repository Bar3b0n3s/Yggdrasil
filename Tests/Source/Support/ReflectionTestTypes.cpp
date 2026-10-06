#include "TestsPCH.h"
#include "Support/ReflectionTestTypes.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"

namespace Engine {

	namespace Test {

		// Resolves TestComponent.Overrides values by key through the schema source, with the component's Owner handle as the
		// schema owner (the ScriptComponent pattern of §5.4): read from the C++ object when there is one, otherwise from the
		// component's JSON object.
		static Result<const FieldInfo*> ResolveTestOverride(const ResolveContext& context)
		{
			if (context.Registry == nullptr || context.OwnerType == nullptr || context.OwnerType != context.Registry->FindComponent<TestComponent>())
				return MakeError(ErrorCode::InvalidArgument, "override '{}' is not inside a TestComponent", std::string(context.Key));
			if (context.Schemas == nullptr)
				return MakeError(ErrorCode::NotFound, "no schema source for override '{}'", std::string(context.Key));

			if (context.Owner != nullptr)
				return context.Schemas->FindField(static_cast<const TestComponent*>(context.Owner)->Owner.GetHandle(), context.Key);
			if (context.OwnerJson == nullptr)
				return MakeError(ErrorCode::InvalidArgument, "override '{}' has no owner to resolve against", std::string(context.Key));
			ENGINE_TRY_ASSIGN(const UUID owner, context.OwnerJson->ReadMember<UUID>("Owner"));
			return context.Schemas->FindField(owner, context.Key);
		}

		// TestComponent's type-level rule: a sphere needs a positive mass.
		static void ValidateTestComponent(const TestComponent& component, ValidationContext& context)
		{
			if (component.Shape == TestShape::Sphere && component.Mass <= 0.0f)
				context.Error("Mass", "must be > 0 for spheres");
		}

		// Makes a randomized TestComponent satisfy its validator.
		static void GenerateTestComponent(TestComponent& component, Random& random)
		{
			if (component.Shape == TestShape::Sphere && component.Mass <= 0.0f)
				component.Mass = 0.5f + random.NextFloat();
		}

		void RegisterReflectionTestTypes(TypeRegistry& registry)
		{
			registry.Enum<TestShape>("TestShape", "A test enum.")
				.Entry(TestShape::Box, "Box", "A box.")
				.Entry(TestShape::Sphere, "Sphere", "A sphere.")
				.Entry(TestShape::Capsule, "Capsule", "A capsule.");

			registry.Struct<TestInner>("TestInner", "A nested test struct.")
				.Field("Weight", &TestInner::Weight, "A non-negative weight.", { .Min = 0.0 })
				.Field("Label", &TestInner::Label, "A label.");

			registry.Struct<TestAllFields>("TestAllFields", "One field of every FieldType.")
				.Field("Flag", &TestAllFields::Flag, "A Bool.")
				.Field("Count", &TestAllFields::Count, "An Int32 in [-10, 10].", { .Min = -10.0, .Max = 10.0 })
				.Field("Size", &TestAllFields::Size, "A UInt32 up to 100.", { .Max = 100.0 })
				.Field("Mass", &TestAllFields::Mass, "A Float of at least 0.001.", { .Min = 0.001, .Unit = "kg" })
				.Field("Offset", &TestAllFields::Offset, "A Vec2.")
				.Field("Position", &TestAllFields::Position, "A Vec3.", { .Unit = "m" })
				.Field("Plane", &TestAllFields::Plane, "A Vec4.")
				.Field("Rotation", &TestAllFields::Rotation, "A Quat.")
				.ColorField("Tint", &TestAllFields::Tint, "A Color3.")
				.ColorField("Glow", &TestAllFields::Glow, "A Color4.")
				.Field("Locks", &TestAllFields::Locks, "A Bool3.")
				.Field("Name", &TestAllFields::Name, "A String.")
				.Field("Target", &TestAllFields::Target, "An EntityRef.")
				.Field("Mesh", &TestAllFields::Mesh, "An AssetRef accepting meshes.")
				.Field("Shape", &TestAllFields::Shape, "An Enum.")
				.Field("Labels", &TestAllFields::Labels, "An Array of strings.")
				.Field("Inner", &TestAllFields::Inner, "A Struct.")
				.Field("Scores", &TestAllFields::Scores, "A Map of floats.")
				.Field("Extra", &TestAllFields::Extra, "A free-form Variant.")
				.Field("Scale", &TestAllFields::Scale, "A Vec3 whose components have a magnitude of at least 1e-4.", { .MinMagnitude = 1e-4 });

			registry.Component<TestComponent>("TestComponent", "A test component with resolved Variant overrides.")
				.Category("Test")
				.Version(1)
				.Field("Owner", &TestComponent::Owner, "The schema owner.")
				.VariantField("Overrides", &TestComponent::Overrides, "Values resolved against the owner's schema.", &ResolveTestOverride)
				.Field("Mass", &TestComponent::Mass, "A mass.", { .Min = 0.0, .Unit = "kg" })
				.Field("Shape", &TestComponent::Shape, "A shape.")
				.Validate(&ValidateTestComponent)
				.Generate(&GenerateTestComponent);
		}

	}

}
