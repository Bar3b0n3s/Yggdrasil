#include "TestsPCH.h"
#include "Support/FakeNativeBackends.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Support/NativeTestModule.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

namespace Engine {

	namespace Test {

		namespace Utils {

			[[nodiscard]] static std::string_view GetFakeModuleName(FakeNativeModule module)
			{
				switch (module)
				{
					case FakeNativeModule::Layout1: return "Layout1";
					case FakeNativeModule::Layout2: return "Layout2";
					case FakeNativeModule::Failing: return "Failing";
				}
				return "Layout1";
			}

			[[nodiscard]] static std::string_view GetSeverityName(DiagnosticSeverity severity)
			{
				return severity == DiagnosticSeverity::Error ? "Error" : "Warning";
			}

			// A build that resolved when it was started.
			class FakeNativeBuildJob final : public INativeBuildJob
			{
			public:
				explicit FakeNativeBuildJob(Result<NativeBuildOutcome> outcome)
					: m_Outcome(std::move(outcome))
				{
				}

				[[nodiscard]] std::optional<Result<NativeBuildOutcome>> Poll() override
				{
					return m_Outcome;
				}

				void Cancel() override
				{
				}
			private:
				Result<NativeBuildOutcome> m_Outcome;
			};

			// A build that runs until it is cancelled (FakeNativeBuildBackend::SetPending).
			class PendingFakeNativeBuildJob final : public INativeBuildJob
			{
			public:
				PendingFakeNativeBuildJob(Ref<uint32_t> cancelCount, bool answersCancel)
					: m_CancelCount(std::move(cancelCount)), m_AnswersCancel(answersCancel)
				{
				}

				[[nodiscard]] std::optional<Result<NativeBuildOutcome>> Poll() override
				{
					if (!m_Cancelled || !m_AnswersCancel)
						return std::nullopt;
					return Result<NativeBuildOutcome>(MakeError(ErrorCode::Cancelled, "the fake native build was cancelled"));
				}

				void Cancel() override
				{
					if (m_Cancelled)
						return;
					m_Cancelled = true;
					++*m_CancelCount;
				}
			private:
				Ref<uint32_t> m_CancelCount;
				bool m_AnswersCancel = true;
				bool m_Cancelled = false;
			};

			[[nodiscard]] static Status WriteText(const std::filesystem::path& path, std::string_view text)
			{
				return FileSystem::WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size())));
			}

		}

		Result<Scope<INativeBuildJob>> FakeNativeBuildBackend::Start(const NativeBuildRequest& request)
		{
			++m_StartCount;
			m_LastRequest = request;
			// Scripts/BuildNative.py refuses an output directory that exists and is not empty, writing no record (exit code 2),
			// which the real backend reports as an Io error when the build resolves.
			std::error_code status;
			if (std::filesystem::exists(request.OutputDirectory, status) && !std::filesystem::is_empty(request.OutputDirectory, status))
			{
				return CreateScope<Utils::FakeNativeBuildJob>(Result<NativeBuildOutcome>(MakeError(ErrorCode::Io,
					"the fake native build exited with code 2 without a build record: --out {} must be a missing or empty directory",
					FileSystem::PathToUtf8(request.OutputDirectory))));
			}
			ENGINE_TRY_ASSIGN(const std::vector<std::string> sources, ListNativeSources(request.ProjectRoot));
			ENGINE_TRY_ASSIGN(const std::string hash, ComputeNativeSourceHash(request.ProjectRoot, request.Configuration, m_ApiHeader));
			ENGINE_TRY(FileSystem::CreateDirectories(request.OutputDirectory));
			if (m_Pending)
				return CreateScope<Utils::PendingFakeNativeBuildJob>(m_CancelCount, m_AnswersCancel);
			const bool succeeded = std::ranges::none_of(m_Diagnostics, [](const NativeBuildDiagnostic& diagnostic)
			{
				return diagnostic.Severity == DiagnosticSeverity::Error;
			});
			NativeBuildOutcome outcome;
			outcome.Succeeded = succeeded;
			outcome.SourceHash = hash;
			outcome.Diagnostics = m_Diagnostics;
			outcome.Log = request.OutputDirectory / "Build.log";
			ENGINE_TRY(Utils::WriteText(outcome.Log, "fake build\n"));
			if (succeeded && request.Kind == NativeBuildKind::Runtime)
			{
				outcome.Output = request.OutputDirectory / m_RuntimeExecutable.filename();
				std::error_code error;
				if (!std::filesystem::copy_file(m_RuntimeExecutable, outcome.Output, std::filesystem::copy_options::overwrite_existing, error) || error)
					return MakeError(ErrorCode::Io, "cannot copy '{}' as the fake build's Runtime: {}", FileSystem::PathToUtf8(m_RuntimeExecutable), error.message());
				// The symbols the linker writes beside it (Scripts/Premake/NativeModule.lua), which an export archives.
				for (const std::string_view extension : { ".pdb", ".map" })
				{
					std::filesystem::path symbols = outcome.Output;
					symbols.replace_extension(FileSystem::PathFromUtf8(extension));
					ENGINE_TRY(Utils::WriteText(symbols, std::format("fake symbols ({})\n", extension)));
				}
			}
			else if (succeeded)
			{
				outcome.Output = request.OutputDirectory / FileSystem::PathFromUtf8(FakeNativeLibraryName);
				ENGINE_TRY(Utils::WriteText(outcome.Output, Utils::GetFakeModuleName(m_Module)));
			}

			// The build record (decision 30).
			Json record = Json::object();
			record["Format"] = "NativeBuild";
			record["Version"] = 1;
			record["Kind"] = request.Kind == NativeBuildKind::Module ? "Module" : "Runtime";
			record["Configuration"] = std::string(ExportConfigurationToString(request.Configuration));
			record["SourceHash"] = hash;
			record["Sources"] = sources;
			record["Succeeded"] = succeeded;
			record["Output"] = succeeded ? FileSystem::PathToUtf8(outcome.Output.filename()) : std::string();
			record["Log"] = "Build.log";
			Json diagnostics = Json::array();
			for (const NativeBuildDiagnostic& diagnostic : m_Diagnostics)
			{
				Json entry = Json::object();
				entry["Severity"] = std::string(Utils::GetSeverityName(diagnostic.Severity));
				entry["Code"] = diagnostic.Code;
				entry["File"] = diagnostic.File;
				entry["Line"] = diagnostic.Line;
				entry["Column"] = diagnostic.Column;
				entry["Message"] = diagnostic.Message;
				diagnostics.push_back(std::move(entry));
			}
			record["Diagnostics"] = std::move(diagnostics);
			ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(record));
			ENGINE_TRY(Utils::WriteText(request.OutputDirectory / FileSystem::PathFromUtf8(NativeBuildRecordFileName), text));
			return CreateScope<Utils::FakeNativeBuildJob>(Result<NativeBuildOutcome>(std::move(outcome)));
		}

		Result<NativeLoadedLibrary> FakeNativeLibraryLoader::Load(const std::filesystem::path& library)
		{
			++m_LoadCount;
			ENGINE_TRY_ASSIGN(const std::string name, FileSystem::ReadText(library));
			for (const FakeNativeModule module : { FakeNativeModule::Layout1, FakeNativeModule::Layout2, FakeNativeModule::Failing })
			{
				if (name != Utils::GetFakeModuleName(module))
					continue;
				switch (module)
				{
					case FakeNativeModule::Layout1: return NativeLoadedLibrary{ .Entry = GetNativeTestModule(), .Keepalive = nullptr };
					case FakeNativeModule::Layout2: return NativeLoadedLibrary{ .Entry = GetNativeTestModuleV2(), .Keepalive = nullptr };
					case FakeNativeModule::Failing: return NativeLoadedLibrary{ .Entry = GetFailingNativeTestModule(), .Keepalive = nullptr };
				}
			}
			return MakeError(ErrorCode::NotFound, "'{}' names no test module ('{}')", FileSystem::PathToUtf8(library), name);
		}

	}

}
