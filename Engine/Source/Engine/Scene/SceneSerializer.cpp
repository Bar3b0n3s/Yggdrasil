#include "EnginePCH.h"
#include "Engine/Scene/SceneSerializer.h"

#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Migrations.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/StructuralValidator.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream C (serialization) implements scene documents, entity snapshots and the
// load pipeline (header, migration, structural pre-validation, entity creation in canonical order).

namespace Engine {

	Result<Json> SceneSerializer::ToJson(const Scene& /*scene*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneSerializer::ToJson is an M3 contract stub");
	}

	Result<std::string> SceneSerializer::SaveToString(const Scene& /*scene*/, JsonStyle /*style*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneSerializer::SaveToString is an M3 contract stub");
	}

	Status SceneSerializer::FromJson(Scene& /*scene*/, const Json& /*document*/, const LoadOptions& /*options*/, LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneSerializer::FromJson is an M3 contract stub");
	}

	Status SceneSerializer::LoadFromString(Scene& /*scene*/, std::string_view /*text*/, const LoadOptions& /*options*/, LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneSerializer::LoadFromString is an M3 contract stub");
	}

	Status SceneSerializer::SaveToFile(const Scene& /*scene*/, VirtualFileSystem& /*vfs*/, const VfsPath& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneSerializer::SaveToFile is an M3 contract stub");
	}

	Status SceneSerializer::LoadFromFile(Scene& /*scene*/, const VirtualFileSystem& /*vfs*/, const VfsPath& /*path*/,
		const LoadOptions& /*options*/, LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneSerializer::LoadFromFile is an M3 contract stub");
	}

	Result<Json> SceneSerializer::EntityToJson(ConstEntity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneSerializer::EntityToJson is an M3 contract stub");
	}

	Result<Entity> SceneSerializer::EntityFromJson(Scene& /*scene*/, const JsonReader& /*entity*/, std::optional<uint32_t> /*siblingIndex*/,
		const LoadOptions& /*options*/, LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneSerializer::EntityFromJson is an M3 contract stub");
	}

	Status SceneSerializer::ApplyEntityJson(Entity /*entity*/, const JsonReader& /*json*/, const LoadOptions& /*options*/,
		LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneSerializer::ApplyEntityJson is an M3 contract stub");
	}

}
