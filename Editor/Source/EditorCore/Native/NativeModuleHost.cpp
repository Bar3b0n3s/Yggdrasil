#include "EditorPCH.h"
#include "EditorCore/Native/NativeModuleHost.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Native/NativeModule.h"

#include <format>
#include <utility>

#if !defined(ENGINE_REPO_ROOT)
	#error "ENGINE_REPO_ROOT must be defined for the editor (Dependencies.lua, ApplyFirstPartySettings): module builds compile against its NativeApi.h"
#endif

namespace Engine {

	namespace {

		// The loader until stream N-C lands SharedLibrary's (decision 31): every Load is Unsupported.
		class ContractNativeLibraryLoader final : public INativeLibraryLoader
		{
		public:
			[[nodiscard]] Result<NativeLoadedLibrary> Load(const std::filesystem::path& library) override
			{
				ENGINE_CONTRACT_STUB();
				return std::unexpected(Error(ErrorCode::Unsupported,
					std::format("loading the native module '{}' (Docs/Decisions/0030-m19-contract.md decision 31, stream N-C)", FileSystem::PathToUtf8(library))));
			}
		};

	}

	namespace Utils {

		// The host's refusal until stream N-C lands it.
		[[nodiscard]] static std::unexpected<Error> MakeHostStubError(std::string_view what)
		{
			return std::unexpected(Error(ErrorCode::Unsupported, std::format("{} (Docs/Decisions/0030-m19-contract.md decisions 29 to 31, stream N-C)", what)));
		}

	}

	struct NativeModuleHost::State
	{
		NativeModuleHostSpecification Specification{};
	};

	Scope<INativeLibraryLoader> CreateNativeLibraryLoader()
	{
		ENGINE_CONTRACT_STUB();
		return CreateScope<ContractNativeLibraryLoader>();
	}

	NativeModuleHostSpecification MakeEditorNativeModuleHostSpecification(Ref<INativeBuildBackend> build, Ref<INativeLibraryLoader> loader)
	{
		NativeModuleHostSpecification specification;
		specification.Build = build != nullptr ? std::move(build) : Ref<INativeBuildBackend>(CreateNativeBuildBackend());
		specification.Loader = loader != nullptr ? std::move(loader) : Ref<INativeLibraryLoader>(CreateNativeLibraryLoader());
#if defined(ENGINE_DEBUG)
		specification.EditorConfiguration = ExportConfiguration::Debug;
#else
		specification.EditorConfiguration = ExportConfiguration::Release;
#endif
		specification.ApiHeader = std::filesystem::path(ENGINE_REPO_ROOT) / "Engine" / "Source" / "Engine" / "Native" / "NativeApi.h";
		return specification;
	}

	NativeModuleHost::NativeModuleHost(NativeModuleHostSpecification specification)
		: m_State(CreateScope<State>())
	{
		ENGINE_ASSERT(specification.Build != nullptr && specification.Loader != nullptr, "the native module host needs a build backend and a library loader");
		m_State->Specification = std::move(specification);
	}

	NativeModuleHost::~NativeModuleHost() = default;

	bool NativeModuleHost::IsAllowed() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Status NativeModuleHost::ReadPreference(const EditorContext& /*editor*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeHostStubError("reading the Allow native code preference");
	}

	void NativeModuleHost::QueuePreferenceChange(bool /*allowed*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void NativeModuleHost::Update(EditorContext& /*editor*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status NativeModuleHost::OnProjectOpened(EditorContext& /*editor*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeHostStubError("loading a project's native module");
	}

	void NativeModuleHost::OnProjectClosed()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<NativeModuleStatus> NativeModuleHost::GetStatus(const EditorContext& /*editor*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeHostStubError("the native module's status");
	}

	Status NativeModuleHost::StartBuild(EditorContext& /*editor*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeHostStubError("building the native module");
	}

	std::optional<Result<NativeBuildOutcome>> NativeModuleHost::PollBuild(EditorContext& /*editor*/)
	{
		ENGINE_CONTRACT_STUB();
		return Result<NativeBuildOutcome>(Utils::MakeHostStubError("a native module build"));
	}

	void NativeModuleHost::CancelBuild()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<INativeBuildJob>> NativeModuleHost::StartRuntimeBuild(EditorContext& /*editor*/, ExportConfiguration configuration,
		const std::filesystem::path& /*outputDirectory*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeHostStubError(std::format("building the {} Runtime with the native module", ExportConfigurationToString(configuration)));
	}

	Result<NativeLoadReport> NativeModuleHost::Load(EditorContext& /*editor*/)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeHostStubError("loading the native module");
	}

	Ref<const NativeModuleTypes> NativeModuleHost::GetLoadedTypes() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

}
