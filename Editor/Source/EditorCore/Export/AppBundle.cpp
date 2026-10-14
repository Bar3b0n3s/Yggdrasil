#include "EditorPCH.h"
#include "EditorCore/Export/AppBundle.h"

#include "Engine/Core/Error.h"

namespace Engine {

	std::string MakeBundleIdentifier(std::string_view /*company*/, std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<std::string> WriteInfoPlist(const AppBundleInfo& /*info*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M15 app bundle contract (stream A)"));
	}

	Result<Buffer> BuildIcns(std::span<const ExecutableIconImage> /*icons*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M15 app bundle contract (stream A)"));
	}

	Result<std::string> RewriteMoltenVkIcd(std::string_view /*sdkIcdJson*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M15 app bundle contract (stream A)"));
	}

	Status CopyAppBundleBinaries(const std::filesystem::path& /*bundle*/, std::string_view /*name*/, const std::filesystem::path& /*runtime*/,
		const std::filesystem::path& /*runtimeOutput*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M15 app bundle contract (stream A)"));
	}

	Status WriteAppBundleResources(const std::filesystem::path& /*bundle*/, const AppBundleInfo& /*info*/,
		std::span<const ExecutableIconImage> /*icons*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M15 app bundle contract (stream A)"));
	}

}
