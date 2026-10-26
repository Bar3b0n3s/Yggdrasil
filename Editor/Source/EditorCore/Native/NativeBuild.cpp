#include "EditorPCH.h"
#include "EditorCore/Native/NativeBuild.h"

#include "Engine/Core/Buffer.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/SharedLibrary.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
#include <system_error>

#if !defined(ENGINE_REPO_ROOT) || !defined(ENGINE_PYTHON_EXECUTABLE)
	#error "ENGINE_REPO_ROOT and ENGINE_PYTHON_EXECUTABLE must be defined for the editor (Dependencies.lua, ApplyFirstPartySettings): it runs Scripts/BuildNative.py"
#endif

// The native build's C++ half (NativeBuild.h; Docs/Decisions/0030-m19-contract.md decision 30): the sources and their hash
// as Scripts/Lib/native_build.py computes them, the strict reader of the record Scripts/BuildNative.py writes, and the
// backend that runs BuildNative.py as one process by fixed paths with fixed arguments. The process code is portable (the
// Process API), but Start refuses first with SharedLibrary::CheckSupported, so Linux and macOS never start a build
// (decision 34). Cancelling a build kills its Python process; the MSBuild it started then finishes on its own, writing
// only below the build's own output directory, which no later build reuses: the editor numbers its builds' directories
// anew, and an export builds into a new numbered directory beside its staging directory, skipping a cancelled build's
// leftover it cannot remove yet (Exporter.cpp's PrepareNativeBuildDirectory).

namespace Engine {

	namespace Utils {

		// The record's members, in the order Scripts/BuildNative.py writes them (decision 30).
		constexpr std::array<std::string_view, 10> NativeRecordKeys = { "Format", "Version", "Kind", "Configuration", "SourceHash", "Sources", "Succeeded",
			"Output", "Log", "Diagnostics" };
		constexpr std::array<std::string_view, 6> NativeDiagnosticKeys = { "Severity", "Code", "File", "Line", "Column", "Message" };
		constexpr std::string_view NativeRecordFormat = "NativeBuild";
		constexpr uint32_t NativeRecordVersion = 1;
		constexpr size_t NativeHashDigits = 16;
		// The source hash's first line and the name it gives NativeApi.h: its path below the Engine include root.
		constexpr std::string_view NativeSourceHashPrefix = "EngineNativeSources 1";
		constexpr std::string_view NativeApiHashName = "Engine/Native/NativeApi.h";
		// The script the backend runs, below the repository, and the most of its output an error quotes.
		constexpr std::string_view NativeBuildScript = "Scripts/BuildNative.py";
		constexpr size_t NativeBuildOutputTail = 2000;
		constexpr std::chrono::milliseconds NativeBuildDrainTimeout{ 5000 };

		[[nodiscard]] static Error MakeRecordError(std::string_view pointer, std::string message)
		{
			ErrorLocation location;
			location.JsonPointer = std::string(pointer);
			return Error(ErrorCode::Validation, std::move(message)).WithLocation(std::move(location));
		}

		[[nodiscard]] static Status ExpectRecordMembers(const JsonReader& reader, std::span<const std::string_view> keys)
		{
			ENGINE_TRY(reader.ExpectType(JsonType::Object));
			ENGINE_TRY_ASSIGN(const std::vector<std::string> unknown, reader.FindUnknownMembers(keys));
			if (!unknown.empty())
				return std::unexpected(MakeRecordError(JsonReader::AppendPointer(reader.GetPointer(), unknown.front()), std::format("unknown member '{}' in a native build record", unknown.front())));
			for (const std::string_view key : keys)
			{
				if (!reader.HasMember(key))
					return std::unexpected(MakeRecordError(reader.GetPointer(), std::format("missing required field '{}'", key)));
			}
			return {};
		}

		// A file name in the build's directory: no separator, no drive, not "." or "..".
		[[nodiscard]] static bool IsPlainFileName(std::string_view name)
		{
			return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string_view::npos;
		}

		[[nodiscard]] static Result<std::string> ReadPlainFileName(const JsonReader& reader, bool allowEmpty)
		{
			ENGINE_TRY_ASSIGN(std::string name, reader.ReadString());
			if (!(allowEmpty && name.empty()) && !IsPlainFileName(name))
				return std::unexpected(MakeRecordError(reader.GetPointer(), std::format("expected a file name in the build's directory, got '{}'", name)));
			return name;
		}

		[[nodiscard]] static Result<NativeBuildKind> ReadBuildKind(const JsonReader& reader)
		{
			ENGINE_TRY_ASSIGN(const std::string text, reader.ReadString());
			if (text == "Module")
				return NativeBuildKind::Module;
			if (text == "Runtime")
				return NativeBuildKind::Runtime;
			return std::unexpected(MakeRecordError(reader.GetPointer(), std::format("expected \"Module\" or \"Runtime\", got '{}'", text)));
		}

		[[nodiscard]] static Result<ExportConfiguration> ReadConfiguration(const JsonReader& reader)
		{
			ENGINE_TRY_ASSIGN(const std::string text, reader.ReadString());
			for (const ExportConfiguration configuration : { ExportConfiguration::Debug, ExportConfiguration::Release, ExportConfiguration::Dist })
			{
				if (text == ExportConfigurationToString(configuration))
					return configuration;
			}
			return std::unexpected(MakeRecordError(reader.GetPointer(), std::format("expected \"Debug\", \"Release\" or \"Dist\", got '{}'", text)));
		}

		// 16 lowercase hex digits, or with `allowEmpty` (a failed build's record) also "": a build that failed before it could
		// hash the sources (not Windows, or a source it could not read) records why without a hash.
		[[nodiscard]] static Result<std::string> ReadSourceHash(const JsonReader& reader, bool allowEmpty)
		{
			ENGINE_TRY_ASSIGN(std::string hash, reader.ReadString());
			if (allowEmpty && hash.empty())
				return hash;
			const bool lowercaseHex = std::ranges::all_of(hash, [](char character)
			{
				return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
			});
			if (hash.size() != NativeHashDigits || !lowercaseHex)
				return std::unexpected(MakeRecordError(reader.GetPointer(), std::format("expected 16 lowercase hex digits, got '{}'", hash)));
			return hash;
		}

		[[nodiscard]] static Result<NativeBuildDiagnostic> ReadDiagnostic(const JsonReader& reader)
		{
			ENGINE_TRY(ExpectRecordMembers(reader, NativeDiagnosticKeys));
			NativeBuildDiagnostic diagnostic;
			ENGINE_TRY_ASSIGN(const JsonReader severity, reader.GetMember("Severity"));
			ENGINE_TRY_ASSIGN(const std::string severityText, severity.ReadString());
			if (severityText == "Error")
				diagnostic.Severity = DiagnosticSeverity::Error;
			else if (severityText == "Warning")
				diagnostic.Severity = DiagnosticSeverity::Warning;
			else
				return std::unexpected(MakeRecordError(severity.GetPointer(), std::format("expected \"Error\" or \"Warning\", got '{}'", severityText)));
			ENGINE_TRY_ASSIGN(diagnostic.Code, reader.ReadMember<std::string>("Code"));
			ENGINE_TRY_ASSIGN(diagnostic.File, reader.ReadMember<std::string>("File"));
			ENGINE_TRY_ASSIGN(diagnostic.Line, reader.ReadMember<uint32_t>("Line"));
			ENGINE_TRY_ASSIGN(diagnostic.Column, reader.ReadMember<uint32_t>("Column"));
			ENGINE_TRY_ASSIGN(diagnostic.Message, reader.ReadMember<std::string>("Message"));
			return diagnostic;
		}

		[[nodiscard]] static Result<NativeBuildRecord> ReadRecord(const Json& document)
		{
			const JsonReader root(document);
			ENGINE_TRY(ExpectRecordMembers(root, NativeRecordKeys));
			ENGINE_TRY_ASSIGN(const JsonReader format, root.GetMember("Format"));
			ENGINE_TRY_ASSIGN(const std::string formatText, format.ReadString());
			if (formatText != NativeRecordFormat)
				return std::unexpected(MakeRecordError(format.GetPointer(), std::format("expected \"{}\", got '{}'", NativeRecordFormat, formatText)));
			ENGINE_TRY_ASSIGN(const JsonReader version, root.GetMember("Version"));
			ENGINE_TRY_ASSIGN(const uint32_t versionValue, version.ReadUInt32());
			if (versionValue != NativeRecordVersion)
				return std::unexpected(MakeRecordError(version.GetPointer(), std::format("expected version {}, got {}", NativeRecordVersion, versionValue)));

			NativeBuildRecord record;
			ENGINE_TRY_ASSIGN(const JsonReader kind, root.GetMember("Kind"));
			ENGINE_TRY_ASSIGN(record.Kind, ReadBuildKind(kind));
			ENGINE_TRY_ASSIGN(const JsonReader configuration, root.GetMember("Configuration"));
			ENGINE_TRY_ASSIGN(record.Configuration, ReadConfiguration(configuration));
			ENGINE_TRY_ASSIGN(record.Succeeded, root.ReadMember<bool>("Succeeded"));
			ENGINE_TRY_ASSIGN(const JsonReader sourceHash, root.GetMember("SourceHash"));
			ENGINE_TRY_ASSIGN(record.SourceHash, ReadSourceHash(sourceHash, !record.Succeeded));
			ENGINE_TRY_ASSIGN(const JsonReader sources, root.GetMember("Sources"));
			ENGINE_TRY_ASSIGN(const size_t sourceCount, sources.GetArraySize());
			for (size_t index = 0; index < sourceCount; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader source, sources.GetElement(index));
				ENGINE_TRY_ASSIGN(std::string path, source.ReadString());
				record.Sources.push_back(std::move(path));
			}
			ENGINE_TRY_ASSIGN(const JsonReader output, root.GetMember("Output"));
			ENGINE_TRY_ASSIGN(record.Output, ReadPlainFileName(output, !record.Succeeded));
			ENGINE_TRY_ASSIGN(const JsonReader log, root.GetMember("Log"));
			ENGINE_TRY_ASSIGN(record.Log, ReadPlainFileName(log, false));
			ENGINE_TRY_ASSIGN(const JsonReader diagnostics, root.GetMember("Diagnostics"));
			ENGINE_TRY_ASSIGN(const size_t diagnosticCount, diagnostics.GetArraySize());
			for (size_t index = 0; index < diagnosticCount; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader entry, diagnostics.GetElement(index));
				ENGINE_TRY_ASSIGN(NativeBuildDiagnostic diagnostic, ReadDiagnostic(entry));
				record.Diagnostics.push_back(std::move(diagnostic));
			}
			return record;
		}

		[[nodiscard]] static std::string_view GetBuildKindArgument(NativeBuildKind kind)
		{
			return kind == NativeBuildKind::Module ? "module" : "runtime";
		}

		[[nodiscard]] static std::string_view GetBuildKindName(NativeBuildKind kind)
		{
			return kind == NativeBuildKind::Module ? "Module" : "Runtime";
		}

		// The end of what the build printed, for an error that quotes it.
		[[nodiscard]] static std::string TailOfBuildOutput(const ProcessResult& result)
		{
			const std::string& text = result.StandardError.empty() ? result.StandardOutput : result.StandardError;
			return text.size() <= NativeBuildOutputTail ? text : text.substr(text.size() - NativeBuildOutputTail);
		}

		// One run of Scripts/BuildNative.py (the file comment).
		class ProcessNativeBuildJob final : public INativeBuildJob
		{
		public:
			ProcessNativeBuildJob(NativeBuildRequest request, Process process)
				: m_Request(std::move(request)), m_Process(std::move(process)), m_Started(std::chrono::steady_clock::now())
			{
			}

			[[nodiscard]] std::optional<Result<NativeBuildOutcome>> Poll() override
			{
				if (m_Outcome.has_value())
					return m_Outcome;
				if (!m_Process.has_value() || !m_Process->HasExited())
					return std::nullopt;
				// The process exited; its output readers end once its last handle closes, which a lingering MSBuild node can
				// delay: poll again next frame until they do.
				Result<ProcessResult> result = m_Process->Wait(std::chrono::milliseconds(0));
				if (!result.has_value() && result.error().GetCode() == ErrorCode::Timeout)
					return std::nullopt;
				m_Process.reset();
				m_Outcome = result.has_value() ? Finish(*result) : Result<NativeBuildOutcome>(std::unexpected(std::move(result).error()));
				return m_Outcome;
			}

			void Cancel() override
			{
				if (m_Outcome.has_value() || !m_Process.has_value())
					return;
				if (Status killed = m_Process->Kill(); !killed.has_value())
					ENGINE_WARN("Cannot stop the native build of '{}': {}", FileSystem::PathToUtf8(m_Request.ProjectRoot), killed.error());
				static_cast<void>(m_Process->Wait(NativeBuildDrainTimeout));
				m_Process.reset();
				m_Outcome = Result<NativeBuildOutcome>(MakeError(ErrorCode::Cancelled, "the native build of '{}' was cancelled", FileSystem::PathToUtf8(m_Request.ProjectRoot)));
			}
		private:
			// The outcome the record of an exited build gives; an error when there is no readable record that matches the
			// request.
			[[nodiscard]] Result<NativeBuildOutcome> Finish(const ProcessResult& result) const
			{
				const std::filesystem::path recordPath = m_Request.OutputDirectory / FileSystem::PathFromUtf8(NativeBuildRecordFileName);
				if (!FileSystem::Exists(recordPath))
				{
					return std::unexpected(Error(ErrorCode::Io,
						std::format("the native build of '{}' exited with code {} without a build record: {}", FileSystem::PathToUtf8(m_Request.ProjectRoot),
							result.ExitCode, TailOfBuildOutput(result)))
							.WithHint(std::format("run python {} --help: the editor runs it with the interpreter python Scripts/Generate.py recorded", NativeBuildScript)));
				}
				ENGINE_TRY_ASSIGN(const std::string text, FileSystem::ReadText(recordPath));
				Result<NativeBuildRecord> record = ParseNativeBuildRecord(text);
				if (!record.has_value())
					return std::unexpected(std::move(record).error().WithContext(std::format("while reading '{}'", FileSystem::PathToUtf8(recordPath))));
				if (record->Kind != m_Request.Kind || record->Configuration != m_Request.Configuration || record->Succeeded != (result.ExitCode == 0))
				{
					return MakeError(ErrorCode::Validation, "'{}' records a {} {} build that {}, but the build ran as a {} {} build that exited with code {}",
						FileSystem::PathToUtf8(recordPath), ExportConfigurationToString(record->Configuration), GetBuildKindName(record->Kind),
						record->Succeeded ? "succeeded" : "failed", ExportConfigurationToString(m_Request.Configuration), GetBuildKindName(m_Request.Kind),
						result.ExitCode);
				}
				NativeBuildOutcome outcome;
				outcome.Succeeded = record->Succeeded;
				if (record->Succeeded)
					outcome.Output = m_Request.OutputDirectory / FileSystem::PathFromUtf8(record->Output);
				outcome.SourceHash = record->SourceHash;
				outcome.Diagnostics = std::move(record->Diagnostics);
				outcome.Log = m_Request.OutputDirectory / FileSystem::PathFromUtf8(record->Log);
				outcome.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_Started).count();
				if (outcome.Succeeded && !FileSystem::Exists(outcome.Output))
					return MakeError(ErrorCode::Io, "the native build of '{}' records '{}', which it did not write", FileSystem::PathToUtf8(m_Request.ProjectRoot),
						FileSystem::PathToUtf8(outcome.Output));
				return outcome;
			}
		private:
			NativeBuildRequest m_Request;
			std::optional<Process> m_Process;
			std::chrono::steady_clock::time_point m_Started;
			std::optional<Result<NativeBuildOutcome>> m_Outcome;
		};

		// The platform's backend (CreateNativeBuildBackend).
		class ProcessNativeBuildBackend final : public INativeBuildBackend
		{
		public:
			[[nodiscard]] Result<Scope<INativeBuildJob>> Start(const NativeBuildRequest& request) override
			{
				ENGINE_TRY(SharedLibrary::CheckSupported());
				if (!request.ProjectRoot.is_absolute() || !request.OutputDirectory.is_absolute())
				{
					return MakeError(ErrorCode::InvalidArgument, "a native build needs an absolute project root and output directory, not '{}' and '{}'",
						FileSystem::PathToUtf8(request.ProjectRoot), FileSystem::PathToUtf8(request.OutputDirectory));
				}
				const std::string_view python = ENGINE_PYTHON_EXECUTABLE;
				if (python.empty())
				{
					return std::unexpected(Error(ErrorCode::InvalidState, "the editor's workspace was generated without a Python interpreter, which runs the native build")
							.WithHint("run python Scripts/Generate.py, then build the editor again"));
				}
				const std::filesystem::path repository = FileSystem::PathFromUtf8(ENGINE_REPO_ROOT);
				std::vector<std::string> arguments = {
					FileSystem::PathToUtf8(repository / FileSystem::PathFromUtf8(NativeBuildScript)),
					"--project",
					FileSystem::PathToUtf8(request.ProjectRoot),
					"--kind",
					std::string(GetBuildKindArgument(request.Kind)),
					"--config",
					std::string(ExportConfigurationToString(request.Configuration)),
					"--out",
					FileSystem::PathToUtf8(request.OutputDirectory),
					"--json",
				};
				ENGINE_TRY_ASSIGN(Process process, Process::Spawn({
													   .Executable = FileSystem::PathFromUtf8(python),
													   .Arguments = std::move(arguments),
													   .WorkingDirectory = repository,
													   .Environment = {},
												   }));
				ENGINE_INFO("Native build of '{}' started: {} {} into '{}'", FileSystem::PathToUtf8(request.ProjectRoot), ExportConfigurationToString(request.Configuration),
					GetBuildKindName(request.Kind), FileSystem::PathToUtf8(request.OutputDirectory));
				return CreateScope<ProcessNativeBuildJob>(request, std::move(process));
			}
		};

		[[nodiscard]] static Status AppendHashedFile(XXH64Hasher& hasher, std::string_view name, const std::filesystem::path& path)
		{
			ENGINE_TRY_ASSIGN(const Buffer bytes, FileSystem::ReadFile(path));
			hasher.Update(name);
			hasher.Update("\n");
			hasher.UpdateU64(bytes.size());
			hasher.Update(std::span<const std::byte>(bytes));
			return {};
		}

	}

	Scope<INativeBuildBackend> CreateNativeBuildBackend()
	{
		return CreateScope<Utils::ProcessNativeBuildBackend>();
	}

	bool HasNativeModuleDirectory(const std::filesystem::path& projectRoot)
	{
		std::error_code error;
		return std::filesystem::is_directory(projectRoot / FileSystem::PathFromUtf8(NativeSourceDirectoryName), error) && !error;
	}

	Result<NativeBuildRecord> ParseNativeBuildRecord(std::string_view text)
	{
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		return Utils::ReadRecord(document);
	}

	Result<std::vector<std::string>> ListNativeSources(const std::filesystem::path& projectRoot)
	{
		std::vector<std::string> sources;
		if (!HasNativeModuleDirectory(projectRoot))
			return sources;
		const std::filesystem::path native = projectRoot / FileSystem::PathFromUtf8(NativeSourceDirectoryName);
		ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> entries, FileSystem::ListDirectory(native, true));
		for (const std::filesystem::path& entry : entries)
		{
			std::error_code error;
			const bool regular = std::filesystem::is_regular_file(entry, error);
			if (error)
				return MakeError(ErrorCode::Io, "cannot read '{}': {}", FileSystem::PathToUtf8(entry), error.message());
			const std::string name = FileSystem::PathToUtf8(entry.filename());
			if (regular && (name.ends_with(".cpp") || name.ends_with(".h")))
				sources.push_back(FileSystem::PathToUtf8(entry.lexically_relative(projectRoot)));
		}
		// ListDirectory sorts by the generic UTF-8 form below Native/; the relative paths keep that order, but the rule is
		// stated on them (byte order, as Scripts/Lib/native_build.py sorts), so it is applied to them.
		std::ranges::sort(sources);
		return sources;
	}

	Result<std::string> ComputeNativeSourceHash(const std::filesystem::path& projectRoot, ExportConfiguration configuration,
		const std::filesystem::path& apiHeader)
	{
		if (!FileSystem::Exists(apiHeader))
			return MakeError(ErrorCode::NotFound, "the native module interface '{}' does not exist", FileSystem::PathToUtf8(apiHeader));
		ENGINE_TRY_ASSIGN(const std::vector<std::string> sources, ListNativeSources(projectRoot));
		XXH64Hasher hasher(0);
		hasher.Update(std::format("{}\n{}\n", Utils::NativeSourceHashPrefix, ExportConfigurationToString(configuration)));
		ENGINE_TRY(Utils::AppendHashedFile(hasher, Utils::NativeApiHashName, apiHeader));
		for (const std::string& source : sources)
			ENGINE_TRY(Utils::AppendHashedFile(hasher, source, projectRoot / FileSystem::PathFromUtf8(source)));
		return std::format("{:016x}", hasher.Digest());
	}

}
