#include "EnginePCH.h"
#include "Engine/Reflection/FieldInfo.h"

#include "Engine/Core/Assert.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements value access, validation and Variant resolution.
// The constructor and the inline accessors are complete.

namespace Engine {

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

	void* FieldInfo::GetAddress(void* /*object*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const void* FieldInfo::GetAddress(const void* /*object*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	Value FieldInfo::GetValue(const FieldContext& /*context*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status FieldInfo::SetValue(const FieldContext& /*context*/, const Value& /*value*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "FieldInfo::SetValue is an M3 contract stub");
	}

	void FieldInfo::ValidateValue(const Value& /*value*/, const ResolveContext& /*resolve*/, ValidationContext& /*validation*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	void FieldInfo::ValidateJson(const JsonReader& /*reader*/, const ResolveContext& /*resolve*/, ValidationContext& /*validation*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<const FieldInfo*> FieldInfo::ResolveVariant(const ResolveContext& /*context*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "FieldInfo::ResolveVariant is an M3 contract stub");
	}

}
