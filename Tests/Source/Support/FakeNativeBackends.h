#pragma once

#include "EditorCore/Native/NativeBuild.h"
#include "EditorCore/Native/NativeModuleHost.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Fake native backends for the editor's native module host (Docs/Decisions/0030-m19-contract.md decision 37), so the host's
// and the native methods' tests run without a toolchain or a shared library. The fake build succeeds at its first Poll
// (or fails with the diagnostics it was given): it writes into the request's output directory a "library", a text file
// that names one of the test modules of Support/NativeTestModule.h, and the build record NativeBuild.json of decision 30
// with the real ComputeNativeSourceHash; the fake loader reads that file back and returns the named module's entry point.

namespace Engine {

	namespace Test {

		// The test modules a fake build can hold.
		enum class FakeNativeModule : uint8_t
		{
			Layout1, // GetNativeTestModule
			Layout2, // GetNativeTestModuleV2
			Failing  // GetFailingNativeTestModule
		};

		class FakeNativeBuildBackend final : public INativeBuildBackend
		{
		public:
			// The module the next successful builds hold.
			void SetModule(FakeNativeModule module) { m_Module = module; }
			// The diagnostics of the next builds; one of severity Error makes them fail.
			void SetDiagnostics(std::vector<NativeBuildDiagnostic> diagnostics) { m_Diagnostics = std::move(diagnostics); }
			[[nodiscard]] uint32_t GetStartCount() const { return m_StartCount; }
			// The NativeApi.h the source hash of a fake build covers (NativeModuleHostSpecification::ApiHeader).
			void SetApiHeader(std::filesystem::path header) { m_ApiHeader = std::move(header); }
			// The executable a successful NativeBuildKind::Runtime build outputs (a copy, named after it): a real Runtime
			// (Test::GetBuiltExecutablePath("Runtime")), so an export's later steps (resources, smoke test) run on it.
			void SetRuntimeExecutable(std::filesystem::path executable) { m_RuntimeExecutable = std::move(executable); }
			// The last request Start received.
			[[nodiscard]] const NativeBuildRequest& GetLastRequest() const { return m_LastRequest; }

			[[nodiscard]] Result<Scope<INativeBuildJob>> Start(const NativeBuildRequest& request) override;
		private:
			FakeNativeModule m_Module = FakeNativeModule::Layout1;
			std::vector<NativeBuildDiagnostic> m_Diagnostics{};
			std::filesystem::path m_ApiHeader{};
			std::filesystem::path m_RuntimeExecutable{};
			NativeBuildRequest m_LastRequest{};
			uint32_t m_StartCount = 0;
		};

		class FakeNativeLibraryLoader final : public INativeLibraryLoader
		{
		public:
			// Errors: Io when the file cannot be read; NotFound when it names no test module.
			[[nodiscard]] Result<NativeLoadedLibrary> Load(const std::filesystem::path& library) override;

			[[nodiscard]] uint32_t GetLoadCount() const { return m_LoadCount; }
		private:
			uint32_t m_LoadCount = 0;
		};

		// The file name of a fake build's library in its build directory.
		inline constexpr std::string_view FakeNativeLibraryName = "FakeNative.library";

	}

}
