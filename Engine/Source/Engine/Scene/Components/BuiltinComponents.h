#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Reflection/TypeList.h"
#include "Engine/Scene/Components/AudioListenerComponent.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/NameComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/PostProcessComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Components/TagsComponent.h"
#include "Engine/Scene/Components/TextComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"

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
		ScriptComponent>;

	// The category registration functions, each registering its enums, structs and components in BuiltinComponents order
	// through RegisterComponent<T> (Scene/ComponentRegistration.h). Each is called exactly once per registry, in this
	// order, by RegisterBuiltinComponents.
	void RegisterCoreComponents(TypeRegistry& registry);
	void RegisterRenderingComponents(TypeRegistry& registry);
	void RegisterPhysicsComponents(TypeRegistry& registry);
	void RegisterAudioComponents(TypeRegistry& registry);
	void RegisterScriptingComponents(TypeRegistry& registry);

	// Registers every built-in component (the five functions above, in order) into a registry that is not frozen yet, then
	// asserts that every BuiltinComponents type is registered and that the registration order equals the list order
	// (the startup check of §5.4). The caller adds its own types (project settings, automation structs) and then freezes
	// the registry.
	void RegisterBuiltinComponents(TypeRegistry& registry);

}
