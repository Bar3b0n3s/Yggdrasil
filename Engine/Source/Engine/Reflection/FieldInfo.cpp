#include "EnginePCH.h"
#include "Engine/Reflection/FieldInfo.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/Private/ReflectionWalk.h"
#include "Engine/Reflection/StructInfo.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		// `resolve` with the registry filled in, when the caller left it out, from the owner's type or else from the struct
		// that declares the field.
		static ResolveContext CompleteResolveContext(const ResolveContext& resolve, const StructInfo* declaringStruct)
		{
			ResolveContext complete = resolve;
			if (complete.Registry == nullptr && complete.OwnerType != nullptr)
				complete.Registry = &complete.OwnerType->GetRegistry();
			if (complete.Registry == nullptr && declaringStruct != nullptr)
				complete.Registry = &declaringStruct->GetRegistry();
			return complete;
		}

	}

	FieldInfo::FieldInfo(Specification specification)
		: m_Specification(std::move(specification))
	{
		const Specification& stored = m_Specification;
		ENGINE_CORE_ASSERT(!stored.Name.empty(), "A reflected field needs a name");
		ENGINE_CORE_ASSERT(!stored.Description.empty(), "Field '{}' needs a description", stored.Name);
		ENGINE_CORE_ASSERT(stored.Type != nullptr, "Field '{}' needs a type", stored.Name);
		ENGINE_CORE_ASSERT(stored.Accessor == nullptr || stored.Getter == nullptr, "Field '{}' is either stored or virtual", stored.Name);
		ENGINE_CORE_ASSERT(stored.Setter == nullptr || stored.Getter != nullptr, "Virtual field '{}' has a setter but no getter", stored.Name);
	}

	void* FieldInfo::GetAddress(void* object) const
	{
		ENGINE_CORE_ASSERT(IsStored(), "Field '{}' is not stored", m_Specification.Name);
		ENGINE_CORE_ASSERT(object != nullptr, "Field '{}' needs an object", m_Specification.Name);
		return m_Specification.Accessor->GetAddress(object);
	}

	const void* FieldInfo::GetAddress(const void* object) const
	{
		ENGINE_CORE_ASSERT(IsStored(), "Field '{}' is not stored", m_Specification.Name);
		ENGINE_CORE_ASSERT(object != nullptr, "Field '{}' needs an object", m_Specification.Name);
		return m_Specification.Accessor->GetAddress(object);
	}

	Value FieldInfo::GetValue(const FieldContext& context) const
	{
		ENGINE_CORE_ASSERT(context.Object != nullptr, "Reading field '{}' needs an object", m_Specification.Name);
		ENGINE_CORE_ASSERT(IsStored() || IsVirtual(), "Field '{}' is schema-only and has no value", m_Specification.Name);
		if (IsVirtual())
			return m_Specification.Getter(context);
		return Utils::ObjectToValue(GetType(), GetAddress(static_cast<const void*>(context.Object)));
	}

	Status FieldInfo::SetValue(const FieldContext& context, const Value& value) const
	{
		ENGINE_CORE_ASSERT(context.Object != nullptr, "Writing field '{}' needs an object", m_Specification.Name);
		ENGINE_CORE_ASSERT(IsStored() || IsVirtual(), "Field '{}' is schema-only and cannot be written", m_Specification.Name);
		if (IsReadOnly())
			return MakeError(ErrorCode::InvalidState, "field '{}' is read-only", m_Specification.Name);

		ValidationContext validation;
		ValidateValue(value, MakeOwnerResolveContext(context.Object), validation);
		ENGINE_TRY(validation.ToStatus(m_Specification.Name));

		if (IsVirtual())
			return m_Specification.Setter(context, value);
		Utils::ValueToObject(GetType(), value, GetAddress(context.Object));
		return {};
	}

	void FieldInfo::ValidateValue(const Value& value, const ResolveContext& resolve, ValidationContext& validation) const
	{
		Utils::WalkContext walk;
		walk.Validation = &validation;
		walk.Schemas = resolve.Schemas;
		walk.Policy = Utils::VariantPolicy::Write;
		validation.PushKey(m_Specification.Name);
		Utils::ValidateValue(GetType(), *this, true, value, Utils::CompleteResolveContext(resolve, m_Owner), walk);
		validation.PopKey();
	}

	void FieldInfo::ValidateJson(const JsonReader& reader, const ResolveContext& resolve, ValidationContext& validation) const
	{
		Utils::WalkContext walk;
		walk.Validation = &validation;
		walk.Schemas = resolve.Schemas;
		walk.Policy = Utils::VariantPolicy::Write;
		validation.PushKey(m_Specification.Name);
		Utils::ReadJson(GetType(), *this, true, reader, nullptr, Utils::CompleteResolveContext(resolve, m_Owner), walk);
		validation.PopKey();
	}

	Result<const FieldInfo*> FieldInfo::ResolveVariant(const ResolveContext& context) const
	{
		ENGINE_CORE_ASSERT(Utils::ContainsVariant(GetType()), "Field '{}' holds no Variant values", m_Specification.Name);
		if (m_Specification.Resolver == nullptr)
			return MakeError(ErrorCode::NotFound, "field '{}' has no resolver: its values are free-form JSON", m_Specification.Name);
		return m_Specification.Resolver(Utils::CompleteResolveContext(context, m_Owner));
	}

	ResolveContext FieldInfo::MakeOwnerResolveContext(const void* owner) const
	{
		ResolveContext resolve;
		resolve.Owner = owner;
		resolve.OwnerType = m_Owner;
		resolve.Registry = m_Owner != nullptr ? &m_Owner->GetRegistry() : nullptr;
		return resolve;
	}

}
