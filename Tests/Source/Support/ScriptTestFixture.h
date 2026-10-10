#pragma once

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Random.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	namespace Test {

		struct ScriptTestFixtureSpecification
		{
			uint64_t Seed = 17;
			ScriptingSettings Settings{};
			RunModes Mode = RunModes::Editor;
			bool TestMode = false;
			bool ReadOnly = false;
			IScriptTestHost* TestHost = nullptr; // borrowed, outlives the fixture's engine
			// Add a native observation function before Freeze. The built-in registrations are already present.
			std::function<Status(ScriptApiRegistry&)> ConfigureApi{};
			// Deterministic safety-clock injection; copied into the engine, captures outlive the fixture.
			std::function<double()> ClockSeconds{};
		};

		// Real compiler/extraction, immutable memory assets, and one runtime engine. Host requests are observed rather
		// than serviced by an application loop. Tests of phase ordering, scene replacement, audio and physics use a
		// real PlaySession instead. All borrowed fixture services outlive the engine, which is destroyed first.
		class ScriptTestFixture final : public IScriptHost, public IScriptModuleReader
		{
		public:
			explicit ScriptTestFixture(const ScriptTestFixtureSpecification& specification = {});
			~ScriptTestFixture() override;
			ScriptTestFixture(const ScriptTestFixture&) = delete;
			ScriptTestFixture& operator=(const ScriptTestFixture&) = delete;

			// Source paths are Assets/... . A failed extraction is returned without replacing a published asset/schema.
			void SetSource(std::string path, std::string source);
			[[nodiscard]] Result<AssetRef<ScriptData>> CompileScript(AssetHandle handle, std::string_view path);
			[[nodiscard]] Result<AssetRef<ScriptData>> AddScript(AssetHandle handle, std::string path, std::string source);
			[[nodiscard]] Status Start();
			void Stop();
			[[nodiscard]] ScriptEngine* GetEngine() { return m_Engine.get(); }
			[[nodiscard]] ScriptApiRegistry& GetApi() { return m_Api; }
			[[nodiscard]] InMemoryAssetManager& GetAssetManager() { return m_Assets; }
			[[nodiscard]] Ref<const ScriptFieldSchemaSource> GetSchemaSnapshot() const { return m_Schemas; }
			[[nodiscard]] Result<ScriptEvaluation> Evaluate(std::string_view source, std::optional<UUID> entity = std::nullopt);
			[[nodiscard]] Result<std::string> ReadModule(const VfsPath& path) override;

			[[nodiscard]] Scene& GetScene() override { return m_Scene.GetScene(); }
			[[nodiscard]] const TypeRegistry& GetTypes() const override;
			[[nodiscard]] AssetManager* GetAssets() override { return &m_Assets; }
			[[nodiscard]] const IFieldSchemaSource* GetFieldSchemas() const override { return m_Schemas.get(); }
			[[nodiscard]] PhysicsSystem* GetPhysics() override { return Physics; }
			[[nodiscard]] AudioSystem* GetAudio() override { return Audio; }
			[[nodiscard]] DebugDrawList* GetDebugDraw() override { return &DebugDraw; }
			[[nodiscard]] Random& GetRandom() override { return m_Random; }
			[[nodiscard]] const InputState& GetInput() const override { return Input; }
			[[nodiscard]] ScriptFrameState GetFrameState() const override { return Frame; }
			[[nodiscard]] ScriptEnvironment GetEnvironment() const override { return Environment; }
			[[nodiscard]] uint64_t GetSceneGeneration() const override { return Generation; }
			[[nodiscard]] const Json& GetLoadParameters() const override { return Parameters; }
			[[nodiscard]] Result<ScriptActionState> GetAction(std::string_view name) const override;
			[[nodiscard]] Result<UUID> CreateEntity(std::string_view name, UUID parent) override;
			[[nodiscard]] Result<UUID> Instantiate(AssetHandle prefab, const std::optional<glm::vec3>& position,
				const std::optional<glm::quat>& rotation, UUID parent) override;
			void MarkTeleported(UUID entity) override;
			[[nodiscard]] Status SetTimeScale(double scale) override;
			[[nodiscard]] Status SetCursorMode(CursorMode mode) override;
			[[nodiscard]] CursorMode GetCursorMode() const override { return Cursor; }
			[[nodiscard]] Status RequestSceneLoad(AssetHandle scene, Json parameters) override;
			void RequestQuit(int32_t exitCode) override { QuitCode = exitCode; }
			void RequestPause() override { PauseRequested = true; }
			void OnScriptError(const ScriptError& error, bool fatal) override;
			void OnExternalMutation(std::string_view reason) override;
			[[nodiscard]] bool IsReloadDeferred() const override { return ReloadDeferred; }

			ScriptFrameState Frame{};
			ScriptEnvironment Environment{ .IsEditor = true, .Platform = "Test", .Version = "1" };
			uint64_t Generation = 1;
			InputState Input{};
			DebugDrawList DebugDraw{};
			// Optional test services over GetScene(); callers stop the VM before destroying these borrowed systems.
			PhysicsSystem* Physics = nullptr;
			AudioSystem* Audio = nullptr;
			Json Parameters = Json::object();
			std::map<std::string, ScriptActionState> Actions{};
			CursorMode Cursor = CursorMode::Normal;
			uint32_t EntityLimit = 1000;
			bool ReloadDeferred = false;
			bool PauseRequested = false;
			std::optional<int32_t> QuitCode{};
			AssetHandle RequestedScene{};
			Json RequestedParameters{};
			std::vector<ScriptError> Errors{};
			bool FatalError = false;
			std::vector<std::string> ExternalMutations{};
			std::vector<UUID> Teleported{};
		private:
			ScriptTestFixtureSpecification m_Specification{};
			SceneTestFixture m_Scene;
			ScriptApiRegistry m_Api{};
			InMemoryAssetManager m_Assets{};
			Random m_Random;
			std::map<std::string, std::string> m_Sources{};
			std::map<AssetHandle, AssetRef<ScriptData>> m_Scripts{};
			Ref<const ScriptFieldSchemaSource> m_Schemas{};
			Scope<ScriptEngine> m_Engine{};
		};

	}

}
