#include "EditorPCH.h"
#include "EditorCore/Project/Basic3DTemplate.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/AudioListenerComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PostProcessComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/TransformSystem.h"

#include <nlohmann/json.hpp>

namespace Engine {

	Result<Json> BuildBasic3DScene(const TypeRegistry& registry, UUIDGenerator& ids)
	{
		for (const std::string_view name : { "Camera", "AudioListener", "DirectionalLight", "Environment", "PostProcess", "MeshRenderer", "RigidBody", "BoxCollider", "Transform" })
			if (!registry.FindComponent(name))
				return MakeError(ErrorCode::Validation, "Basic3D requires the registered component '{}'", name);
		Scene scene({ .Name = "Main", .Registry = &registry, .IdGenerator = &ids });
		const Entity camera = scene.CreateEntity("Camera");
		camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
		camera.AddComponent<AudioListenerComponent>();
		camera.Patch<TransformComponent>([](TransformComponent& transform)
		{
			transform.Translation = glm::vec3(0.0f, 3.0f, 6.0f);
			transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(-20.0f, 0.0f, 0.0f));
		});
		const Entity sun = scene.CreateEntity("Sun");
		sun.AddComponent<DirectionalLightComponent>();
		sun.Patch<TransformComponent>([](TransformComponent& transform)
		{
			transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(-45.0f, -30.0f, 0.0f));
		});
		const Entity environment = scene.CreateEntity("Environment");
		environment.AddComponent<EnvironmentComponent>(EnvironmentComponent{ .Environment = TypedAssetHandle<AssetType::Environment>(BuiltinAssetHandles::StudioEnvironment) });
		scene.CreateEntity("Post Process").AddComponent<PostProcessComponent>();
		const Entity ground = scene.CreateEntity("Ground");
		ground.AddComponent<MeshRendererComponent>(MeshRendererComponent{
			.Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh),
			.Materials = { TypedAssetHandle<AssetType::Material>(BuiltinAssetHandles::DefaultMaterial) } });
		ground.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
		ground.AddComponent<BoxColliderComponent>();
		ground.Patch<TransformComponent>([](TransformComponent& transform)
		{
			transform.Translation = glm::vec3(0.0f, -0.25f, 0.0f);
			transform.Scale = glm::vec3(10.0f, 0.5f, 10.0f);
		});
		return SceneSerializer::ToJson(scene);
	}

	Result<CreatedProject> CreateBasic3DProject(const ProjectCreateSpecification& specification, const TypeRegistry& registry)
	{
		ProjectCreateSpecification basic = specification;
		basic.Template = ProjectTemplate::Basic3D;
		return ProjectManager::CreateProject(basic, registry);
	}

}
