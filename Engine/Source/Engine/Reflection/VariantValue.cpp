#include "EnginePCH.h"
#include "Engine/Reflection/VariantValue.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/Private/ReflectionWalk.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace Engine {

	namespace Utils {

		static Ref<const Json> MakePayload(Json value)
		{
			ENGINE_CORE_ASSERT(ComputeJsonDepth(value) <= MaxJsonDepth, "A Variant value nests deeper than {} levels", MaxJsonDepth);
			if (value.is_null())
				return nullptr;
			return CreateRef<const Json>(std::move(value));
		}

	}

	VariantValue::VariantValue(Json value)
		: m_Value(Utils::MakePayload(std::move(value)))
	{
	}

	const Json& VariantValue::Get() const
	{
		static const Json NullJson;
		return m_Value != nullptr ? *m_Value : NullJson;
	}

	void VariantValue::Set(Json value)
	{
		m_Value = Utils::MakePayload(std::move(value));
	}

	bool VariantValue::IsNull() const
	{
		return m_Value == nullptr;
	}

	bool VariantValue::operator==(const VariantValue& other) const
	{
		return Get() == other.Get();
	}

}
