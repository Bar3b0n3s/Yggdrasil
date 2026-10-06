#include "EnginePCH.h"
#include "Engine/Reflection/StructInfo.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/Private/ReflectionWalk.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <format>

namespace Engine {

	namespace Utils {

		static bool IsStrictPromotedCode(std::string_view code)
		{
			return code == UnknownFieldCode || code == VariantUnresolvedCode || code == VariantSchemaMismatchCode;
		}

		// Finishes a read: warnings promoted by --strict become errors, the remaining warnings go to the diagnostics, and the
		// errors (if any) become one Validation error, located at the single problem or, for several, at the object.
		static Status FinishRead(const StructInfo& type, const JsonReader& reader, ValidationContext& validation, const ReadContext& context)
		{
			std::vector<ErrorIssue> errors;
			for (ValidationIssue& issue : validation.TakeIssues())
			{
				if (issue.Severity == DiagnosticSeverity::Warning && context.Strict && IsStrictPromotedCode(issue.Code))
					issue.Severity = DiagnosticSeverity::Error;

				if (issue.Severity == DiagnosticSeverity::Error)
					errors.push_back(ErrorIssue{ std::move(issue.JsonPointer), std::move(issue.Message), std::move(issue.Hint), std::move(issue.Suggestions) });
				else if (context.Diagnostics != nullptr)
					context.Diagnostics->push_back(std::move(issue));
			}
			if (errors.empty())
				return {};

			const bool single = errors.size() == 1;
			std::string message = single ? errors.front().Message : std::format("{} invalid fields in {}", errors.size(), type.GetName());
			std::string hint = single ? errors.front().Hint : std::string();
			ErrorLocation location;
			location.JsonPointer = single ? errors.front().JsonPointer : reader.GetPointer();
			return std::unexpected(Error(ErrorCode::Validation, std::move(message))
					.WithHint(std::move(hint))
					.WithLocation(std::move(location))
					.WithIssues(std::move(errors)));
		}

	}

	StructInfo::StructInfo(std::string name, std::string description, const TypeRegistry& registry)
		: m_Name(std::move(name)), m_Description(std::move(description)), m_Registry(&registry)
	{
	}

	StructInfo::~StructInfo() = default;

	const TypeInfo& StructInfo::GetType() const
	{
		ENGINE_CORE_ASSERT(m_Type != nullptr, "Struct '{}' has no TypeInfo yet", m_Name);
		return *m_Type;
	}

	const FieldInfo* StructInfo::FindField(std::string_view name) const
	{
		for (const Scope<FieldInfo>& field : m_Fields)
		{
			if (field->GetName() == name)
				return field.get();
		}
		return nullptr;
	}

	std::vector<std::string> StructInfo::SuggestFieldNames(std::string_view name, size_t maxResults) const
	{
		std::vector<std::string_view> names;
		names.reserve(m_Fields.size());
		for (const Scope<FieldInfo>& field : m_Fields)
			names.push_back(field->GetName());
		return FuzzySuggest(name, names, maxResults);
	}

	const FieldInfo& StructInfo::GetSelfField() const
	{
		ENGINE_CORE_ASSERT(m_SelfField != nullptr, "Struct '{}' has no TypeInfo yet", m_Name);
		return *m_SelfField;
	}

	ObjectPtr StructInfo::CreateDefault() const
	{
		const TypeOps& ops = GetType().GetOps();
		ENGINE_CORE_ASSERT(ops.Create != nullptr, "Struct '{}' has no C++ storage", m_Name);
		if (ops.Create == nullptr)
			return ObjectPtr(nullptr, [](void* /*object*/) {});
		return ops.Create();
	}

	Result<Json> StructInfo::ToJson(const void* object) const
	{
		ENGINE_CORE_ASSERT(object != nullptr, "ToJson of struct '{}' needs an object", m_Name);
		return Utils::ObjectToJson(GetType(), object);
	}

	Status StructInfo::FromJson(void* object, const JsonReader& reader, const ReadContext& context) const
	{
		ENGINE_CORE_ASSERT(object != nullptr, "FromJson of struct '{}' needs an object", m_Name);
		const TypeOps& ops = GetType().GetOps();

		// Read into a copy so that a failed read leaves `object` unchanged.
		const ObjectPtr copy = ops.Create();
		ops.Copy(copy.get(), object);

		ValidationContext validation(reader.GetPointer());
		Utils::WalkContext walk;
		walk.Validation = &validation;
		walk.Schemas = context.Schemas;
		walk.Policy = Utils::VariantPolicy::Read;
		Utils::ReadStructJson(*this, reader, copy.get(), walk);

		ENGINE_TRY(Utils::FinishRead(*this, reader, validation, context));
		ops.Copy(object, copy.get());
		return {};
	}

	Status StructInfo::ApplyMergePatch(void* object, const JsonReader& patch, const ReadContext& context) const
	{
		ENGINE_CORE_ASSERT(object != nullptr, "ApplyMergePatch of struct '{}' needs an object", m_Name);
		if (!patch.IsObject())
		{
			return std::unexpected(Utils::MakeLocatedValidationError(patch.GetPointer(),
				std::format("a merge patch of '{}' must be an object, got {}", m_Name, JsonTypeToString(patch.GetType()))));
		}

		ENGINE_TRY_ASSIGN(const Json current, ToJson(object));
		const Json merged = Utils::MergePatchForType(GetType(), current, patch.GetValue());

		const ObjectPtr fresh = CreateDefault();
		ENGINE_TRY(FromJson(fresh.get(), JsonReader(merged, patch.GetPointer()), context));
		GetType().GetOps().Copy(object, fresh.get());
		return {};
	}

	void StructInfo::Validate(const void* object, const ResolveContext& resolve, ValidationContext& context) const
	{
		ENGINE_CORE_ASSERT(object != nullptr, "Validate of struct '{}' needs an object", m_Name);
		Utils::WalkContext walk;
		walk.Validation = &context;
		walk.Schemas = resolve.Schemas;
		walk.Policy = Utils::VariantPolicy::Write;
		Utils::ValidateStructObject(*this, object, walk);
	}

	Json StructInfo::MakeDefaultJson() const
	{
		const ObjectPtr object = CreateDefault();
		Result<Json> json = ToJson(object.get());
		ENGINE_CORE_ASSERT(json.has_value(), "The default value of struct '{}' cannot be serialized: {}", m_Name,
			json.has_value() ? std::string() : json.error().ToString());
		return json.has_value() ? std::move(*json) : Json::object();
	}

	void StructInfo::AddField(FieldInfo::Specification specification)
	{
		ENGINE_CORE_ASSERT(!m_Registry->IsFrozen(), "Field '{}' is added to struct '{}' after TypeRegistry::Freeze", specification.Name, m_Name);
		ENGINE_CORE_ASSERT(!specification.Name.empty(), "A field of struct '{}' needs a name", m_Name);
		ENGINE_CORE_ASSERT(FindField(specification.Name) == nullptr, "Struct '{}' already has a field named '{}'", m_Name, specification.Name);
		ENGINE_CORE_ASSERT(!specification.Description.empty(), "Field '{}.{}' needs a description", m_Name, specification.Name);
		ENGINE_CORE_ASSERT(specification.Type != nullptr, "Field '{}.{}' needs a type", m_Name, specification.Name);
		ENGINE_CORE_ASSERT(specification.Resolver == nullptr || (specification.Type != nullptr && Utils::ContainsVariant(*specification.Type)),
			"Field '{}.{}' has a Variant resolver but holds no Variant values", m_Name, specification.Name);

		if (specification.Type != nullptr && specification.Type->GetKind() == FieldType::AssetRef)
			specification.Meta.AssetFilter = specification.Type->GetAssetTypeName();

		Scope<FieldInfo> field = CreateScope<FieldInfo>(std::move(specification));
		Detail::ReflectionAccess::SetOwner(*field, this);
		m_Fields.push_back(std::move(field));
	}

	void StructInfo::Generate(void* object, Random& random) const
	{
		ENGINE_CORE_ASSERT(object != nullptr, "Generate of struct '{}' needs an object", m_Name);
		for (const Scope<StructGenerator>& generator : m_Generators)
			(*generator)(object, random);
	}

	void StructInfo::AddValidator(StructValidator validator)
	{
		ENGINE_CORE_ASSERT(!m_Registry->IsFrozen(), "A validator is added to struct '{}' after TypeRegistry::Freeze", m_Name);
		ENGINE_CORE_ASSERT(static_cast<bool>(validator), "Struct '{}' gets an empty validator", m_Name);
		m_Validators.push_back(CreateScope<StructValidator>(std::move(validator)));
	}

	void StructInfo::AddGenerator(StructGenerator generator)
	{
		ENGINE_CORE_ASSERT(!m_Registry->IsFrozen(), "A Generate hook is added to struct '{}' after TypeRegistry::Freeze", m_Name);
		ENGINE_CORE_ASSERT(static_cast<bool>(generator), "Struct '{}' gets an empty Generate hook", m_Name);
		m_Generators.push_back(CreateScope<StructGenerator>(std::move(generator)));
	}

	void StructInfo::SetType(const TypeInfo& type)
	{
		ENGINE_CORE_ASSERT(m_Type == nullptr, "Struct '{}' already has a TypeInfo", m_Name);
		m_Type = &type;

		FieldInfo::Specification self;
		self.Name = m_Name;
		self.Description = m_Description;
		self.Type = &type;
		m_SelfField = CreateScope<FieldInfo>(std::move(self));
	}

}
