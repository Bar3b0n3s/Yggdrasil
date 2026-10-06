#include "EnginePCH.h"
#include "Engine/Reflection/StructInfo.h"

#include "Engine/Core/Assert.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements the generic JSON, patch and validation operations
// and field registration. The constructor, the type and self-field setup and the accessors are complete.

namespace Engine {

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

	const FieldInfo* StructInfo::FindField(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::vector<std::string> StructInfo::SuggestFieldNames(std::string_view /*name*/, size_t /*maxResults*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	const FieldInfo& StructInfo::GetSelfField() const
	{
		ENGINE_CORE_ASSERT(m_SelfField != nullptr, "Struct '{}' has no TypeInfo yet", m_Name);
		return *m_SelfField;
	}

	ObjectPtr StructInfo::CreateDefault() const
	{
		ENGINE_CONTRACT_STUB();
		return ObjectPtr(nullptr, [](void* /*object*/) {});
	}

	Result<Json> StructInfo::ToJson(const void* /*object*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "StructInfo::ToJson is an M3 contract stub");
	}

	Status StructInfo::FromJson(void* /*object*/, const JsonReader& /*reader*/, const ReadContext& /*context*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "StructInfo::FromJson is an M3 contract stub");
	}

	Status StructInfo::ApplyMergePatch(void* /*object*/, const JsonReader& /*patch*/, const ReadContext& /*context*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "StructInfo::ApplyMergePatch is an M3 contract stub");
	}

	void StructInfo::Validate(const void* /*object*/, const ResolveContext& /*resolve*/, ValidationContext& /*context*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	Json StructInfo::MakeDefaultJson() const
	{
		ENGINE_CONTRACT_STUB();
		return Json::object();
	}

	void StructInfo::AddField(FieldInfo::Specification /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void StructInfo::Generate(void* /*object*/, Random& /*random*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	void StructInfo::AddValidator(StructValidator /*validator*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void StructInfo::AddGenerator(StructGenerator /*generator*/)
	{
		ENGINE_CONTRACT_STUB();
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
