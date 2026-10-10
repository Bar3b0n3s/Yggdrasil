#include "TestsPCH.h"
#include "Support/ScriptTestFixture.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/LoadTimeVm.h"
#include "Engine/Scripting/RegisterBindings.h"

#include <cmath>
#include <utility>

namespace Engine {

	namespace Test {

		ScriptTestFixture::ScriptTestFixture(const ScriptTestFixtureSpecification& specification)
			: m_Specification(specification), m_Scene(specification.Seed, true), m_Random(specification.Seed)
		{
			Frame.DeltaTime = Frame.FixedDeltaTime;
		}

		ScriptTestFixture::~ScriptTestFixture() = default;

		void ScriptTestFixture::SetSource(std::string path, std::string source)
		{
			m_Sources.insert_or_assign(std::move(path), std::move(source));
		}

		Result<AssetRef<ScriptData>> ScriptTestFixture::CompileScript(AssetHandle handle, std::string_view path)
		{
			ENGINE_TRY_ASSIGN(const VfsPath modulePath, VfsPath::Create("project", path));
			ENGINE_TRY_ASSIGN(const std::string source, ReadModule(modulePath));
			ENGINE_TRY_ASSIGN(const Ref<const ScriptData> extracted,
				LoadTimeVm::Extract({ .Path = modulePath, .Source = source, .Modules = this }, { .ClockSeconds = m_Specification.ClockSeconds }));
			Ref<ScriptData> script = CreateRef<ScriptData>(*extracted);
			for (ScriptRequire& dependency : script->Requires)
			{
				const auto dependencyHandle = m_Assets.Resolve(dependency.Path);
				if (!dependencyHandle.has_value())
					return MakeError(ErrorCode::NotFound, "Publish script dependency '{}' before compiling '{}'", dependency.Path, path);
				dependency.Handle = *dependencyHandle;
			}
			auto scripts = m_Scripts;
			scripts.insert_or_assign(handle, script);
			ENGINE_TRY_ASSIGN(auto schemas, ScriptFieldSchemaSource::Create(scripts));
			m_Scripts = std::move(scripts);
			m_Schemas = std::move(schemas);
			m_Assets.Publish(handle, script, std::string(path));
			return script;
		}

		Result<AssetRef<ScriptData>> ScriptTestFixture::AddScript(AssetHandle handle, std::string path, std::string source)
		{
			SetSource(path, std::move(source));
			return CompileScript(handle, path);
		}

		Status ScriptTestFixture::Start()
		{
			if (m_Engine != nullptr)
				return MakeError(ErrorCode::InvalidState, "The script fixture already has an engine");
			if (!m_Api.IsFrozen())
			{
				ENGINE_TRY(RegisterBindings(m_Api, GetTypes(), m_Specification.ConfigureApi));
			}
			ENGINE_TRY_ASSIGN(auto engine, ScriptEngine::Create({
											   .Host = this,
											   .Api = &m_Api,
											   .Settings = m_Specification.Settings,
											   .Mode = m_Specification.Mode,
											   .TestMode = m_Specification.TestMode,
											   .TestHost = m_Specification.TestHost,
											   .ReadOnly = m_Specification.ReadOnly,
											   .ClockSeconds = m_Specification.ClockSeconds,
										   }));
			ENGINE_TRY(engine->InitializeInstances());
			engine->StartPending();
			m_Engine = std::move(engine);
			return {};
		}

		void ScriptTestFixture::Stop()
		{
			m_Engine.reset();
		}

		Result<ScriptEvaluation> ScriptTestFixture::Evaluate(std::string_view source, std::optional<UUID> entity)
		{
			if (m_Engine == nullptr)
				return MakeError(ErrorCode::InvalidState, "Start the script fixture before evaluating");
			return m_Engine->Evaluate(source, {}, entity);
		}

		Result<std::string> ScriptTestFixture::ReadModule(const VfsPath& path)
		{
			if (path.GetScheme() != "project" || !path.GetPath().starts_with("Assets/"))
				return MakeError(ErrorCode::Validation, "Script fixture modules must be inside project://Assets");
			const auto source = m_Sources.find(std::string(path.GetPath()));
			if (source == m_Sources.end())
				return MakeError(ErrorCode::NotFound, "No script fixture source '{}'", path);
			return source->second;
		}

		const TypeRegistry& ScriptTestFixture::GetTypes() const
		{
			return m_Scene.GetRegistry();
		}

		Result<ScriptActionState> ScriptTestFixture::GetAction(std::string_view name) const
		{
			const auto action = Actions.find(std::string(name));
			if (action == Actions.end())
				return MakeError(ErrorCode::NotFound, "INPUT_UNKNOWN_ACTION: no action '{}'", name);
			return action->second;
		}

		Result<UUID> ScriptTestFixture::CreateEntity(std::string_view name, UUID parent)
		{
			if (GetScene().GetEntityCount() >= EntityLimit)
				return MakeError(ErrorCode::Validation, "The fixture entity limit is reached");
			Entity parentEntity = GetScene().FindEntityByID(parent);
			if (parent.IsValid() && !parentEntity.IsValid())
				return MakeError(ErrorCode::NotFound, "No parent entity {}", parent);
			return (parentEntity ? GetScene().CreateEntity(name, parentEntity) : GetScene().CreateEntity(name)).GetUUID();
		}

		Result<UUID> ScriptTestFixture::Instantiate(AssetHandle prefab, const std::optional<glm::vec3>& position,
			const std::optional<glm::quat>& rotation, UUID parent)
		{
			Entity parentEntity = GetScene().FindEntityByID(parent);
			if (parent.IsValid() && !parentEntity.IsValid())
				return MakeError(ErrorCode::NotFound, "No parent entity {}", parent);
			LoadReport report;
			ENGINE_TRY_ASSIGN(const Prefab asset, LoadPrefabAsset(m_Assets, prefab, GetTypes(), report));
			const size_t existing = GetScene().GetEntityCount();
			if (existing >= EntityLimit || asset.GetEntityIDs().size() > EntityLimit - existing)
				return MakeError(ErrorCode::Validation, "The prefab exceeds the fixture entity limit");
			PrefabInstantiateOptions options{};
			options.PrefabHandle = prefab;
			options.RootID = m_Scene.GetGenerator().Next();
			options.Parent = parentEntity;
			ENGINE_TRY_ASSIGN(const Entity created, PrefabInstantiator::Instantiate(GetScene(), asset, options, { .Schemas = m_Schemas.get() }, report));
			if (position.has_value())
				TransformSystem::SetWorldPosition(created, *position);
			if (rotation.has_value())
				TransformSystem::SetWorldRotation(created, *rotation);
			return created.GetUUID();
		}

		void ScriptTestFixture::MarkTeleported(UUID entity)
		{
			Teleported.push_back(entity);
			const Entity target = GetScene().FindEntityByID(entity);
			if (target && !target.HasComponent<InterpolationResetTag>())
				target.AddComponent<InterpolationResetTag>();
		}

		Status ScriptTestFixture::SetTimeScale(double scale)
		{
			if (!std::isfinite(scale) || scale < 0.0 || scale > 100.0)
				return MakeError(ErrorCode::InvalidArgument, "Time scale must be finite and in [0, 100]");
			Frame.TimeScale = scale;
			return {};
		}

		Status ScriptTestFixture::SetCursorMode(CursorMode mode)
		{
			if (mode != CursorMode::Normal && mode != CursorMode::Hidden && mode != CursorMode::Locked)
				return MakeError(ErrorCode::InvalidArgument, "Invalid cursor mode");
			Cursor = mode;
			return {};
		}

		Status ScriptTestFixture::RequestSceneLoad(AssetHandle scene, Json parameters)
		{
			if (m_Assets.GetAssetType(scene) != AssetType::Scene)
				return MakeError(ErrorCode::Validation, "Scene.Load needs a Scene asset");
			if (!parameters.is_object())
				return MakeError(ErrorCode::Validation, "Scene.Load parameters must be an object");
			RequestedScene = scene;
			RequestedParameters = std::move(parameters);
			return {};
		}

		void ScriptTestFixture::OnScriptError(const ScriptError& error, bool fatal)
		{
			Errors.push_back(error);
			FatalError = FatalError || fatal;
		}

		void ScriptTestFixture::OnExternalMutation(std::string_view reason)
		{
			ExternalMutations.emplace_back(reason);
		}

	}

}
