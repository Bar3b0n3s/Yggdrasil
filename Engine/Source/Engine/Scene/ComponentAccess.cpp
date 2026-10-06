#include "EnginePCH.h"
#include "Engine/Scene/ComponentAccess.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		enum class ComponentAccessKind : uint8_t
		{
			Read,
			Write
		};

		// What PatchFromCopy hands to its mutate callback.
		struct ComponentCopy
		{
			const TypeOps* Ops = nullptr;
			const void* Source = nullptr;
		};

	}

	namespace Utils {

		static ErrorIssue MakeSuggestionIssue(std::string message, std::string hint, std::vector<std::string> suggestions)
		{
			ErrorIssue issue;
			issue.Message = std::move(message);
			issue.Hint = std::move(hint);
			issue.Suggestions = std::move(suggestions);
			return issue;
		}

		static std::string DescribeEntity(ConstEntity entity)
		{
			return std::format("'{}' ({})", entity.GetName(), entity.GetUUID());
		}

		// The registered component `name`, rejected when the access path may not reach it: entity-level components always,
		// Hidden (engine-maintained) components for writes.
		static Result<const ComponentInfo*> FindAccessibleComponent(const TypeRegistry& registry, std::string_view name,
			ComponentAccessKind access)
		{
			const ComponentInfo* info = registry.FindComponent(name);
			if (info == nullptr)
			{
				std::vector<std::string> suggestions = registry.SuggestComponentNames(name);
				std::string hint = MakeDidYouMeanHint(suggestions);
				std::string message = std::format("unknown component '{}'", name);
				return std::unexpected(Error(ErrorCode::NotFound, message)
						.WithHint(hint)
						.WithIssue(MakeSuggestionIssue(std::move(message), std::move(hint), std::move(suggestions))));
			}
			if (info->HasFlag(ComponentFlags::EntityLevel))
			{
				std::string message = std::format("'{}' is an entity-level component, not reachable by component name", name);
				return std::unexpected(Error(ErrorCode::InvalidArgument, std::move(message))
						.WithHint("use the entity's name, tags and hierarchy operations instead"));
			}
			if (access == ComponentAccessKind::Write && info->HasFlag(ComponentFlags::Hidden))
			{
				std::string message = std::format("'{}' is maintained by the engine and cannot be written", name);
				return std::unexpected(Error(ErrorCode::InvalidArgument, std::move(message))
						.WithHint("prefab components change only through the prefab operations"));
			}

			ENGINE_CORE_ASSERT(info->GetHostOps() != nullptr, "Component '{}' was registered without ECS operations (use RegisterComponent)",
				name);
			if (info->GetHostOps() == nullptr)
				return MakeError(ErrorCode::InvalidState, "component '{}' has no ECS operations", name);
			return info;
		}

		static Result<const FieldInfo*> FindComponentField(const ComponentInfo& info, std::string_view name)
		{
			if (const FieldInfo* field = info.FindField(name))
				return field;

			std::vector<std::string> suggestions = info.SuggestFieldNames(name);
			std::string hint = MakeDidYouMeanHint(suggestions);
			std::string message = std::format("unknown field '{}' on '{}'", name, info.GetName());
			return std::unexpected(Error(ErrorCode::NotFound, message)
					.WithHint(hint)
					.WithIssue(MakeSuggestionIssue(std::move(message), std::move(hint), std::move(suggestions))));
		}

		static Error MakeAbsentComponentError(ConstEntity entity, const ComponentInfo& info)
		{
			return Error(ErrorCode::NotFound, std::format("Entity {} has no component '{}'", DescribeEntity(entity), info.GetName()));
		}

		static bool HasComponent(const ComponentInfo& info, ConstEntity entity)
		{
			return info.GetHostOps() != nullptr && info.GetHostOps()->Has(entity);
		}

		static bool Contains(std::span<const ComponentInfo* const> components, const ComponentInfo* component)
		{
			return std::ranges::find(components, component) != components.end();
		}

		// Writes are strict (ADR 0006 decision 6): an unknown member or a Variant value that cannot be checked against its
		// schema is rejected, where a file read would only warn.
		static ReadContext MakeWriteContext(const IFieldSchemaSource* schemas)
		{
			ReadContext context;
			context.Schemas = schemas;
			context.Strict = true;
			return context;
		}

		// The validation result of a write: errors fail it, and so do the read warnings a strict read promotes.
		static Status MakeWriteStatus(const ValidationContext& validation, std::string_view subject)
		{
			ValidationContext strict;
			for (const ValidationIssue& issue : validation.GetIssues())
			{
				ValidationIssue promoted = issue;
				if (promoted.Code == UnknownFieldCode || promoted.Code == VariantUnresolvedCode || promoted.Code == VariantSchemaMismatchCode)
					promoted.Severity = DiagnosticSeverity::Error;
				strict.AddIssue(std::move(promoted));
			}
			return strict.ToStatus(subject);
		}

		// Replaces the entity's component with `source` (a validated object of the component's type) in one
		// ComponentHostOps::Patch, so EnTT's on_update, the change tracker and the revision see exactly one write.
		static void PatchFromCopy(const ComponentInfo& info, Entity entity, const void* source)
		{
			ComponentCopy copy;
			copy.Ops = &info.GetType().GetOps();
			copy.Source = source;
			info.GetHostOps()->Patch(entity, [](void* component, void* context)
			{
				const ComponentCopy& request = *static_cast<const ComponentCopy*>(context);
				request.Ops->Copy(component, request.Source);
			}, &copy);
		}

		// A new object holding a copy of the entity's current component value.
		static ObjectPtr CopyCurrentValue(const ComponentInfo& info, const void* current)
		{
			ObjectPtr object = info.CreateDefault();
			info.GetType().GetOps().Copy(object.get(), current);
			return object;
		}

	}

	Result<Json> ComponentAccess::GetComponentJson(ConstEntity entity, std::string_view component)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "ComponentAccess needs a valid entity");
		const TypeRegistry& registry = entity.GetScene()->GetTypeRegistry();
		ENGINE_TRY_ASSIGN(const ComponentInfo* info, Utils::FindAccessibleComponent(registry, component, ComponentAccessKind::Read));

		const void* object = info->GetHostOps()->GetConst(entity);
		if (object == nullptr)
			return std::unexpected(Utils::MakeAbsentComponentError(entity, *info));
		return info->ToJson(object);
	}

	Status ComponentAccess::AddComponent(Entity entity, std::string_view component, const Json* initial,
		const IFieldSchemaSource* schemas)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "ComponentAccess needs a valid entity");
		const Scene& scene = *entity.GetScene();
		const TypeRegistry& registry = scene.GetTypeRegistry();
		ENGINE_TRY_ASSIGN(const ComponentInfo* info, Utils::FindAccessibleComponent(registry, component, ComponentAccessKind::Write));
		const ComponentHostOps& ops = *info->GetHostOps();

		if (ops.Has(entity))
			return MakeError(ErrorCode::InvalidState, "Entity {} already has component '{}'", Utils::DescribeEntity(entity), info->GetName());

		for (const ComponentInfo* required : info->GetRequires())
		{
			if (!Utils::HasComponent(*required, entity))
			{
				std::string message = std::format("'{}' requires '{}', which entity {} does not have", info->GetName(), required->GetName(),
					Utils::DescribeEntity(entity));
				std::string hint = std::format("add '{}' first", required->GetName());
				return std::unexpected(Error(ErrorCode::InvalidState, std::move(message)).WithHint(std::move(hint)));
			}
		}

		// Exclusions hold in both directions, whichever side declared them.
		for (const ComponentInfo* present : registry.GetComponents())
		{
			if (!Utils::HasComponent(*present, entity))
				continue;
			if (Utils::Contains(info->GetExcludes(), present) || Utils::Contains(present->GetExcludes(), info))
			{
				std::string message = std::format("'{}' cannot be combined with '{}', which entity {} has", info->GetName(), present->GetName(),
					Utils::DescribeEntity(entity));
				std::string hint = std::format("remove '{}' first", present->GetName());
				return std::unexpected(Error(ErrorCode::InvalidState, std::move(message)).WithHint(std::move(hint)));
			}
		}

		if (info->HasFlag(ComponentFlags::UniquePerScene))
		{
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const ConstEntity other = scene.FindEntityByID(id);
				if (other.IsValid() && ops.Has(other))
				{
					return MakeError(ErrorCode::InvalidState, "only one '{}' may exist per scene, and entity {} already has it", info->GetName(),
						Utils::DescribeEntity(other));
				}
			}
		}

		ObjectPtr value = info->CreateDefault();
		if (initial != nullptr)
		{
			const JsonReader reader(*initial);
			if (!reader.IsObject())
				return MakeError(ErrorCode::Validation, "the initial value of '{}' must be an object of its fields", info->GetName());
			ENGINE_TRY(info->FromJson(value.get(), reader, Utils::MakeWriteContext(schemas)));
		}

		static_cast<void>(ops.Add(entity));
		if (initial != nullptr)
			Utils::PatchFromCopy(*info, entity, value.get());
		return {};
	}

	Status ComponentAccess::RemoveComponent(Entity entity, std::string_view component)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "ComponentAccess needs a valid entity");
		const TypeRegistry& registry = entity.GetScene()->GetTypeRegistry();
		ENGINE_TRY_ASSIGN(const ComponentInfo* info, Utils::FindAccessibleComponent(registry, component, ComponentAccessKind::Write));
		const ComponentHostOps& ops = *info->GetHostOps();

		if (!ops.Has(entity))
			return std::unexpected(Utils::MakeAbsentComponentError(entity, *info));
		if (!info->HasFlag(ComponentFlags::Removable))
			return MakeError(ErrorCode::InvalidState, "'{}' cannot be removed from an entity", info->GetName());

		for (const ComponentInfo* present : registry.GetComponents())
		{
			if (present != info && Utils::HasComponent(*present, entity) && Utils::Contains(present->GetRequires(), info))
			{
				std::string message = std::format("'{}' is required by '{}', which entity {} has", info->GetName(), present->GetName(),
					Utils::DescribeEntity(entity));
				std::string hint = std::format("remove '{}' first", present->GetName());
				return std::unexpected(Error(ErrorCode::InvalidState, std::move(message)).WithHint(std::move(hint)));
			}
		}

		ops.Remove(entity);
		return {};
	}

	Status ComponentAccess::SetComponentJson(Entity entity, std::string_view component, const Json& value,
		const IFieldSchemaSource* schemas)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "ComponentAccess needs a valid entity");
		const TypeRegistry& registry = entity.GetScene()->GetTypeRegistry();
		ENGINE_TRY_ASSIGN(const ComponentInfo* info, Utils::FindAccessibleComponent(registry, component, ComponentAccessKind::Write));
		if (!info->GetHostOps()->Has(entity))
			return std::unexpected(Utils::MakeAbsentComponentError(entity, *info));

		const JsonReader reader(value);
		if (!reader.IsObject())
			return MakeError(ErrorCode::Validation, "the value of '{}' must be an object of its fields", info->GetName());

		ObjectPtr object = info->CreateDefault();
		ENGINE_TRY(info->FromJson(object.get(), reader, Utils::MakeWriteContext(schemas)));
		Utils::PatchFromCopy(*info, entity, object.get());
		return {};
	}

	Status ComponentAccess::PatchComponentJson(Entity entity, std::string_view component, const Json& patch,
		const IFieldSchemaSource* schemas)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "ComponentAccess needs a valid entity");
		const TypeRegistry& registry = entity.GetScene()->GetTypeRegistry();
		ENGINE_TRY_ASSIGN(const ComponentInfo* info, Utils::FindAccessibleComponent(registry, component, ComponentAccessKind::Write));
		const void* current = info->GetHostOps()->GetConst(entity);
		if (current == nullptr)
			return std::unexpected(Utils::MakeAbsentComponentError(entity, *info));

		ObjectPtr object = Utils::CopyCurrentValue(*info, current);
		ENGINE_TRY(info->ApplyMergePatch(object.get(), JsonReader(patch), Utils::MakeWriteContext(schemas)));
		Utils::PatchFromCopy(*info, entity, object.get());
		return {};
	}

	Result<Value> ComponentAccess::GetFieldValue(Entity entity, std::string_view component, std::string_view field)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "ComponentAccess needs a valid entity");
		const TypeRegistry& registry = entity.GetScene()->GetTypeRegistry();
		ENGINE_TRY_ASSIGN(const ComponentInfo* info, Utils::FindAccessibleComponent(registry, component, ComponentAccessKind::Read));
		void* object = info->GetHostOps()->Get(entity);
		if (object == nullptr)
			return std::unexpected(Utils::MakeAbsentComponentError(entity, *info));
		ENGINE_TRY_ASSIGN(const FieldInfo* fieldInfo, Utils::FindComponentField(*info, field));

		FieldContext context;
		context.Object = object;
		context.Owner = &entity;
		return fieldInfo->GetValue(context);
	}

	Status ComponentAccess::SetFieldValue(Entity entity, std::string_view component, std::string_view field, const Value& value,
		const IFieldSchemaSource* schemas)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "ComponentAccess needs a valid entity");
		const TypeRegistry& registry = entity.GetScene()->GetTypeRegistry();
		ENGINE_TRY_ASSIGN(const ComponentInfo* info, Utils::FindAccessibleComponent(registry, component, ComponentAccessKind::Write));
		void* current = info->GetHostOps()->Get(entity);
		if (current == nullptr)
			return std::unexpected(Utils::MakeAbsentComponentError(entity, *info));
		ENGINE_TRY_ASSIGN(const FieldInfo* fieldInfo, Utils::FindComponentField(*info, field));
		if (fieldInfo->IsReadOnly())
			return MakeError(ErrorCode::InvalidState, "field '{}.{}' is read-only", info->GetName(), fieldInfo->GetName());

		if (fieldInfo->IsVirtual())
		{
			// A virtual setter validates the value and applies it through the scene itself (Transform.WorldPosition patches
			// the local transform), so it is one atomic write.
			FieldContext context;
			context.Object = current;
			context.Owner = &entity;
			return fieldInfo->SetValue(context, value);
		}

		// A stored field is written into a copy, which must then pass the component's type-level rules (a spot light's
		// inner cone below its outer one) before it replaces the component in one patch.
		ObjectPtr object = Utils::CopyCurrentValue(*info, current);
		FieldContext context;
		context.Object = object.get();
		context.Owner = &entity;
		ENGINE_TRY(fieldInfo->SetValue(context, value));

		ResolveContext resolve;
		resolve.Registry = &registry;
		resolve.Schemas = schemas;
		ValidationContext validation;
		info->Validate(object.get(), resolve, validation);
		ENGINE_TRY(Utils::MakeWriteStatus(validation, std::format("component '{}'", info->GetName())));

		Utils::PatchFromCopy(*info, entity, object.get());
		return {};
	}

}
