#include "EnginePCH.h"
#include "Engine/Scripting/ScriptProxy.h"

#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
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

	}

	bool ScriptProxy::IsValid(IScriptHost& host, ScriptEntityIdentity entity)
	{
		return entity.ID.IsValid() && entity.SceneGeneration != 0 && entity.SceneGeneration == host.GetSceneGeneration() && host.GetScene().FindEntityByID(entity.ID).IsValid();
	}
	Status ScriptProxy::ValidateEntity(IScriptHost& host, ScriptEntityIdentity entity)
	{
		if (!IsValid(host, entity))
			return MakeError(ErrorCode::NotFound, "entity {} is destroyed or belongs to an expired scene", entity.ID);
		return {};
	}
	Status ScriptProxy::ValidateComponent(IScriptHost& host, ScriptProxyIdentity proxy)
	{
		ENGINE_TRY(ValidateEntity(host, proxy.Entity));
		const auto components = host.GetTypes().GetComponents();
		if (proxy.ComponentTypeIndex >= components.size())
			return MakeError(ErrorCode::InvalidArgument, "invalid component identity");
		const auto& info = *components[proxy.ComponentTypeIndex];
		if (!Utils::VisibleComponent(info))
			return MakeError(ErrorCode::InvalidArgument, "component '{}' is not exposed to scripts", info.GetName());
		if (!info.GetHostOps()->Has(host.GetScene().FindEntityByID(proxy.Entity.ID)))
			return MakeError(ErrorCode::NotFound, "entity {} no longer has '{}'", proxy.Entity.ID, info.GetName());
		return {};
	}
	Result<std::optional<ScriptProxyIdentity>> ScriptProxy::GetComponent(IScriptHost& host, ScriptEntityIdentity entity, std::string_view component)
	{
		ENGINE_TRY(ValidateEntity(host, entity));
		const auto* info = host.GetTypes().FindComponent(component);
		if (!info || !Utils::VisibleComponent(*info))
			return std::unexpected(Error(ErrorCode::NotFound, std::format("unknown script component '{}'", component)).WithHint(MakeDidYouMeanHint(host.GetTypes().SuggestComponentNames(component))));
		if (!info->GetHostOps()->Has(host.GetScene().FindEntityByID(entity.ID)))
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
		ENGINE_TRY(ValidateComponent(host, proxy));
		const auto& info = *host.GetTypes().GetComponents()[proxy.ComponentTypeIndex];
		ENGINE_TRY(Utils::ProxyField(call, info, field));
		ENGINE_TRY_ASSIGN(auto value, ComponentAccess::GetFieldValue(host.GetScene().FindEntityByID(proxy.Entity.ID), info.GetName(), field));
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
		if (info.GetName() == "Script" && field == "Script")
			ENGINE_TRY(Detail::ValidateBehaviourAssignment(host, value.AsUUID()));
		if (info.GetName() == "Transform" && field == "WorldPosition")
		{
			// The reflection setter also checks this, but validation must precede recording invalidation.
			if (const Entity parent = entity.GetParent(); parent)
			{
				const glm::vec3 local(glm::affineInverse(TransformSystem::ComputeWorldMatrix(parent)) * glm::vec4(value.AsVec3(), 1));
				if (glm::any(glm::isnan(local)) || glm::any(glm::isinf(local)))
					return MakeError(ErrorCode::Validation, "world position cannot be represented relative to this parent");
			}
		}
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

}
