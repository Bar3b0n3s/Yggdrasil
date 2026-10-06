#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/TypeInfo.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

// The fixture resolver input of the registry suite (Architecture §5.4 "with the resolver fed a fixture schema", Roadmap M3
// "Map and Variant fields through fixture resolvers"): an IFieldSchemaSource that plays the role of M13's script field
// schemas, with schema-only fields declared per owner. The fields declared for DefaultOwner also answer for every owner
// that has no declarations of its own, so randomly generated script handles still resolve.

namespace Engine {

	namespace Test {

		class FixtureSchemaSource final : public IFieldSchemaSource
		{
		public:
			// The script handle under which Declare... adds fields when no owner is given.
			static constexpr UUID DefaultOwner = UUID(0x00000000c0ffee01ull);

			FixtureSchemaSource() = default;

			FixtureSchemaSource(const FixtureSchemaSource&) = delete;
			FixtureSchemaSource& operator=(const FixtureSchemaSource&) = delete;
			// Moving keeps every FieldInfo address (the fields are owned through Scope).
			FixtureSchemaSource(FixtureSchemaSource&&) noexcept = default;
			FixtureSchemaSource& operator=(FixtureSchemaSource&&) noexcept = default;

			// Declares a schema-only field `name` of scalar `kind` (no arrays, maps or structs) with `meta` for `owner`, the
			// way a script declares Field.Number(30, {Min = 0, Max = 200}). Asserts a scalar kind and a unique name.
			void DeclareField(std::string_view name, FieldType kind, const FieldMeta& meta = {}, UUID owner = DefaultOwner);

			// The fixture schema every registry test uses: Torque (Float, 0-200), Count (Int32), Enabled (Bool), Goal
			// (EntityRef), Tint (Color4) and Label (String) on DefaultOwner.
			[[nodiscard]] static FixtureSchemaSource CreateStandard();

			[[nodiscard]] Result<const FieldInfo*> FindField(UUID owner, std::string_view name) const override;
			[[nodiscard]] std::vector<std::string> GetFieldNames(UUID owner) const override;
		private:
			std::vector<Scope<TypeInfo>> m_Types;
			std::map<UUID, std::vector<Scope<FieldInfo>>> m_Fields;
		};

	}

}
