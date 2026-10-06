#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"

namespace Engine {

	// The C++ storage of a FieldType::Variant value (Architecture §5.4): one JSON value kept verbatim, whose schema is
	// resolved at run time by the field's VariantSchemaResolver (ScriptComponent.Fields values, PrefabOverride.Value) or
	// that is free-form JSON when the field has no resolver (TestSuiteSettings.Parameters). A value that cannot be resolved
	// or does not match its resolved schema is still kept exactly as read, so data is never dropped.
	//
	// The payload is immutable and shared between copies (Ref<const Json>, §4.7), so copying a component or an override
	// list is cheap; Set replaces the payload of this instance only. The empty state is JSON null. A value type; distinct
	// instances may be used from different threads, one instance is not thread-safe.
	class VariantValue
	{
	public:
		// JSON null.
		VariantValue() = default;
		// Takes `value` (nesting at most MaxJsonDepth, asserted).
		explicit VariantValue(Json value);

		// The JSON value; a shared null value when empty. The reference stays valid until this instance is assigned or
		// destroyed.
		[[nodiscard]] const Json& Get() const;

		// Replaces the value (nesting at most MaxJsonDepth, asserted).
		void Set(Json value);

		// True when the value is JSON null.
		[[nodiscard]] bool IsNull() const;

		// Deep JSON equality (object member order is significant, as in Json's own operator==).
		[[nodiscard]] bool operator==(const VariantValue& other) const;
	private:
		Ref<const Json> m_Value; // null pointer = JSON null
	};

}
