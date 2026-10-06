#include "EnginePCH.h"
#include "Engine/Core/VfsPath.h"

// M1 contract stub (Roadmap rule 3): stream C implements parsing, validation and path arithmetic.

namespace Engine {

	Result<VfsPath> VfsPath::Parse(std::string_view /*text*/)
	{
		return MakeError(ErrorCode::Unsupported, "VfsPath::Parse is an M1 contract stub");
	}

	Result<VfsPath> VfsPath::Create(std::string_view /*scheme*/, std::string_view /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "VfsPath::Create is an M1 contract stub");
	}

	Status VfsPath::ValidateScheme(std::string_view /*scheme*/)
	{
		return MakeError(ErrorCode::Unsupported, "VfsPath::ValidateScheme is an M1 contract stub");
	}

	Status VfsPath::ValidateRelativePath(std::string_view /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "VfsPath::ValidateRelativePath is an M1 contract stub");
	}

	std::string VfsPath::ToString() const
	{
		return {};
	}

	std::string_view VfsPath::GetFileName() const
	{
		return {};
	}

	std::string_view VfsPath::GetExtension() const
	{
		return {};
	}

	std::string_view VfsPath::GetStem() const
	{
		return {};
	}

	VfsPath VfsPath::GetParent() const
	{
		return {};
	}

	Result<VfsPath> VfsPath::Join(std::string_view /*relative*/) const
	{
		return MakeError(ErrorCode::Unsupported, "VfsPath::Join is an M1 contract stub");
	}

	bool VfsPath::IsUnder(const VfsPath& /*directory*/) const
	{
		return false;
	}

}
