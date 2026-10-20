#include "EnginePCH.h"
#include "Engine/Native/NativeModule.h"

#include "Engine/Core/Error.h"
#include "Engine/Core/Hash.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace Engine {

	NativeModuleTypes::NativeModuleTypes(ConstructionKey /*key*/, std::vector<NativeTypeInfo> components, std::vector<NativeTypeInfo> resources,
		std::vector<NativeSystemInfo> systems, Ref<const void> keepalive)
		: m_Components(std::move(components)), m_Resources(std::move(resources)), m_Systems(std::move(systems)), m_Keepalive(std::move(keepalive))
	{
		ENGINE_CONTRACT_STUB();
	}

	const NativeTypeInfo* NativeModuleTypes::FindComponent(std::string_view registryName) const
	{
		const auto found = std::ranges::find(m_Components, registryName, &NativeTypeInfo::RegistryName);
		return found == m_Components.end() ? nullptr : &*found;
	}

	const NativeTypeInfo* NativeModuleTypes::FindResource(std::string_view name) const
	{
		const auto found = std::ranges::find(m_Resources, name, &NativeTypeInfo::Name);
		return found == m_Resources.end() ? nullptr : &*found;
	}

	Result<Ref<const NativeModuleTypes>> BuildNativeModuleTypes(const NativeModuleEntryPoint& entry, Ref<const void> /*keepalive*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("registering a native module of interface version {} (Docs/Decisions/0030-m19-contract.md decision 26, stream N-B)", entry.ApiVersion)));
	}

	Json NativeValueToJson(const NativeTypeInfo& type, std::span<const std::byte> /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json::object({ { "Unsupported", type.RegistryName } });
	}

	Result<NativeValueReadReport> NativeValueFromJson(const NativeTypeInfo& type, const Json& /*json*/, std::string_view pointer, std::span<std::byte> /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("reading the native value '{}' at '{}' (Docs/Decisions/0030-m19-contract.md decision 27, stream N-B)", type.RegistryName, pointer)));
	}

	Result<NativeCanonicalValue> CanonicalizeNativeValue(const NativeTypeInfo& type, const Json& /*json*/, std::string_view pointer)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("canonicalizing the native value '{}' at '{}' (Docs/Decisions/0030-m19-contract.md decision 27, stream N-B)", type.RegistryName, pointer)));
	}

	Json GetNativeTypeSchema(const NativeTypeInfo& /*type*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json::object();
	}

	void AppendNativeValueHash(const NativeTypeInfo& /*type*/, std::span<const std::byte> /*value*/, XXH64Hasher& /*hasher*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
