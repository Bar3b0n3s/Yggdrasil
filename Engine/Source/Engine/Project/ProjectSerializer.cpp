#include "EnginePCH.h"
#include "Engine/Project/ProjectSerializer.h"

#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream C (serialization and project) implements .eproj reading and writing.

namespace Engine {

	Result<Json> ProjectSerializer::ToJson(const ProjectSettings& /*settings*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectSerializer::ToJson is an M3 contract stub");
	}

	Result<std::string> ProjectSerializer::SaveToString(const ProjectSettings& /*settings*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectSerializer::SaveToString is an M3 contract stub");
	}

	Result<ProjectSettings> ProjectSerializer::FromJson(const Json& /*document*/, const TypeRegistry& /*registry*/,
		const ProjectLoadOptions& /*options*/, ProjectLoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectSerializer::FromJson is an M3 contract stub");
	}

	Result<ProjectSettings> ProjectSerializer::LoadFromString(std::string_view /*text*/, const TypeRegistry& /*registry*/,
		const ProjectLoadOptions& /*options*/, ProjectLoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectSerializer::LoadFromString is an M3 contract stub");
	}

	Status ProjectSerializer::SaveToFile(const ProjectSettings& /*settings*/, const TypeRegistry& /*registry*/, VirtualFileSystem& /*vfs*/,
		const VfsPath& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectSerializer::SaveToFile is an M3 contract stub");
	}

	Result<ProjectSettings> ProjectSerializer::LoadFromFile(const VirtualFileSystem& /*vfs*/, const VfsPath& /*path*/,
		const TypeRegistry& /*registry*/, const ProjectLoadOptions& /*options*/, ProjectLoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectSerializer::LoadFromFile is an M3 contract stub");
	}

}
