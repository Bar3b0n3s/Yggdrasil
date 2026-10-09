#include "TestsPCH.h"
#include "EditorCore/Project/Basic3DTemplate.h"

#include "EditorCore/Project/ProjectValidator.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
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
#include "Engine/Scene/SceneSerializer.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static ProjectCreateSpecification ContentBasicSpecification(const Test::EditorTestFixture& fixture)
	{
		return { .Directory = fixture.GetProjectRoot(), .Name = "TestProject", .Template = ProjectTemplate::Basic3D, .TemplatesDirectory = Test::GetRepositoryRoot() / "Resources/Templates/Projects" };
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("Basic3DTemplate: the initial scene validates without errors or listener warnings")
		{
			Test::EditorTestFixture fixture("BasicValidation");
			// The standard CPU fixture deliberately has no shipped resources. Basic3D resolves the real Studio catalogue.
			auto engine = EngineContext::Create({ .EngineResourcesDirectory = Test::GetRepositoryRoot() / "Resources", .RegisterTypes = &RegisterEditorMethodTypes });
			REQUIRE(engine);
			auto context = EditorContext::Create(**engine, { .IdGeneratorState = Test::EditorTestIdState });
			REQUIRE(context);
			auto& editor = **context;
			const auto created = ProjectManager::CreateProject(ContentBasicSpecification(fixture), editor.GetTypeRegistry());
			REQUIRE(created);
			CHECK(created->RecordedFiles.size() == 2);
			for (const auto& file : created->RecordedFiles)
			{
				const auto contents = FileSystem::ReadFile(fixture.GetProjectRoot() / FileSystem::PathFromUtf8(file.Path));
				REQUIRE(contents);
				CHECK(XXH64(*contents) == file.Hash);
			}
			auto opened = ProjectManager::OpenProject(created->ProjectFile, {}, editor.GetTypeRegistry());
			REQUIRE(opened);
			REQUIRE(editor.OpenProject(std::move(*opened)));
			CHECK(editor.GetProject().GetSettings().StartScene == "Assets/Scenes/Main.scene");
			CHECK(editor.GetProject().GetSettings().Export.BuildScenes == std::vector<std::string>{ "Assets/Scenes/Main.scene" });
			const auto report = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE(report);
			for (const auto& diagnostic : report->Diagnostics)
			{
				INFO(diagnostic.Code, ": ", diagnostic.Message);
				CHECK(diagnostic.Severity != DiagnosticSeverity::Error);
				CHECK(diagnostic.Code != "AUDIO_NO_LISTENER");
				CHECK(diagnostic.Code != "AUDIO_MULTIPLE_PRIMARY_LISTENERS");
			}
			CHECK(report->ErrorCount == 0);
			CHECK_FALSE(FileSystem::Exists(fixture.GetProjectRoot() / "Assets/Textures/Studio.hdr"));
		}

		TEST_CASE("Basic3DTemplate: camera sun environment post-process and ground use built-ins")
		{
			Test::EditorTestFixture fixture("BasicComposition");
			auto& editor = fixture.GetEditor();
			UUIDGenerator ids = UUIDGenerator::CreateDeterministic(42);
			UUIDGenerator sameIds = UUIDGenerator::CreateDeterministic(42);
			const auto document = BuildBasic3DScene(editor.GetTypeRegistry(), ids);
			REQUIRE(document);
			const auto repeated = BuildBasic3DScene(editor.GetTypeRegistry(), sameIds);
			REQUIRE(repeated);
			CHECK(*repeated == *document);
			auto scene = editor.CreateScene("Scratch");
			LoadReport load;
			REQUIRE(SceneSerializer::FromJson(*scene, *document, {}, load));
			CHECK(scene->GetEntityCount() == 5);
			uint32_t cameras = 0, suns = 0, environments = 0, post = 0, grounds = 0;
			for (const UUID id : scene->GetCanonicalOrder())
			{
				const Entity entity = scene->FindEntityByID(id);
				if (entity.HasComponent<CameraComponent>())
				{
					++cameras;
					CHECK(entity.GetComponent<CameraComponent>().Primary);
					CHECK(entity.HasComponent<AudioListenerComponent>());
					CHECK(entity.GetComponent<AudioListenerComponent>().Primary);
					CHECK(entity.GetComponent<TransformComponent>().Translation.y > 0.0f);
				}
				if (entity.HasComponent<DirectionalLightComponent>())
					++suns;
				if (entity.HasComponent<EnvironmentComponent>())
				{
					++environments;
					CHECK(entity.GetComponent<EnvironmentComponent>().Environment.GetHandle() == BuiltinAssetHandles::StudioEnvironment);
				}
				if (entity.HasComponent<PostProcessComponent>())
					++post;
				if (entity.HasComponent<MeshRendererComponent>())
				{
					++grounds;
					CHECK(entity.GetComponent<MeshRendererComponent>().Mesh.GetHandle() == BuiltinAssetHandles::CubeMesh);
					CHECK(entity.GetComponent<MeshRendererComponent>().Materials.front().GetHandle() == BuiltinAssetHandles::DefaultMaterial);
					CHECK(entity.GetComponent<RigidBodyComponent>().Type == BodyType::Static);
					CHECK(entity.HasComponent<BoxColliderComponent>());
					CHECK(entity.GetComponent<TransformComponent>().Scale.y > 0.0f);
				}
			}
			CHECK(cameras == 1);
			CHECK(suns == 1);
			CHECK(environments == 1);
			CHECK(post == 1);
			CHECK(grounds == 1);
			CHECK_FALSE(document->dump().contains("Script"));
		}

		TEST_CASE("Basic3DTemplate: creation rolls back every file after a failure")
		{
			Test::EditorTestFixture fixture("BasicRollback");
			auto specification = ContentBasicSpecification(fixture);
			specification.TemplatesDirectory = fixture.GetDirectory() / "BrokenTemplates";
			// This template creates a directory at the future .eproj path, failing only after the scene was written.
			REQUIRE(FileSystem::CreateDirectories(specification.TemplatesDirectory / "Basic3D/TestProject.eproj"));
			const auto& registry = fixture.GetEditor().GetTypeRegistry();
			const auto result = CreateBasic3DProject(specification, registry);
			REQUIRE_FALSE(result);
			CHECK_FALSE(FileSystem::Exists(specification.Directory));
			REQUIRE(FileSystem::CreateDirectories(specification.Directory));
			CHECK_FALSE(ProjectManager::CreateProject(specification, registry));
			CHECK(FileSystem::Exists(specification.Directory));
			const auto remaining = FileSystem::ListDirectory(specification.Directory);
			REQUIRE(remaining);
			CHECK(remaining->empty());
		}
	}

}
