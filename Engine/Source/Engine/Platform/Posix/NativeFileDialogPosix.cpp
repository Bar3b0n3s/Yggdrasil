#include "EnginePCH.h"
#include "Engine/Platform/NativeFileDialog.h"

// The native file dialog on Linux and macOS: none. Windows backends only (ADR 0028 decision 10): Open refuses with
// Unsupported naming the platform, as ResourcePatcher::Patch does here, and the editor's FileDialog shows its ImGui picker
// instead, exactly as it does headless (Docs/Decisions/0029-m18-contract.md decision 19). No object is ever created here.

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/Error.h"

	#include <format>
	#include <string_view>

namespace Engine {

	namespace Utils {

	#if defined(ENGINE_PLATFORM_MACOS)
		static constexpr std::string_view NativeFileDialogPlatform = "macOS";
	#else
		static constexpr std::string_view NativeFileDialogPlatform = "Linux";
	#endif

		// The refusal of every native dialog on this platform.
		[[nodiscard]] static Error MakeNativeFileDialogUnsupported()
		{
			return Error(ErrorCode::Unsupported, std::format("{} has no native file dialog: only Windows has a backend", NativeFileDialogPlatform))
				.WithHint("the editor shows its own file picker instead");
		}

	}

	struct NativeFileDialog::State
	{
	};

	NativeFileDialog::NativeFileDialog(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	NativeFileDialog::~NativeFileDialog() = default;

	Result<Scope<NativeFileDialog>> NativeFileDialog::Open(const FileDialogRequest& request, const Window* /*owner*/)
	{
		ENGINE_TRY(ValidateFileDialogRequest(request));
		return std::unexpected(Utils::MakeNativeFileDialogUnsupported());
	}

	Result<FileDialogOutcome> NativeFileDialog::Poll()
	{
		return std::unexpected(Utils::MakeNativeFileDialogUnsupported());
	}

	void NativeFileDialog::Cancel()
	{
	}

}

#endif
