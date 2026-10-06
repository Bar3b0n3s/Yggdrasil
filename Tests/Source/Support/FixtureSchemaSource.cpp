#include "TestsPCH.h"
#include "Support/FixtureSchemaSource.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/FuzzySuggest.h"

#include <algorithm>
#include <format>

namespace Engine {

	namespace Test {

		static const FieldInfo* FindDeclaredField(const std::vector<Scope<FieldInfo>>& fields, std::string_view name)
		{
			for (const Scope<FieldInfo>& field : fields)
			{
				if (field->GetName() == name)
					return field.get();
			}
			return nullptr;
		}

		void FixtureSchemaSource::DeclareField(std::string_view name, FieldType kind, const FieldMeta& meta, UUID owner)
		{
			ENGINE_CORE_ASSERT(IsScalarFieldType(kind) && kind != FieldType::Enum,
				"Fixture field '{}' needs a scalar kind without an enum table, got {}", name, FieldTypeToString(kind));
			std::vector<Scope<FieldInfo>>& fields = m_Fields[owner];
			ENGINE_CORE_ASSERT(FindDeclaredField(fields, name) == nullptr, "Fixture field '{}' is declared twice for one owner", name);

			// One schema-only TypeInfo per kind, shared by every field of that kind.
			const auto existing = std::find_if(m_Types.begin(), m_Types.end(), [kind](const Scope<TypeInfo>& type)
			{
				return type->GetKind() == kind;
			});
			const TypeInfo* type = nullptr;
			if (existing != m_Types.end())
			{
				type = existing->get();
			}
			else
			{
				TypeInfo::Specification specification;
				specification.Kind = kind;
				specification.Name = std::string(FieldTypeToString(kind));
				m_Types.push_back(CreateScope<TypeInfo>(std::move(specification)));
				type = m_Types.back().get();
			}

			FieldInfo::Specification specification;
			specification.Name = std::string(name);
			specification.Description = std::format("Fixture script field '{}' ({}).", name, FieldTypeToString(kind));
			specification.Type = type;
			specification.Meta = meta;
			fields.push_back(CreateScope<FieldInfo>(std::move(specification)));
		}

		FixtureSchemaSource FixtureSchemaSource::CreateStandard()
		{
			FixtureSchemaSource schemas;
			schemas.DeclareField("Torque", FieldType::Float, FieldMeta{ .Min = 0.0, .Max = 200.0, .Unit = "N*m" });
			schemas.DeclareField("Count", FieldType::Int32);
			schemas.DeclareField("Enabled", FieldType::Bool);
			schemas.DeclareField("Goal", FieldType::EntityRef);
			schemas.DeclareField("Tint", FieldType::Color4);
			schemas.DeclareField("Label", FieldType::String);
			return schemas;
		}

		Result<const FieldInfo*> FixtureSchemaSource::FindField(UUID owner, std::string_view name) const
		{
			auto declared = m_Fields.find(owner);
			if (declared == m_Fields.end())
				declared = m_Fields.find(DefaultOwner);
			if (declared == m_Fields.end())
				return MakeError(ErrorCode::NotFound, "script '{}' declares no fields", owner);

			std::vector<std::string_view> names;
			for (const Scope<FieldInfo>& field : declared->second)
			{
				if (field->GetName() == name)
					return field.get();
				names.push_back(field->GetName());
			}
			const std::vector<std::string> suggestions = FuzzySuggest(name, names);
			return std::unexpected(Error(ErrorCode::NotFound, std::format("script '{}' declares no field '{}'", owner, name))
					.WithHint(MakeDidYouMeanHint(suggestions)));
		}

		std::vector<std::string> FixtureSchemaSource::GetFieldNames(UUID owner) const
		{
			auto declared = m_Fields.find(owner);
			if (declared == m_Fields.end())
				declared = m_Fields.find(DefaultOwner);
			std::vector<std::string> names;
			if (declared == m_Fields.end())
				return names;
			for (const Scope<FieldInfo>& field : declared->second)
				names.push_back(field->GetName());
			return names;
		}

	}

}
