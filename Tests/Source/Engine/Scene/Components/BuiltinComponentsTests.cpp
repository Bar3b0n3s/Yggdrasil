#include "TestsPCH.h"

#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Support/SceneTestFixture.h"

#include <type_traits>

namespace Engine {

	namespace {

		// A Reflection-only component registered ahead of the built-in ones.
		struct EarlierComponent
		{
			int32_t Value = 0;
		};

		// What §5.4 requires of a reflected component type: plain data that the registry can create, copy and reset.
		template<typename T>
		constexpr bool IsReflectableComponent = std::is_class_v<T> && !std::is_empty_v<T> && std::is_default_constructible_v<T>
			&& std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>;

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("BuiltinComponents: every listed type is plain component data")
		{
			static_assert(BuiltinComponents::Size == 24);
			size_t count = 0;
			ForEachType(BuiltinComponents{}, [&count]<typename T>()
			{
				static_assert(IsReflectableComponent<T>, "a built-in component is non-empty, default-constructible and copyable");
				++count;
			});
			CHECK(count == BuiltinComponents::Size);
		}

		TEST_CASE("BuiltinComponents: runtime-only components, tags and unknown-component storage are not listed")
		{
			// §5.3: runtime-only components are never registered; DisabledTag is the entity key "Active" and
			// UnknownComponentsComponent the serializer's storage (ADR 0006 decisions 7, 8 and 14).
			static_assert(!TypeListContains<WorldTransformComponent, BuiltinComponents>);
			static_assert(!TypeListContains<PreviousWorldTransformComponent, BuiltinComponents>);
			static_assert(!TypeListContains<InterpolationResetTag, BuiltinComponents>);
			static_assert(!TypeListContains<HierarchyDisabledTag, BuiltinComponents>);
			static_assert(!TypeListContains<PendingDestroyTag, BuiltinComponents>);
			static_assert(!TypeListContains<PendingStartTag, BuiltinComponents>);
			static_assert(!TypeListContains<DisabledTag, BuiltinComponents>);
			static_assert(!TypeListContains<UnknownComponentsComponent, BuiltinComponents>);
			static_assert(std::is_empty_v<DisabledTag>);

			// The reflected structs that components hold are not components themselves.
			static_assert(!TypeListContains<PrefabOverride, BuiltinComponents>);
			static_assert(!TypeListContains<PrefabEntityKeys, BuiltinComponents>);
			CHECK(TypeListContains<ScriptComponent, BuiltinComponents>);
		}

		TEST_CASE("BuiltinComponents: core and prefab defaults follow the component table")
		{
			CHECK_FALSE(IDComponent{}.ID.IsValid());
			CHECK(NameComponent{}.Name == "Entity");
			CHECK(TagsComponent{}.Tags.empty());
			CHECK_FALSE(RelationshipComponent{}.Parent.IsValid());
			CHECK(RelationshipComponent{}.Children.empty());

			const TransformComponent transform;
			CHECK(transform.Translation == glm::vec3(0.0f));
			CHECK(transform.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK(transform.Scale == glm::vec3(1.0f));
			CHECK(MinTransformScaleMagnitude == 1e-4f);

			const PrefabInstanceComponent instance;
			CHECK_FALSE(instance.Prefab.IsValid());
			CHECK(instance.Overrides.empty());
			const PrefabOverride prefabOverride;
			CHECK_FALSE(prefabOverride.PrefabEntityID.IsValid());
			CHECK(prefabOverride.Kind == PrefabOverrideKind::Field);
			CHECK(prefabOverride.Component.empty());
			CHECK(prefabOverride.Field.empty());
			CHECK(prefabOverride.Value.IsNull());
			const PrefabEntityKeys keys;
			CHECK(keys.Name == NameComponent{}.Name);
			CHECK(keys.Active);
			CHECK(keys.Tags.empty());
			CHECK_FALSE(PrefabLinkComponent{}.PrefabEntityID.IsValid());
			CHECK_FALSE(PrefabLinkComponent{}.InstanceRoot.IsValid());
		}

		TEST_CASE("BuiltinComponents: rendering defaults follow the component table")
		{
			const MeshRendererComponent mesh;
			CHECK_FALSE(mesh.Mesh.IsValid());
			CHECK(mesh.Materials.empty());
			CHECK(mesh.CastShadows);
			CHECK(mesh.ReceiveShadows);
			CHECK(mesh.Visible);

			const CameraComponent camera;
			CHECK(camera.Projection == ProjectionType::Perspective);
			CHECK(camera.VerticalFov == 60.0f);
			CHECK(camera.OrthographicSize == 10.0f);
			CHECK(camera.NearClip == 0.1f);
			CHECK(camera.FarClip == 1000.0f);
			CHECK_FALSE(camera.Primary);
			CHECK(camera.Clear == ClearMode::Skybox);
			CHECK(camera.ClearColor == glm::vec3(0.05f, 0.05f, 0.06f));

			const DirectionalLightComponent sun;
			CHECK(sun.Color == glm::vec3(1.0f));
			CHECK(sun.Intensity == 3.0f);
			CHECK(sun.CastShadows);
			CHECK(sun.ShadowDistance == 100.0f);
			CHECK(sun.CascadeCount == 4);
			CHECK(sun.CascadeSplitLambda == 0.75f);
			CHECK(sun.LightAngle == 1.0f);
			CHECK(sun.DepthBias == 1.0f);
			CHECK(sun.NormalBias == 1.0f);

			const PointLightComponent point;
			CHECK(point.Color == glm::vec3(1.0f));
			CHECK(point.Intensity == 10.0f);
			CHECK(point.Range == 10.0f);
			CHECK(point.SourceRadius == 0.05f);

			const SpotLightComponent spot;
			CHECK(spot.Color == glm::vec3(1.0f));
			CHECK(spot.Intensity == 10.0f);
			CHECK(spot.Range == 15.0f);
			CHECK(spot.InnerConeAngle == 20.0f);
			CHECK(spot.OuterConeAngle == 30.0f);
			CHECK_FALSE(spot.CastShadows);
			CHECK(spot.SourceRadius == 0.05f);

			const EnvironmentComponent environment;
			CHECK_FALSE(environment.Environment.IsValid());
			CHECK(environment.Intensity == 1.0f);
			CHECK(environment.Rotation == 0.0f);
			CHECK(environment.ShowSkybox);
			CHECK(environment.SkyboxBlur == 0.0f);
			CHECK(environment.FallbackColor == glm::vec3(0.2f, 0.22f, 0.25f));

			const PostProcessComponent post;
			CHECK(post.ExposureEV == 0.0f);
			CHECK(post.Tonemap == Tonemapper::AgX);
			CHECK(post.SsaoEnabled);
			CHECK(post.SsaoRadius == 0.5f);
			CHECK(post.SsaoIntensity == 1.0f);
			CHECK(post.SsaoQuality == SsaoQuality::Medium);
			CHECK(post.BloomEnabled);
			CHECK(post.BloomIntensity == 0.04f);
			CHECK(post.FxaaEnabled);

			const TextComponent text;
			CHECK(text.Text.empty());
			CHECK_FALSE(text.Font.IsValid());
			CHECK(text.Size == 32.0f);
			CHECK(text.Color == glm::vec4(1.0f));
			CHECK(text.Space == TextSpace::Screen);
			CHECK(text.Anchor == glm::vec2(0.5f, 0.5f));
			CHECK(text.Pivot == glm::vec2(0.5f, 0.5f));
			CHECK(text.Offset == glm::vec2(0.0f, 0.0f));
			CHECK(text.Alignment == TextAlignment::Center);
			CHECK_FALSE(text.Billboard);
		}

		TEST_CASE("BuiltinComponents: physics defaults follow the component table")
		{
			const RigidBodyComponent body;
			CHECK(body.Type == BodyType::Dynamic);
			CHECK(body.Mass == 1.0f);
			CHECK(body.Friction == 0.5f);
			CHECK(body.Restitution == 0.0f);
			CHECK(body.LinearDamping == 0.05f);
			CHECK(body.AngularDamping == 0.05f);
			CHECK(body.GravityFactor == 1.0f);
			CHECK(body.MotionQuality == MotionQuality::Discrete);
			CHECK(body.AllowSleeping);
			CHECK(body.LockTranslation == glm::bvec3(false));
			CHECK(body.LockRotation == glm::bvec3(false));
			CHECK(body.Layer == "Default");
			CHECK(body.MaxLinearVelocity == 500.0f);
			CHECK(body.MaxAngularVelocity == 47.12f);
			CHECK_FALSE(body.EnhancedInternalEdgeRemoval);
			CHECK(body.InitialLinearVelocity == glm::vec3(0.0f));
			CHECK(body.InitialAngularVelocity == glm::vec3(0.0f));

			const BoxColliderComponent box;
			CHECK(box.HalfExtents == glm::vec3(0.5f));
			CHECK(box.Offset == glm::vec3(0.0f));
			CHECK(box.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK_FALSE(box.IsTrigger);

			const SphereColliderComponent sphere;
			CHECK(sphere.Radius == 0.5f);
			CHECK(sphere.Offset == glm::vec3(0.0f));
			CHECK_FALSE(sphere.IsTrigger);

			const CapsuleColliderComponent capsule;
			CHECK(capsule.Radius == 0.5f);
			CHECK(capsule.HalfHeight == 0.5f);
			CHECK(capsule.Offset == glm::vec3(0.0f));
			CHECK(capsule.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK_FALSE(capsule.IsTrigger);

			const MeshColliderComponent meshCollider;
			CHECK_FALSE(meshCollider.Mesh.IsValid());
			CHECK_FALSE(meshCollider.Convex);
			CHECK_FALSE(meshCollider.IsTrigger);

			const CharacterControllerComponent character;
			CHECK(character.Height == 1.8f);
			CHECK(character.Radius == 0.3f);
			CHECK(character.MaxSlopeAngle == 45.0f);
			CHECK(character.StepHeight == 0.3f);
			CHECK(character.Mass == 70.0f);
			CHECK(character.GravityFactor == 1.0f);
			CHECK(character.Layer == "Default");

			CHECK(MinColliderDimension == 0.001f);
		}

		TEST_CASE("BuiltinComponents: audio and scripting defaults follow the component table")
		{
			const AudioSourceComponent source;
			CHECK_FALSE(source.Clip.IsValid());
			CHECK(source.Volume == 1.0f);
			CHECK(source.Pitch == 1.0f);
			CHECK_FALSE(source.Loop);
			CHECK_FALSE(source.PlayOnStart);
			CHECK(source.Spatial);
			CHECK(source.MinDistance == 1.0f);
			CHECK(source.MaxDistance == 50.0f);
			CHECK(source.Attenuation == Attenuation::Inverse);
			CHECK(source.Rolloff == 1.0f);
			CHECK(source.DopplerFactor == 1.0f);
			CHECK(source.Group == AudioGroup::Sfx);

			CHECK(AudioListenerComponent{}.Primary);

			const ScriptComponent script;
			CHECK_FALSE(script.Script.IsValid());
			CHECK(script.Fields.empty());
			CHECK(script.ExecutionOrder == 0);
		}

		TEST_CASE("BuiltinComponents: every listed type is registered in list order")
		{
			TypeRegistry registry;
			RegisterBuiltinComponents(registry);
			registry.Freeze();
			REQUIRE(registry.AreComponentsRegistered(BuiltinComponents{}));
			REQUIRE(registry.GetComponents().size() == BuiltinComponents::Size);

			size_t index = 0;
			ForEachType(BuiltinComponents{}, [&]<typename T>()
			{
				const ComponentInfo* info = registry.FindComponent<T>();
				REQUIRE(info != nullptr);
				CHECK(info->GetIndex() == index);
				CHECK(registry.GetComponents()[index] == info);
				++index;
			});
		}

		TEST_CASE("BuiltinComponents: components registered earlier keep their place before the built-in ones")
		{
			TypeRegistry registry;
			registry.Component<EarlierComponent>("Earlier", "A component registered before the built-in ones.")
				.Category("Core")
				.Field("Value", &EarlierComponent::Value, "A value.");
			RegisterBuiltinComponents(registry);
			registry.Freeze();

			REQUIRE(registry.GetComponents().size() == BuiltinComponents::Size + 1);
			CHECK(registry.GetComponents()[0]->GetName() == "Earlier");
			size_t index = 1;
			ForEachType(BuiltinComponents{}, [&]<typename T>()
			{
				const ComponentInfo* info = registry.FindComponent<T>();
				REQUIRE(info != nullptr);
				CHECK(info->GetIndex() == index);
				++index;
			});
		}

		TEST_CASE("BuiltinComponents: registry names are those of the component table")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::vector<std::string> expected = {
				"ID",
				"Name",
				"Tags",
				"Relationship",
				"Transform",
				"Prefab",
				"PrefabLink",
				"MeshRenderer",
				"Camera",
				"DirectionalLight",
				"PointLight",
				"SpotLight",
				"Environment",
				"PostProcess",
				"Text",
				"RigidBody",
				"BoxCollider",
				"SphereCollider",
				"CapsuleCollider",
				"MeshCollider",
				"CharacterController",
				"AudioSource",
				"AudioListener",
				"Script",
			};
			std::vector<std::string> names;
			for (const ComponentInfo* component : registry->GetComponents())
				names.push_back(component->GetName());
			CHECK(names == expected);
		}

		TEST_CASE("BuiltinComponents: every component has a category, a version and descriptions")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::vector<std::string> categories = { "Core", "Rendering", "Physics", "Audio", "Scripting" };
			for (const ComponentInfo* component : registry->GetComponents())
			{
				INFO(component->GetName());
				CHECK(std::find(categories.begin(), categories.end(), component->GetCategory()) != categories.end());
				CHECK(component->GetVersion() == 1);
				CHECK_FALSE(component->GetDescription().empty());
				for (const Scope<FieldInfo>& field : component->GetFields())
					CHECK_FALSE(field->GetDescription().empty());
			}
			for (const EnumInfo* info : registry->GetEnums())
			{
				INFO(info->GetName());
				CHECK_FALSE(info->GetDescription().empty());
				for (const EnumEntry& entry : info->GetEntries())
					CHECK_FALSE(entry.Description.empty());
			}
		}

		TEST_CASE("BuiltinComponents: default member initializers are valid for their own metadata")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			for (const ComponentInfo* component : registry->GetComponents())
			{
				INFO(component->GetName());
				const ObjectPtr object = component->CreateDefault();
				REQUIRE(object != nullptr);
				ResolveContext resolve;
				resolve.Registry = registry.get();
				ValidationContext context;
				component->Validate(object.get(), resolve, context);
				CHECK_FALSE(context.HasErrors());
			}
		}
	}

}
