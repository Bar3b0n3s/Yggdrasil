#include "TestsPCH.h"
#include "Support/TestGame.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/PakFormat.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/AssetPipeline/PakWriter.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Project/GameManifest.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/quaternion.hpp>

namespace Engine {

	namespace Test {

		namespace {

			constexpr std::string_view StartScenePath = "Assets/Scenes/Main.scene";

#if defined(ENGINE_DEBUG)
			constexpr std::string_view ConfigurationName = "Debug";
#elif defined(ENGINE_RELEASE)
			constexpr std::string_view ConfigurationName = "Release";
#else
			constexpr std::string_view ConfigurationName = "Dist";
#endif

			// The start scene's canonical document: a primary camera, a sun and the built-in cube.
			Result<Json> MakeStartScene()
			{
				SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				scene.SetName("Main");
				Entity camera = scene.CreateEntity("Camera");
				camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 1.5f, 5.0f);
				camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true, .Clear = ClearMode::Color, .ClearColor = glm::vec3(0.1f, 0.1f, 0.12f) });
				Entity sun = scene.CreateEntity("Sun");
				sun.GetComponent<TransformComponent>().Rotation = glm::quat(glm::radians(glm::vec3(-50.0f, -30.0f, 0.0f)));
				sun.AddComponent<DirectionalLightComponent>();
				Entity cube = scene.CreateEntity("Cube");
				cube.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
				return SceneSerializer::ToJson(scene);
			}

			// The XXH64 of a written file.
			Result<uint64_t> HashFile(const std::filesystem::path& path)
			{
				ENGINE_TRY_ASSIGN(const Buffer bytes, FileSystem::ReadFile(path));
				return XXH64(std::span<const std::byte>(bytes.data(), bytes.size()));
			}

			Status WriteEnginePak(const std::filesystem::path& path, bool shaders)
			{
				PakWriter writer;
				if (shaders)
				{
					// ListDirectory returns paths that start with the directory, so each file's relative path follows its prefix.
					const std::filesystem::path root(ENGINE_SHADER_DIRECTORY);
					const std::string rootText = FileSystem::PathToUtf8(root);
					ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> files, FileSystem::ListDirectory(root, true));
					for (const std::filesystem::path& file : files)
					{
						const std::string text = FileSystem::PathToUtf8(file);
						if ((!text.ends_with(".spv") && !text.ends_with(".refl.json")) || !text.starts_with(rootText))
							continue;
						std::string relative = text.substr(rootText.size());
						while (!relative.empty() && relative.front() == '/')
							relative.erase(relative.begin());
						ENGINE_TRY_ASSIGN(Buffer bytes, FileSystem::ReadFile(file));
						ENGINE_TRY(writer.Add(PakWriterEntry{ .Handle = {}, .Type = std::string(PakFileEntryType), .Path = "Shaders/" + relative, .Data = std::move(bytes) }));
					}
				}
				Json metadata = Json::object();
				metadata["Configuration"] = std::string(ConfigurationName);
				metadata["EngineVersion"] = std::string(EngineVersionString);
				writer.SetMetadata(VariantValue(std::move(metadata)));
				return writer.WriteToFile(path);
			}

			Status WriteGamePak(const std::filesystem::path& path, const ProjectSettings& project, const TypeRegistry& registry)
			{
				ENGINE_TRY_ASSIGN(const Json scene, MakeStartScene());
				ENGINE_TRY_ASSIGN(Buffer cooked, CookDocument(AssetType::Scene, scene, 1));
				PakWriter writer;
				ENGINE_TRY(writer.Add(PakWriterEntry{ .Handle = TestGameStartScene, .Type = "Scene", .Path = std::string(StartScenePath), .Data = std::move(cooked) }));
				ENGINE_TRY_ASSIGN(Json settings, ProjectSerializer::ToJson(project, registry));
				Json metadata = Json::object();
				metadata["Project"] = std::move(settings);
				writer.SetMetadata(VariantValue(std::move(metadata)));
				return writer.WriteToFile(path);
			}

		}

		Result<std::filesystem::path> WriteTestGame(const std::filesystem::path& directory, const TestGameSpecification& specification)
		{
			const Scope<TypeRegistry> registry = CreateBuiltinRegistry();
			ProjectSettings project;
			project.Name = specification.Name;
			project.StartScene = std::string(StartScenePath);
			project.Window.Title = specification.Name;
			project.Window.Width = specification.Width;
			project.Window.Height = specification.Height;
			project.Simulation.FixedHz = specification.FixedHz;
			project.Simulation.Seed = specification.Seed;
			project.Export.BuildScenes = { std::string(StartScenePath) };

			const std::filesystem::path data = directory / "Data";
			ENGINE_TRY(FileSystem::CreateDirectories(data));
			ENGINE_TRY(WriteEnginePak(data / "Engine.pak", specification.Shaders));
			ENGINE_TRY(WriteGamePak(data / "Game.pak", project, *registry));

			GameManifest manifest;
			manifest.Name = specification.Name;
			manifest.EngineVersion = std::string(EngineVersionString);
			manifest.StartScene = TestGameStartScene;
			ENGINE_TRY_ASSIGN(const uint64_t engineHash, HashFile(data / "Engine.pak"));
			ENGINE_TRY_ASSIGN(const uint64_t gameHash, HashFile(data / "Game.pak"));
			manifest.Paks = { GameManifestPak{ .Path = "Data/Engine.pak", .Hash = engineHash }, GameManifestPak{ .Path = "Data/Game.pak", .Hash = gameHash } };
			manifest.Window = project.Window;
			manifest.Simulation = project.Simulation;
			ENGINE_TRY_ASSIGN(const std::string text, GameManifestSerializer::SaveToString(manifest));
			const std::filesystem::path manifestPath = directory / std::string(GameManifest::FileName);
			ENGINE_TRY(FileSystem::WriteFileAtomic(manifestPath, std::as_bytes(std::span(text.data(), text.size()))));
			return manifestPath;
		}

	}

}
