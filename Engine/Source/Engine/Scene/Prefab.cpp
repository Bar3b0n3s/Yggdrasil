#include "EnginePCH.h"
#include "Engine/Scene/Prefab.h"

#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Migrations.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/StructuralValidator.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream E (prefabs, after B and C land) implements prefab documents.

namespace Engine {

	Result<Prefab> Prefab::FromJson(const Json& /*document*/, const TypeRegistry& /*registry*/, const LoadOptions& /*options*/,
		LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Prefab::FromJson is an M3 contract stub");
	}

	Result<Prefab> Prefab::LoadFromString(std::string_view /*text*/, const TypeRegistry& /*registry*/, const LoadOptions& /*options*/,
		LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Prefab::LoadFromString is an M3 contract stub");
	}

	Result<Prefab> Prefab::LoadFromFile(const VirtualFileSystem& /*vfs*/, const VfsPath& /*path*/, const TypeRegistry& /*registry*/,
		const LoadOptions& /*options*/, LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Prefab::LoadFromFile is an M3 contract stub");
	}

	Result<Prefab> Prefab::CreateFromEntity(ConstEntity /*root*/, std::string /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Prefab::CreateFromEntity is an M3 contract stub");
	}

	Result<Json> Prefab::ToJson() const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Prefab::ToJson is an M3 contract stub");
	}

	Result<std::string> Prefab::SaveToString(JsonStyle /*style*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Prefab::SaveToString is an M3 contract stub");
	}

	Status Prefab::SaveToFile(VirtualFileSystem& /*vfs*/, const VfsPath& /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Prefab::SaveToFile is an M3 contract stub");
	}

	const Json& Prefab::GetEntities() const
	{
		ENGINE_CONTRACT_STUB();
		static const Json EmptyArray = Json::array();
		return EmptyArray;
	}

	const Json* Prefab::FindEntity(UUID /*id*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::vector<UUID> Prefab::GetEntityIDs() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
