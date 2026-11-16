#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Reflection/TypeList.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/AudioListenerComponent.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/FogComponent.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/IrradianceVolumeComponent.h"
#include "Engine/Scene/Components/Light2DComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/NameComponent.h"
#include "Engine/Scene/Components/ParticleEmitterComponent.h"
#include "Engine/Scene/Components/PixelPerfectCameraComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/PolygonCollider2DComponent.h"
#include "Engine/Scene/Components/PostProcessComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/ReflectionProbeComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Components/SpriteAnimatorComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Components/TagsComponent.h"
#include "Engine/Scene/Components/TerrainColliderComponent.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Components/TextComponent.h"
#include "Engine/Scene/Components/TilemapColliderComponent.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"
#include "Engine/Scene/Components/UiLayoutElementComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"

namespace Engine {

	class TypeRegistry;

	// The one list of reflected component types (Architecture §5.4), in registration order, which is the §5.3 table order
	// and the canonical order of components in files ("Components" and "ComponentVersions" keys, §6). Adding a component
	// means adding it here, in its category's registration file and to its category's position below; the registry suite
	// then tests it with no further code. Shared integration file: one owner per milestone (Roadmap rule 4).
	using BuiltinComponents = TypeList<
		// Core (Registration/CoreRegistration.cpp)
		IDComponent, NameComponent, TagsComponent, RelationshipComponent, TransformComponent, PrefabInstanceComponent,
		PrefabLinkComponent,
		// Rendering (Registration/RenderingRegistration.cpp)
		MeshRendererComponent, CameraComponent, DirectionalLightComponent, PointLightComponent, SpotLightComponent,
		EnvironmentComponent, PostProcessComponent, TextComponent,
		// Physics (Registration/PhysicsRegistration.cpp)
		RigidBodyComponent, BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent, MeshColliderComponent,
		CharacterControllerComponent,
		// Audio (Registration/AudioRegistration.cpp)
		AudioSourceComponent, AudioListenerComponent,
		// Scripting (Registration/ScriptingRegistration.cpp)
		ScriptComponent,
		// Rendering2D (Registration/Rendering2DRegistration.cpp; M21, Docs/Decisions/0032-m21-contract.md decisions 3, 5, 6,
		// 20 and 24)
		SpriteRendererComponent, SpriteAnimatorComponent, PixelPerfectCameraComponent, Light2DComponent, TilemapRendererComponent,
		// Physics2D (Registration/Physics2DRegistration.cpp; M21, Docs/Decisions/0032-m21-contract.md decisions 9 and 25)
		RigidBody2DComponent, BoxCollider2DComponent, CircleCollider2DComponent, PolygonCollider2DComponent, TilemapColliderComponent,
		// Ui (Registration/UiRegistration.cpp; M22, Docs/Decisions/0033-m22-contract.md decision 2: phase 2's categories go
		// after Scripting in landing order, ADR 0028's phase-2 ledger, so after M21's; C22.2's widgets appended, decision 21;
		// C22.3's text input appended, decision 45)
		UiCanvasComponent, UiRectComponent, UiImageComponent, UiTextComponent, UiLayoutComponent, UiLayoutElementComponent,
		UiButtonComponent, UiToggleComponent, UiSliderComponent, UiScrollViewComponent, UiTextInputComponent,
		// Animation (Registration/AnimationRegistration.cpp; M23, Docs/Decisions/0034-m23-contract.md decision 28). ADR 0028's
		// phase-2 ledger: a milestone's new components go in categories of their own after Scripting, appended in landing
		// order, so M23, the last lander, appended this category after M21's and M22's at its merge.
		AnimatorComponent, SkinnedMeshComponent,
		// Particles (Registration/ParticlesRegistration.cpp), Probes (Registration/ProbesRegistration.cpp) and Terrain
		// (Registration/TerrainRegistration.cpp; M26, Docs/Decisions/0037-m26-contract.md decision 4). ADR 0028's phase-3
		// ledger: M26's components go in categories of their own after Animation, in that order; M24 adds none.
		ParticleEmitterComponent, IrradianceVolumeComponent, ReflectionProbeComponent, TerrainComponent, TerrainColliderComponent,
		// Fog (Registration/FogRegistration.cpp; M32, Docs/Decisions/0047-m32-fog-ssr.md decision 2). ADR 0042's phase-5 ledger:
		// M30's Navigation, M31's MaterialParameters and M32's Fog categories go after Terrain, in landing order.
		FogComponent>;

	// The category registration functions, each registering its enums, structs and components in BuiltinComponents order
	// through RegisterComponent<T> (Scene/ComponentRegistration.h). Each is called exactly once per registry, in this
	// order, by RegisterBuiltinComponents.
	void RegisterCoreComponents(TypeRegistry& registry);
	void RegisterRenderingComponents(TypeRegistry& registry);
	void RegisterPhysicsComponents(TypeRegistry& registry);
	void RegisterAudioComponents(TypeRegistry& registry);
	void RegisterScriptingComponents(TypeRegistry& registry);
	// M21 (ADR 0028's phase-2 ledger: a milestone's new components go in categories of their own after Scripting).
	void RegisterRendering2DComponents(TypeRegistry& registry);
	void RegisterPhysics2DComponents(TypeRegistry& registry);
	// M22 (ADR 0028's phase-2 ledger: after M21's categories, in landing order).
	void RegisterUiComponents(TypeRegistry& registry);
	// M23 (Docs/Decisions/0034-m23-contract.md decision 28), after M21's and M22's categories.
	void RegisterAnimationComponents(TypeRegistry& registry);
	// M26 (Docs/Decisions/0037-m26-contract.md decision 4), after Animation, in ADR 0028's phase-3 ledger's order.
	void RegisterParticlesComponents(TypeRegistry& registry);
	void RegisterProbesComponents(TypeRegistry& registry);
	void RegisterTerrainComponents(TypeRegistry& registry);
	// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 2), after Terrain, in ADR 0042's phase-5 ledger's landing order.
	void RegisterFogComponents(TypeRegistry& registry);

	// Registers every built-in component (the thirteen functions above, in order) into a registry that is not frozen yet, then
	// asserts that every BuiltinComponents type is registered and that the registration order equals the list order
	// (the startup check of §5.4). The caller adds its own types (project settings, automation structs) and then freezes
	// the registry.
	void RegisterBuiltinComponents(TypeRegistry& registry);

}
