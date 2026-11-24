#include "TestsPCH.h"

#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/ParticleStateComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
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
			// M30's Navigation and NavModifier (Docs/Decisions/0045-m30-navigation.md decision 5) made 57, M36's ColliderSurface
			// (Docs/Decisions/0053-m36-contact-surfaces.md decision 1) 58, M37's four joints (Docs/Decisions/0054-m37-joints.md
			// decision 1) 62, M41's BoneAttachment (Docs/Decisions/0058-m41-bone-attachments.md decision 3) 63, and M42's
			// PlanarReflection (Docs/Decisions/0059-m42-planar-reflections.md decision 3) 64, and M47's Water and Buoyancy
			// (Docs/Decisions/0065-m47-water.md decision 3) 66.
			static_assert(BuiltinComponents::Size == 66);
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
			// M23 (C23.3, Docs/Decisions/0034-m23-contract.md decision 29): an Animator's sampled pose is runtime state.
			static_assert(!TypeListContains<AnimationPoseComponent, BuiltinComponents>);
			// M26 (C26.1, Docs/Decisions/0037-m26-contract.md decision 5): an emitter's live particles are runtime state.
			static_assert(!TypeListContains<ParticleStateComponent, BuiltinComponents>);
			static_assert(!TypeListContains<DisabledTag, BuiltinComponents>);
			static_assert(!TypeListContains<UnknownComponentsComponent, BuiltinComponents>);
			static_assert(std::is_empty_v<DisabledTag>);

			// The reflected structs that components hold are not components themselves.
			static_assert(!TypeListContains<PrefabOverride, BuiltinComponents>);
			static_assert(!TypeListContains<PrefabEntityKeys, BuiltinComponents>);
			static_assert(!TypeListContains<AnimatorLayer, BuiltinComponents>);
			static_assert(!TypeListContains<TerrainLayer, BuiltinComponents>);
			static_assert(!TypeListContains<NavAgentType, BuiltinComponents>);
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
			CHECK(prefabOverride.Path.empty());
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
			CHECK_FALSE(PrefabLinkComponent{}.OuterPrefabEntityID.IsValid());
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
			CHECK_FALSE(point.CastShadows);
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

			// M26 (Docs/Decisions/0037-m26-contract.md decision 12): PostProcess version 2's defaults render as version 1's did.
			const PostProcessComponent post;
			CHECK(post.ExposureEV == 0.0f);
			CHECK(post.ExposureMode == ExposureMode::Manual);
			CHECK(post.AutoExposureMinEV == -6.0f);
			CHECK(post.AutoExposureMaxEV == 12.0f);
			CHECK(post.AutoExposureSpeedUp == 3.0f);
			CHECK(post.AutoExposureSpeedDown == 1.0f);
			CHECK(post.Tonemap == Tonemapper::AgX);
			CHECK_FALSE(post.Lut.IsValid());
			CHECK(post.LutIntensity == 1.0f);
			CHECK(post.SsaoEnabled);
			CHECK(post.SsaoRadius == 0.5f);
			CHECK(post.SsaoIntensity == 1.0f);
			CHECK(post.SsaoQuality == SsaoQuality::Medium);
			CHECK_FALSE(post.SsrEnabled);
			CHECK(post.SsrIntensity == 1.0f);
			CHECK(post.SsrMaxDistance == 50.0f);
			CHECK(post.SsrThickness == 0.2f);
			CHECK(post.SsrMaxRoughness == 0.6f);
			CHECK(post.BloomEnabled);
			CHECK(post.BloomIntensity == 0.04f);
			CHECK(post.AntiAliasing == AntiAliasing::Fxaa);

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

			CHECK(ScriptComponent{}.Scripts.empty());
			const ScriptSlot slot;
			CHECK_FALSE(slot.Script.IsValid());
			CHECK(slot.Fields.empty());
			CHECK(slot.ExecutionOrder == 0);
		}

		// M21 (Docs/Decisions/0032-m21-contract.md decisions 3, 5, 6 and 9).
		TEST_CASE("BuiltinComponents: 2D defaults follow the component table")
		{
			const SpriteRendererComponent sprite;
			CHECK_FALSE(sprite.Sheet.IsValid());
			CHECK(sprite.Frame == 0);
			CHECK_FALSE(sprite.Texture.IsValid());
			CHECK(sprite.Color == glm::vec4(1.0f));
			CHECK_FALSE(sprite.FlipX);
			CHECK_FALSE(sprite.FlipY);
			CHECK(sprite.PixelsPerUnit == 16.0f);
			CHECK(sprite.SortingLayer == "Default");
			CHECK(sprite.OrderInLayer == 0);
			CHECK_FALSE(sprite.Lit);
			CHECK_FALSE(sprite.NormalMap.IsValid());

			const SpriteAnimatorComponent animator;
			CHECK(animator.Clip.empty());
			CHECK(animator.Playing);
			CHECK(animator.Step == 0);
			CHECK(animator.StepTicks == 0);

			const PixelPerfectCameraComponent camera;
			CHECK(camera.ReferenceWidth == 320);
			CHECK(camera.ReferenceHeight == 180);
			CHECK(camera.PixelsPerUnit == 16.0f);
			CHECK(camera.LetterboxColor == glm::vec3(0.0f));

			// C21.2 (Docs/Decisions/0032-m21-contract.md decisions 20, 24 and 25).
			const Light2DComponent light;
			CHECK(light.Type == Light2DType::Point);
			CHECK(light.Color == glm::vec3(1.0f));
			CHECK(light.Intensity == 1.0f);
			CHECK(light.Radius == 5.0f);
			CHECK(light.Falloff == 0.5f);
			CHECK(light.Height == 1.0f);

			const TilemapRendererComponent tiles;
			CHECK_FALSE(tiles.Tilemap.IsValid());
			CHECK(tiles.Color == glm::vec4(1.0f));
			CHECK(tiles.PixelsPerUnit == 16.0f);
			CHECK(tiles.SortingLayer == "Default");
			CHECK(tiles.OrderInLayer == 0);
			CHECK_FALSE(tiles.Lit);

			const RigidBody2DComponent body;
			CHECK(body.Type == BodyType::Dynamic);
			CHECK(body.Mass == 1.0f);
			CHECK_FALSE(body.FixedRotation);
			CHECK(body.Layer == "Default");
			CHECK_FALSE(body.EnhancedInternalEdgeRemoval);
			CHECK(body.InitialLinearVelocity == glm::vec2(0.0f));
			CHECK(body.InitialAngularVelocity == 0.0f);
			CHECK(BoxCollider2DComponent{}.HalfExtents == glm::vec2(0.5f));
			CHECK(BoxCollider2DComponent{}.Angle == 0.0f);
			CHECK(CircleCollider2DComponent{}.Radius == 0.5f);
			const PolygonCollider2DComponent polygon;
			REQUIRE(polygon.Points.size() == 3);
			CHECK(polygon.Points[2] == glm::vec2(0.0f, 0.5f));
			CHECK_FALSE(polygon.IsTrigger);
			CHECK_FALSE(TilemapColliderComponent{}.Tilemap.IsValid());
			CHECK_FALSE(TilemapColliderComponent{}.IsTrigger);
		}

		// M22 (C22.1, Docs/Decisions/0033-m22-contract.md decision 2): the Ui category's defaults.
		TEST_CASE("BuiltinComponents: UI defaults follow the component table")
		{
			const UiCanvasComponent canvas;
			CHECK(canvas.SortOrder == 0);
			CHECK(canvas.ScaleMode == UiScaleMode::ConstantPixelSize);
			CHECK(canvas.ScaleFactor == 1.0f);
			CHECK(canvas.ReferenceHeight == 1080.0f);
			CHECK(canvas.PixelSnap);
			// C22.2 (decision 25): the canvas's theme.
			CHECK_FALSE(canvas.Font.IsValid());
			CHECK(canvas.Theme.Normal == glm::vec4(1.0f));
			CHECK(canvas.Theme.Hovered == glm::vec4(0.85f, 0.85f, 0.85f, 1.0f));
			CHECK(canvas.Theme.Pressed == glm::vec4(0.6f, 0.6f, 0.6f, 1.0f));
			CHECK(canvas.Theme.Focused == glm::vec4(0.75f, 0.85f, 1.0f, 1.0f));
			CHECK(canvas.Theme.Disabled == glm::vec4(0.5f, 0.5f, 0.5f, 0.5f));
			// C22.2 (decision 23): no default focus, so navigation starts only from a press or Ui.SetFocus.
			CHECK_FALSE(canvas.DefaultFocus.IsValid());
			// C22.3 (decision 49): a Screen canvas; a World canvas's size and scale (registered by stream W).
			CHECK(canvas.RenderMode == UiCanvasRenderMode::Screen);
			CHECK(canvas.WorldWidth == 1024u);
			CHECK(canvas.WorldHeight == 576u);
			CHECK(canvas.PixelsPerUnit == 512.0f);

			const UiRectComponent rect;
			CHECK(rect.AnchorMin == glm::vec2(0.5f));
			CHECK(rect.AnchorMax == glm::vec2(0.5f));
			CHECK(rect.Pivot == glm::vec2(0.5f));
			CHECK(rect.Position == glm::vec2(0.0f));
			CHECK(rect.Size == glm::vec2(100.0f));
			CHECK_FALSE(rect.ClipChildren);

			const UiImageComponent image;
			CHECK_FALSE(image.Sheet.IsValid());
			CHECK(image.Frame == 0);
			CHECK(image.Color == glm::vec4(1.0f));
			CHECK(image.Type == UiImageType::Simple);
			CHECK(image.BorderScale == 1.0f);
			CHECK(image.Filter == UiImageFilter::Point);
			CHECK(image.RaycastTarget);

			const UiTextComponent text;
			CHECK(text.Text.empty());
			CHECK_FALSE(text.Font.IsValid());
			CHECK(text.Size == 32.0f);
			CHECK(text.Color == glm::vec4(1.0f));
			CHECK(text.Alignment == TextAlignment::Left);
			CHECK(text.VerticalAlignment == UiTextVerticalAlignment::Top);
			CHECK(text.Wrap);
			CHECK(text.LineSpacing == 1.0f);
			CHECK(text.RaycastTarget);

			const UiLayoutComponent layout;
			CHECK(layout.Kind == UiLayoutKind::Stack);
			CHECK(layout.Direction == UiLayoutDirection::Vertical);
			CHECK(layout.Padding == UiPadding{});
			CHECK(layout.Spacing == glm::vec2(0.0f));
			CHECK(layout.MainAlignment == UiMainAlignment::Start);
			CHECK(layout.CrossAlignment == UiCrossAlignment::Start);
			CHECK(layout.Columns == 0);
			CHECK(layout.CellSize == glm::vec2(100.0f));
			CHECK_FALSE(layout.Wrap);

			const UiLayoutElementComponent element;
			CHECK_FALSE(element.IgnoreLayout);
			CHECK(element.Grow == 0.0f);
			CHECK(element.Shrink == 1.0f);
			CHECK(element.MinSize == glm::vec2(0.0f));

			// C22.2 (Docs/Decisions/0033-m22-contract.md decision 21): the widgets.
			const UiButtonComponent button;
			CHECK(button.Interactable);
			CHECK(button.Focusable);

			const UiToggleComponent toggle;
			CHECK(toggle.Interactable);
			CHECK(toggle.Focusable);
			CHECK_FALSE(toggle.IsOn);
			CHECK_FALSE(toggle.Graphic.IsValid());

			const UiSliderComponent slider;
			CHECK(slider.Interactable);
			CHECK(slider.Focusable);
			CHECK(slider.Value == 0.0f);
			CHECK(slider.Min == 0.0f);
			CHECK(slider.Max == 1.0f);
			CHECK_FALSE(slider.WholeNumbers);
			CHECK(slider.Step == 0.0f);
			CHECK(slider.Direction == UiLayoutDirection::Horizontal);
			CHECK_FALSE(slider.Fill.IsValid());
			CHECK_FALSE(slider.Handle.IsValid());

			const UiScrollViewComponent scrollView;
			CHECK(scrollView.Interactable);
			CHECK_FALSE(scrollView.Focusable);
			CHECK_FALSE(scrollView.Content.IsValid());
			CHECK_FALSE(scrollView.Horizontal);
			CHECK(scrollView.Vertical);
			CHECK(scrollView.ScrollPosition == glm::vec2(0.0f));
			CHECK(scrollView.ScrollStep == 40.0f);

			// C22.3 (decision 45): the text input.
			const UiTextInputComponent textInput;
			CHECK(textInput.Interactable);
			CHECK(textInput.Focusable);
			CHECK(textInput.Text.empty());
			CHECK(textInput.MaxLength == 256u);
			CHECK(textInput.Caret == 0u);
			CHECK_FALSE(textInput.TextElement.IsValid());
			CHECK_FALSE(textInput.Placeholder.IsValid());
		}

		// Several paths read only the added component's own Excludes (Entity:AddComponent, component.list and component.schema,
		// the generated reference), so both components of an exclusion declare it: RigidBody and CharacterController, UiCanvas
		// and UiRect (the C22.1 review, Docs/Decisions/0033-m22-contract.md decision 2), and each pair of C22.2's four widgets
		// (decision 21) and C22.3's text input (decision 45): five widgets, 20 ordered pairs; M37's four joints
		// (Docs/Decisions/0054-m37-joints.md decision 1): 12 more.
		TEST_CASE("BuiltinComponents: every exclusion is declared by both components")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			size_t exclusions = 0;
			for (const ComponentInfo* component : registry->GetComponents())
			{
				for (const ComponentInfo* excluded : component->GetExcludes())
				{
					INFO(component->GetName(), " excludes ", excluded->GetName());
					CHECK(std::find(excluded->GetExcludes().begin(), excluded->GetExcludes().end(), component) != excluded->GetExcludes().end());
					++exclusions;
				}
			}
			CHECK(exclusions == 36);
			const ComponentInfo* canvas = registry->FindComponent<UiCanvasComponent>();
			const ComponentInfo* rect = registry->FindComponent<UiRectComponent>();
			REQUIRE(canvas != nullptr);
			REQUIRE(rect != nullptr);
			CHECK(std::find(rect->GetExcludes().begin(), rect->GetExcludes().end(), canvas) != rect->GetExcludes().end());
		}

		// M23 (C23.3, Docs/Decisions/0034-m23-contract.md decision 28).
		TEST_CASE("BuiltinComponents: animation defaults follow the component table")
		{
			const AnimatorComponent animator;
			CHECK_FALSE(animator.Skeleton.IsValid());
			CHECK(animator.Layers.empty());
			CHECK(animator.Playing);
			CHECK(animator.RootMotion == AnimatorRootMotion::None);
			const AnimatorLayer layer;
			CHECK_FALSE(layer.Clip.IsValid());
			CHECK(layer.Time == 0.0f);
			CHECK(layer.Speed == 1.0f);
			CHECK(layer.Weight == 1.0f);
			CHECK(layer.Loop);

			CHECK(SkinnedMeshComponent{}.MorphWeights.empty());

			// M41 (Docs/Decisions/0058-m41-bone-attachments.md decision 3).
			const BoneAttachmentComponent attachment;
			CHECK(attachment.Bone.empty());
			CHECK(attachment.Offset == glm::vec3(0.0f));
			CHECK(attachment.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		}

		// M26 (C26.1, Docs/Decisions/0037-m26-contract.md decisions 4, 5 and 20).
		TEST_CASE("BuiltinComponents: particle, probe and terrain defaults follow the component table")
		{
			const ParticleEmitterComponent emitter;
			CHECK(emitter.Playing);
			CHECK(emitter.Loop);
			CHECK(emitter.Duration == 5.0f);
			CHECK(emitter.Rate == 10.0f);
			CHECK(emitter.Burst == 0);
			CHECK(emitter.MaxParticles == 256);
			CHECK(emitter.Seed == 0);
			CHECK(emitter.Lifetime == glm::vec2(1.0f, 2.0f));
			CHECK(emitter.Speed == glm::vec2(1.0f, 2.0f));
			CHECK(emitter.Size == glm::vec2(0.1f, 0.2f));
			CHECK(emitter.Shape == ParticleEmitterShape::Cone);
			CHECK(emitter.ShapeRadius == 0.25f);
			CHECK(emitter.ConeAngle == 25.0f);
			CHECK(emitter.BoxHalfExtents == glm::vec3(0.5f));
			CHECK(emitter.Acceleration == glm::vec3(0.0f));
			CHECK(emitter.Drag == 0.0f);
			CHECK(emitter.SimulationSpace == ParticleSimulationSpace::World);
			CHECK_FALSE(emitter.Texture.IsValid());
			CHECK(emitter.Blend == ParticleBlend::Alpha);
			CHECK(emitter.ColorOverLife.empty());
			CHECK(emitter.SizeOverLife.empty());

			const IrradianceVolumeComponent volume;
			CHECK_FALSE(volume.Data.IsValid());
			CHECK(volume.Size == glm::vec3(10.0f, 4.0f, 10.0f));
			CHECK(volume.ProbeCountX == 4);
			CHECK(volume.ProbeCountY == 3);
			CHECK(volume.ProbeCountZ == 4);
			CHECK(volume.Intensity == 1.0f);
			CHECK(volume.BlendDistance == 1.0f);

			const ReflectionProbeComponent probe;
			CHECK_FALSE(probe.Data.IsValid());
			CHECK(probe.Size == glm::vec3(10.0f, 5.0f, 10.0f));
			CHECK(probe.BoxProjection);
			CHECK(probe.Resolution == 128);
			CHECK(probe.Intensity == 1.0f);
			CHECK(probe.BlendDistance == 1.0f);

			// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the built-in Quad's face, through the origin, at half
			// resolution.
			const PlanarReflectionComponent reflection;
			CHECK(reflection.Normal == glm::vec3(0.0f, 0.0f, 1.0f));
			CHECK(reflection.Offset == 0.0f);
			CHECK(reflection.ResolutionScale == 0.5f);

			const TerrainComponent terrain;
			CHECK_FALSE(terrain.Heightfield.IsValid());
			CHECK(terrain.Size == glm::vec2(64.0f));
			CHECK(terrain.HeightScale == 16.0f);
			CHECK(terrain.Layers.empty());
			CHECK_FALSE(terrain.SplatMap.IsValid());
			CHECK(terrain.ReceiveShadows);
			CHECK(terrain.LodBias == 1.0f);
			const TerrainLayer layer;
			CHECK(layer.Tint == glm::vec3(1.0f));
			CHECK(layer.Tiling == 4.0f);
			CHECK(layer.Roughness == 0.9f);
			CHECK(layer.Metallic == 0.0f);

			const TerrainColliderComponent collider;
			CHECK(collider.Layer == "Default");
			CHECK(collider.Friction == 0.5f);
			CHECK(collider.Restitution == 0.0f);
		}

		// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 1): the defaults are neutral, so they draw as no
		// component (RenderExtraction's ResolveMaterialParameters).
		TEST_CASE("BuiltinComponents: material parameter defaults follow the component table")
		{
			const MaterialParametersComponent parameters;
			CHECK(parameters.Tint == glm::vec3(1.0f));
			CHECK(parameters.Emissive == glm::vec3(1.0f));
			CHECK(parameters.EmissiveStrength == 0.0f);
			CHECK(parameters.UVOffset == glm::vec2(0.0f));
			CHECK(parameters.UVScroll == glm::vec2(0.0f));
			CHECK(parameters.DissolveAmount == 0.0f);
			CHECK(parameters.DissolveEdgeColor == glm::vec3(1.0f, 0.5f, 0.1f));
			CHECK(parameters.DissolveEdgeStrength == 2.0f);
			CHECK(parameters.DissolveEdgeWidth == 0.05f);
			CHECK(parameters.DissolveNoiseScale == 1.0f);
			CHECK(parameters.Opacity == 1.0f);
		}

		// M47 (Docs/Decisions/0065-m47-water.md decision 3): an Ocean with a calm surface of the decision's look, and a physical
		// Buoyancy that takes the water holding each collider.
		TEST_CASE("BuiltinComponents: water defaults follow the component table")
		{
			const WaterComponent water;
			CHECK(water.Shape == WaterShape::Ocean);
			CHECK(water.Size == glm::vec2(20.0f));
			CHECK(water.Depth == 5.0f);
			CHECK(water.Headroom == 10.0f);
			CHECK(water.Waves.empty());
			CHECK(water.WaveScale == 1.0f);
			CHECK(water.Current == glm::vec2(0.0f));
			CHECK(water.Density == 1000.0f);
			CHECK(water.ShallowColor == glm::vec3(0.10f, 0.55f, 0.50f));
			CHECK(water.DeepColor == glm::vec3(0.01f, 0.07f, 0.12f));
			CHECK(water.Clarity == 4.0f);
			CHECK(water.Roughness == 0.05f);
			CHECK(water.RefractionStrength == 0.3f);
			CHECK(water.FoamAmount == 0.5f);
			CHECK(water.ShoreFoamDepth == 0.5f);
			CHECK(water.DetailNormalStrength == 0.5f);
			CHECK(water.DetailScale == 4.0f);
			CHECK(water.DetailDrift == glm::vec2(2.0f, 1.0f));
			CHECK(water.UnderwaterColor == glm::vec3(0.02f, 0.10f, 0.12f));
			CHECK(water.UnderwaterVisibility == 15.0f);
			CHECK_FALSE(water.NormalMap.IsValid());
			CHECK_FALSE(water.FoamTexture.IsValid());
			const WaterWave wave;
			CHECK(wave.Direction == 0.0f);
			CHECK(wave.Wavelength == 20.0f);
			CHECK(wave.Amplitude == 0.2f);
			CHECK(wave.Steepness == 0.5f);
			CHECK(wave.Speed == 0.0f);
			CHECK(wave.Phase == 0.0f);
			const BuoyancyComponent buoyancy;
			CHECK_FALSE(buoyancy.Water.IsValid());
			CHECK(buoyancy.BuoyancyScale == 1.0f);
			CHECK(buoyancy.LinearDrag == 2.0f);
			CHECK(buoyancy.AngularDrag == 0.05f);
		}

		// M47 (decision 3): Waves holds at most MaxWaterWaves waves, refused past it on every write path (the component's Validate
		// rule) and cut to it when generated; Buoyancy requires a RigidBody.
		TEST_CASE("BuiltinComponents: a Water holds at most eight waves and a Buoyancy requires a RigidBody")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* water = registry->FindComponent<WaterComponent>();
			REQUIRE(water != nullptr);
			CHECK(water->GetCategory() == "Water");
			WaterComponent component;
			component.Waves.resize(MaxWaterWaves);
			ResolveContext resolve;
			resolve.Registry = registry.get();
			ValidationContext full;
			water->Validate(&component, resolve, full);
			CHECK_FALSE(full.HasErrors());
			component.Waves.resize(MaxWaterWaves + 1);
			ValidationContext over;
			water->Validate(&component, resolve, over);
			CHECK(over.HasErrors());
			const ComponentInfo* buoyancy = registry->FindComponent<BuoyancyComponent>();
			REQUIRE(buoyancy != nullptr);
			const std::span<const ComponentInfo* const> required = buoyancy->GetRequires();
			REQUIRE(required.size() == 1);
			CHECK(required[0]->GetName() == "RigidBody");
		}

		// M33 (Docs/Decisions/0048-m33-particle-look.md decision 1): every default reproduces the look of an emitter without one.
		TEST_CASE("BuiltinComponents: a ParticleLook's defaults follow the component table and change nothing")
		{
			const ParticleLookComponent look;
			CHECK_FALSE(look.SpriteSheet.IsValid());
			CHECK(look.FirstFrame == 0);
			CHECK(look.FrameCount == 0);
			CHECK(look.FrameMode == ParticleFrameMode::OverLife);
			CHECK(look.FrameCycles == 1.0f);
			CHECK_FALSE(look.BlendFrames);
			CHECK(look.StartRotation == glm::vec2(0.0f));
			CHECK(look.Spin == glm::vec2(0.0f));
			CHECK(look.Alignment == ParticleAlignment::Camera);
			CHECK(look.VelocityStretch == 0.1f);
			CHECK(look.SoftDistance == 0.0f);
			CHECK(look.Intensity == 1.0f);
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
				// M36 (Docs/Decisions/0053-m36-contact-surfaces.md decision 1): in Physics, after CharacterController.
				"ColliderSurface",
				"AudioSource",
				"AudioListener",
				"Script",
				// M21 (Docs/Decisions/0032-m21-contract.md decision 2).
				"SpriteRenderer",
				"SpriteAnimator",
				"PixelPerfectCamera",
				"Light2D",         // C21.2
				"TilemapRenderer", // C21.2
				"RigidBody2D",
				"BoxCollider2D",
				"CircleCollider2D",
				"PolygonCollider2D",
				"TilemapCollider", // C21.2
				// M22 (Docs/Decisions/0033-m22-contract.md decision 2), after M21's categories (landing order).
				"UiCanvas",
				"UiRect",
				"UiImage",
				"UiText",
				"UiLayout",
				"UiLayoutElement",
				"UiButton",
				"UiToggle",
				"UiSlider",
				"UiScrollView",
				"UiTextInput",
				// M23 (Docs/Decisions/0034-m23-contract.md decision 28): after M21's and M22's categories on phase 2's merged tree.
				"Animator",
				"SkinnedMesh",
				// M41 (Docs/Decisions/0058-m41-bone-attachments.md decision 3): in Animation, after SkinnedMesh.
				"BoneAttachment",
				// M26 (Docs/Decisions/0037-m26-contract.md decision 4): ADR 0028's phase-3 ledger's categories after Animation,
				// Particles, Probes and Terrain in that order. M33's ParticleLook follows the emitter (Docs/Decisions/0048-m33-particle-look.md
				// decision 1).
				"ParticleEmitter",
				"ParticleLook",
				"IrradianceVolume",
				"ReflectionProbe",
				// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): after the ReflectionProbe in Probes.
				"PlanarReflection",
				"Terrain",
				"TerrainCollider",
				// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 1): ADR 0042's phase-5 ledger's categories after
				// Terrain, in landing order.
				"MaterialParameters",
				// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 2): ADR 0042's phase-5 ledger's Fog category after Terrain.
				"Fog",
				// M30 (Docs/Decisions/0045-m30-navigation.md decision 5): ADR 0042's phase-5 ledger's categories after Terrain, in
				// landing order.
				"Navigation",
				"NavModifier",
				// M37 (Docs/Decisions/0054-m37-joints.md decision 1): ADR 0052's phase-6 ledger's Joints category after Navigation.
				"HingeJoint",
				"SliderJoint",
				"DistanceJoint",
				"FixedJoint",
				// M47 (Docs/Decisions/0065-m47-water.md decision 3): phase 8's ledger's Water category, last.
				"Water",
				"Buoyancy",
			};
			std::vector<std::string> names;
			for (const ComponentInfo* component : registry->GetComponents())
				names.push_back(component->GetName());
			CHECK(names == expected);
		}

		TEST_CASE("BuiltinComponents: every component has a category, a version and descriptions")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::vector<std::string> categories = { "Core", "Rendering", "Physics", "Audio", "Scripting", "Rendering2D", "Physics2D", "Ui",
				"Animation", "Particles", "Probes", "Terrain", "MaterialParameters", "Fog", "Navigation", "Joints", "Water" };
			// The components that migrated since their first version: Prefab's override paths and Script's list of slots (M20,
			// Docs/Decisions/0031-m20-contract.md decisions 10 and 17), PointLight's CastShadows (M23,
			// Docs/Decisions/0034-m23-contract.md decision 3) and PostProcess's AntiAliasing, exposure, LUT and SSR members (M26,
			// Docs/Decisions/0037-m26-contract.md decision 12). Every other component is at version 1.
			const std::map<std::string, uint32_t> migrated = { { "PointLight", 2 }, { "PostProcess", 2 }, { "Prefab", 2 }, { "Script", 2 } };
			for (const ComponentInfo* component : registry->GetComponents())
			{
				INFO(component->GetName());
				CHECK(std::find(categories.begin(), categories.end(), component->GetCategory()) != categories.end());
				const auto version = migrated.find(component->GetName());
				CHECK(component->GetVersion() == (version != migrated.end() ? version->second : 1));
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
