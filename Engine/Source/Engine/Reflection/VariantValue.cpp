#include "EnginePCH.h"
#include "Engine/Reflection/VariantValue.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements the shared JSON payload.

namespace Engine {

	VariantValue::VariantValue(Json /*value*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	const Json& VariantValue::Get() const
	{
		ENGINE_CONTRACT_STUB();
		static const Json NullJson;
		return NullJson;
	}

	void VariantValue::Set(Json /*value*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool VariantValue::IsNull() const
	{
		ENGINE_CONTRACT_STUB();
		return true;
	}

	bool VariantValue::operator==(const VariantValue& /*other*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
