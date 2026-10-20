#include "EditorPCH.h"
#include "EditorCore/Native/NativeBuild.h"

#include "Engine/Core/Error.h"
#include "Engine/Core/FileSystem.h"

#include <format>
#include <system_error>

namespace Engine {

	namespace {

		// The backend until stream N-A lands the process (decision 30): every Start is Unsupported.
		class ContractNativeBuildBackend final : public INativeBuildBackend
		{
		public:
			[[nodiscard]] Result<Scope<INativeBuildJob>> Start(const NativeBuildRequest& request) override
			{
				ENGINE_CONTRACT_STUB();
				return std::unexpected(Error(ErrorCode::Unsupported,
					std::format("building the native module of '{}' (Docs/Decisions/0030-m19-contract.md decision 30, stream N-A)",
						FileSystem::PathToUtf8(request.ProjectRoot))));
			}
		};

	}

	Scope<INativeBuildBackend> CreateNativeBuildBackend()
	{
		ENGINE_CONTRACT_STUB();
		return CreateScope<ContractNativeBuildBackend>();
	}

	bool HasNativeModuleDirectory(const std::filesystem::path& projectRoot)
	{
		std::error_code error;
		return std::filesystem::is_directory(projectRoot / FileSystem::PathFromUtf8(NativeSourceDirectoryName), error) && !error;
	}

	Result<NativeBuildRecord> ParseNativeBuildRecord(std::string_view text)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("reading a native build record of {} bytes (Docs/Decisions/0030-m19-contract.md decision 30, stream N-A)", text.size())));
	}

	Result<std::vector<std::string>> ListNativeSources(const std::filesystem::path& projectRoot)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("listing the native sources of '{}' (Docs/Decisions/0030-m19-contract.md decision 30, stream N-A)", FileSystem::PathToUtf8(projectRoot))));
	}

	Result<std::string> ComputeNativeSourceHash(const std::filesystem::path& projectRoot, ExportConfiguration /*configuration*/,
		const std::filesystem::path& /*apiHeader*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("hashing the native sources of '{}' (Docs/Decisions/0030-m19-contract.md decision 30, stream N-A)", FileSystem::PathToUtf8(projectRoot))));
	}

}
