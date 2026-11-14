#include "EnginePCH.h"
#include "Engine/Scripting/ScriptProxy.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/Private/BindingRegistration.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <format>

namespace Engine {

	namespace Utils {

		static bool VisibleComponent(const ComponentInfo& info)
		{
			return info.HasFlag(ComponentFlags::ScriptVisible) && !info.HasFlag(ComponentFlags::Hidden) && !info.HasFlag(ComponentFlags::EntityLevel) && info.GetHostOps();
		}
		static Result<const FieldInfo*> ProxyField(ScriptCall& call, const ComponentInfo& info, std::string_view name)
		{
			const auto* field = info.FindField(name);
			if (!field || !field->GetMeta().Scriptable || field->GetMeta().Hidden)
				return std::unexpected(Error(ErrorCode::NotFound, std::format("unknown script field '{}.{}'", info.GetName(), name)).WithHint(MakeDidYouMeanHint(info.SuggestFieldNames(name))));
			if (!HasFlag(field->GetMeta().Modes, call.Engine->GetRunMode()))
				return MakeError(ErrorCode::InvalidState, "field '{}.{}' is not available in this run mode", info.GetName(), name);
			return field;
		}
		static Status StrictValidation(const ValidationContext& validation)
		{
			ValidationContext strict;
			for (auto issue : validation.GetIssues())
			{
				issue.Severity = DiagnosticSeverity::Error;
				strict.AddIssue(std::move(issue));
			}
			return strict.ToStatus("script component write");
		}
		// The identity names an entity of the current scene generation that is live or pending destruction (M20, W-P02).
		static bool IsReadable(IScriptHost& host, ScriptEntityIdentity entity)
		{
			return entity.ID.IsValid() && entity.SceneGeneration != 0 && entity.SceneGeneration == host.GetSceneGeneration()
				&& host.GetScene().FindReadableEntityByID(entity.ID).IsReadable();
		}
		// ScriptProxy::ValidateAssetTypes' walk: `path` names the value being checked ("Terrain.Layers[1].BaseColorMap",
		// array elements by Lua's 1-based index, map values by key), extended and restored around each element and member.
		static Status CheckAssetTypes(const AssetManager& assets, const TypeInfo& type, const Value& value, std::string& path)
		{
			if (value.IsNull())
				return {};
			switch (type.GetKind())
			{
				case FieldType::AssetRef:
				{
					if (type.GetAssetTypeName().empty() || value.GetKind() != FieldType::AssetRef || !value.AsUUID().IsValid())
						return {};
					const AssetType actual = assets.GetAssetType(value.AsUUID());
					if (actual == AssetType::None || AssetTypeToString(actual) == type.GetAssetTypeName())
						return {};
					return MakeError(ErrorCode::Validation, "field '{}' takes assets of type {}, not {}", path, type.GetAssetTypeName(), AssetTypeToString(actual));
				}
				case FieldType::Array:
				case FieldType::Map:
				{
					const TypeInfo* element = type.GetElement();
					if (element == nullptr || (value.GetKind() != FieldType::Array && value.GetKind() != FieldType::Map))
						return {};
					const size_t length = path.size();
					const auto elements = value.GetElements();
					const auto keys = value.GetKeys();
					for (size_t index = 0; index < elements.size(); ++index)
					{
						path += index < keys.size() ? std::format("[\"{}\"]", keys[index]) : std::format("[{}]", index + 1);
						const Status checked = CheckAssetTypes(assets, *element, elements[index], path);
						path.resize(length);
						ENGINE_TRY(checked);
					}
					return {};
				}
				case FieldType::Struct:
				{
					if (type.GetStruct() == nullptr || value.GetKind() != FieldType::Struct)
						return {};
					const size_t length = path.size();
					for (const auto& member : type.GetStruct()->GetFields())
					{
						const Value* memberValue = value.FindMember(member->GetName());
						if (memberValue == nullptr)
							continue;
						path += ".";
						path += member->GetName();
						const Status checked = CheckAssetTypes(assets, member->GetType(), *memberValue, path);
						path.resize(length);
						ENGINE_TRY(checked);
					}
					return {};
				}
				default:
					return {};
			}
		}

		// ValidateComponent's checks of the proxy's component on `entity`, an entity already validated.
		static Status ValidateProxyComponent(IScriptHost& host, ScriptProxyIdentity proxy, ConstEntity entity)
		{
			const auto components = host.GetTypes().GetComponents();
			if (proxy.ComponentTypeIndex >= components.size())
				return MakeError(ErrorCode::InvalidArgument, "invalid component identity");
			const auto& info = *components[proxy.ComponentTypeIndex];
			if (!VisibleComponent(info))
				return MakeError(ErrorCode::InvalidArgument, "component '{}' is not exposed to scripts", info.GetName());
			if (!info.GetHostOps()->Has(entity))
				return MakeError(ErrorCode::NotFound, "entity {} no longer has '{}'", proxy.Entity.ID, info.GetName());
			return {};
		}

	}

	bool ScriptProxy::IsValid(IScriptHost& host, ScriptEntityIdentity entity)
	{
		return entity.ID.IsValid() && entity.SceneGeneration != 0 && entity.SceneGeneration == host.GetSceneGeneration() && host.GetScene().FindEntityByID(entity.ID).IsValid();
	}
	Status ScriptProxy::ValidateEntity(IScriptHost& host, ScriptEntityIdentity entity)
	{
		if (IsValid(host, entity))
			return {};
		if (!entity.ID.IsValid() || entity.SceneGeneration == 0)
			return MakeError(ErrorCode::NotFound, "entity {} is not a valid entity identity", entity.ID);
		if (entity.SceneGeneration != host.GetSceneGeneration())
			return MakeError(ErrorCode::NotFound, "entity {} belongs to an expired scene", entity.ID);
		return MakeError(ErrorCode::NotFound, "entity {} was destroyed", entity.ID);
	}
	Status ScriptProxy::ValidateEntityForRead(IScriptHost& host, ScriptEntityIdentity entity)
	{
		if (Utils::IsReadable(host, entity))
			return {};
		// Not readable, so not valid either: ValidateEntity's error names why.
		return ValidateEntity(host, entity);
	}
	Status ScriptProxy::ValidateComponent(IScriptHost& host, ScriptProxyIdentity proxy)
	{
		ENGINE_TRY(ValidateEntity(host, proxy.Entity));
		return Utils::ValidateProxyComponent(host, proxy, host.GetScene().FindEntityByID(proxy.Entity.ID));
	}
	Status ScriptProxy::ValidateComponentForRead(IScriptHost& host, ScriptProxyIdentity proxy)
	{
		ENGINE_TRY(ValidateEntityForRead(host, proxy.Entity));
		return Utils::ValidateProxyComponent(host, proxy, host.GetScene().FindReadableEntityByID(proxy.Entity.ID));
	}
	Result<std::optional<ScriptProxyIdentity>> ScriptProxy::GetComponent(IScriptHost& host, ScriptEntityIdentity entity, std::string_view component)
	{
		ENGINE_TRY(ValidateEntityForRead(host, entity));
		const auto* info = host.GetTypes().FindComponent(component);
		// A game's native components live outside the type registry with no script proxy (Docs/Decisions/0030-m19-contract.md
		// decision 26): the hint says so instead of suggesting built-in names (the native test game's friction NT-05).
		if (!info && IsNativeComponentName(component))
			return std::unexpected(Error(ErrorCode::NotFound, std::format("unknown script component '{}': a native component has no script proxy", component)).WithHint(std::string(Detail::NativeComponentScriptHint)));
		if (!info || !Utils::VisibleComponent(*info))
			return std::unexpected(Error(ErrorCode::NotFound, std::format("unknown script component '{}'", component)).WithHint(MakeDidYouMeanHint(host.GetTypes().SuggestComponentNames(component))));
		if (!info->GetHostOps()->Has(host.GetScene().FindReadableEntityByID(entity.ID)))
			return std::optional<ScriptProxyIdentity>{};
		return std::optional<ScriptProxyIdentity>{ { entity, info->GetIndex() } };
	}
	Result<std::optional<ScriptProxyIdentity>> ScriptProxy::GetShortcut(IScriptHost& host, ScriptEntityIdentity entity, std::string_view component)
	{
		ENGINE_TRY_ASSIGN(auto proxy, GetComponent(host, entity, component));
		const auto* info = host.GetTypes().FindComponent(component);
		if (info->HasFlag(ComponentFlags::NoShortcut))
			return MakeError(ErrorCode::NotFound, "'{}' has no entity shortcut", component);
		return proxy;
	}
	Result<Value> ScriptProxy::ReadField(ScriptCall& call, ScriptProxyIdentity proxy, std::string_view field)
	{
		if (!call.Engine)
			return MakeError(ErrorCode::InvalidState, "requires an active script engine");
		auto& host = call.Engine->GetHost();
		ENGINE_TRY(ValidateComponentForRead(host, proxy));
		const auto& info = *host.GetTypes().GetComponents()[proxy.ComponentTypeIndex];
		ENGINE_TRY(Utils::ProxyField(call, info, field));
		ENGINE_TRY_ASSIGN(auto value, ComponentAccess::GetFieldValue(host.GetScene().FindReadableEntityByID(proxy.Entity.ID), info.GetName(), field));
		ENGINE_TRY(call.Engine->GetApi().RecordProxyAccess(call, proxy.ComponentTypeIndex, field, false));
		return value;
	}
	Status ScriptProxy::WriteField(ScriptCall& call, ScriptProxyIdentity proxy, std::string_view field, const Value& value)
	{
		ENGINE_TRY(call.CheckWritable());
		auto& host = call.Engine->GetHost();
		ENGINE_TRY(ValidateComponent(host, proxy));
		const auto& info = *host.GetTypes().GetComponents()[proxy.ComponentTypeIndex];
		ENGINE_TRY_ASSIGN(const auto* descriptor, Utils::ProxyField(call, info, field));
		if (descriptor->IsReadOnly())
			return MakeError(ErrorCode::InvalidState, "field '{}.{}' is read-only", info.GetName(), field);
		const Entity entity = host.GetScene().FindEntityByID(proxy.Entity.ID);
		ResolveContext resolve{ .Registry = &host.GetTypes(), .Owner = info.GetHostOps()->GetConst(entity), .OwnerType = &info, .Key = {}, .Schemas = host.GetFieldSchemas() };
		ValidationContext validation;
		descriptor->ValidateValue(value, resolve, validation);
		ENGINE_TRY(Utils::StrictValidation(validation));
		ENGINE_TRY(ValidateAssetTypes(host, info.GetName(), field, descriptor->GetType(), value));
		if (info.GetName() == "Script" && field == "Scripts")
		{
			// Every slot's script must be a Behaviour (Docs/Decisions/0031-m20-contract.md decision 17).
			for (const Value& slot : value.GetElements())
			{
				if (const Value* script = slot.FindMember("Script"); script != nullptr)
					ENGINE_TRY(Detail::ValidateBehaviourAssignment(host, script->AsUUID()));
			}
		}
		// The reflection setter also checks this, but validation must precede recording invalidation.
		if (info.GetName() == "Transform" && field == "WorldPosition")
			ENGINE_TRY(ValidateWorldPosition(entity.GetParent(), value.AsVec3()));
		if (!descriptor->IsVirtual())
		{
			auto copy = info.CreateDefault();
			info.GetType().GetOps().Copy(copy.get(), resolve.Owner);
			ENGINE_TRY(descriptor->SetValue({ copy.get(), &entity }, value));
			resolve.Owner = copy.get();
			info.Validate(copy.get(), resolve, validation);
			ENGINE_TRY(Utils::StrictValidation(validation));
		}
		ENGINE_TRY(call.PrepareHostMutation());
		ENGINE_TRY(ComponentAccess::SetFieldValue(entity, info.GetName(), field, value, host.GetFieldSchemas()));
		ENGINE_TRY(call.Engine->GetApi().RecordProxyAccess(call, proxy.ComponentTypeIndex, field, true));
		return {};
	}

	Status ScriptProxy::ValidateAssetTypes(IScriptHost& host, std::string_view component, std::string_view field, const TypeInfo& type,
		const Value& value)
	{
		const AssetManager* assets = host.GetAssets();
		if (assets == nullptr)
			return {};
		std::string path = std::format("{}.{}", component, field);
		return Utils::CheckAssetTypes(*assets, type, value, path);
	}

	Status ScriptProxy::ValidateWorldPosition(ConstEntity parent, const glm::vec3& position)
	{
		if (glm::any(glm::isnan(position)) || glm::any(glm::isinf(position)))
			return MakeError(ErrorCode::Validation, "world position contains NaN or infinity");
		if (!parent.IsValid())
			return {};
		const glm::vec3 local(glm::affineInverse(TransformSystem::ComputeWorldMatrix(parent)) * glm::vec4(position, 1.0f));
		if (!glm::any(glm::isnan(local)) && !glm::any(glm::isinf(local)))
			return {};
		return MakeError(ErrorCode::Validation, "world position ({}, {}, {}) cannot be represented relative to parent {}: its local translation is not finite",
			position.x, position.y, position.z, parent.GetUUID());
	}

}
