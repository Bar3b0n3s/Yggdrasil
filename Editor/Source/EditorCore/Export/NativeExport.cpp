#include "EditorPCH.h"
#include "EditorCore/Export/NativeExport.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Native/NativeModuleHost.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/SharedLibrary.h"

#include <format>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Utils {

		// How an issue names where a diagnostic is: "Native/Spinner.cpp:3:5", or the file alone, or nothing.
		[[nodiscard]] static std::string DescribeNativeLocation(const NativeBuildDiagnostic& diagnostic)
		{
			if (diagnostic.File.empty())
				return {};
			if (diagnostic.Line == 0)
				return diagnostic.File;
			return diagnostic.Column == 0 ? std::format("{}:{}", diagnostic.File, diagnostic.Line) : std::format("{}:{}:{}", diagnostic.File, diagnostic.Line, diagnostic.Column);
		}

		[[nodiscard]] static std::string DescribeNativeDiagnostic(const NativeBuildDiagnostic& diagnostic)
		{
			const std::string location = DescribeNativeLocation(diagnostic);
			return location.empty() ? std::format("{}: {}", diagnostic.Code, diagnostic.Message) : std::format("{}: {}: {}", location, diagnostic.Code, diagnostic.Message);
		}

		// Below the project: what an export keeps of its native builds (GetNativeExportLogPath, GetNativeExportSymbolsDirectory).
		constexpr std::string_view NativeExportLogsDirectory = "Library/Native/ExportLogs";
		constexpr std::string_view NativeExportSymbolsDirectory = "Library/Native/Symbols";

		// Copies `source` to `target` through a temporary file, replacing it.
		[[nodiscard]] static Status CopyNativeExportFile(const std::filesystem::path& source, const std::filesystem::path& target)
		{
			ENGINE_TRY_ASSIGN(const Buffer bytes, FileSystem::ReadFile(source));
			return FileSystem::WriteFileAtomic(target, bytes, { .KeepBackup = false });
		}

	}

	Status CheckNativeExport(const EditorContext& editor)
	{
		const std::filesystem::path& root = editor.GetProject().GetRoot();
		const std::string project = FileSystem::PathToUtf8(root);
		ENGINE_TRY(SharedLibrary::CheckSupported());
		if (!editor.GetNativeModule().IsAllowed())
		{
			return std::unexpected(Error(ErrorCode::PermissionDenied,
				std::format("'{}' has a native module (Native/), and its export builds that code and, with smokeTest or featureTest, runs it, but the user's "
							"Allow native code preference is off",
					project))
					.WithHint("the user turns on Allow native code in the editor's Native panel; no automation method can"));
		}
		ENGINE_TRY_ASSIGN(const std::vector<std::string> sources, ListNativeSources(root));
		if (sources.empty())
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("'{}' has a Native/ folder without a .cpp or .h source, so its module does not build", project))
					.WithHint("write the module's sources with native.write, one of them naming ENGINE_NATIVE_MODULE, or remove the empty Native/ folder"));
		}
		return {};
	}

	Result<Scope<INativeBuildJob>> StartNativeRuntimeBuild(EditorContext& editor, ExportConfiguration configuration, const std::filesystem::path& outputDirectory)
	{
		return editor.GetNativeModule().StartRuntimeBuild(editor, configuration, outputDirectory);
	}

	Error MakeNativeBuildError(const NativeBuildOutcome& outcome, std::string_view name)
	{
		const auto first = std::ranges::find_if(outcome.Diagnostics, [](const NativeBuildDiagnostic& diagnostic)
		{
			return diagnostic.Severity == DiagnosticSeverity::Error;
		});
		std::string firstText = "the build reported no diagnostic";
		if (first != outcome.Diagnostics.end())
			firstText = Utils::DescribeNativeDiagnostic(*first);
		else if (!outcome.Diagnostics.empty())
			firstText = Utils::DescribeNativeDiagnostic(outcome.Diagnostics.front());
		std::vector<ErrorIssue> issues;
		issues.reserve(outcome.Diagnostics.size());
		for (const NativeBuildDiagnostic& diagnostic : outcome.Diagnostics)
		{
			ErrorIssue issue;
			issue.Message = Utils::DescribeNativeDiagnostic(diagnostic);
			issues.push_back(std::move(issue));
		}
		ErrorLocation location;
		if (first != outcome.Diagnostics.end())
		{
			location.File = first->File;
			location.Line = first->Line;
			location.Column = first->Column;
		}
		const std::string log = outcome.Log.empty() ? std::string("its log") : std::format("'{}'", FileSystem::PathToUtf8(outcome.Log));
		return Error(ErrorCode::CompileFailed, std::format("the native module of '{}' does not build: {}", name, firstText))
			.WithLocation(std::move(location))
			.WithHint(std::format("fix the module's sources; native.build reports the same diagnostics in the editor's configuration, and {} holds the "
								  "build's whole output",
				log))
			.WithIssues(std::move(issues));
	}

	std::filesystem::path GetNativeExportLogPath(const std::filesystem::path& projectRoot, ExportConfiguration configuration)
	{
		return projectRoot / FileSystem::PathFromUtf8(Utils::NativeExportLogsDirectory) / FileSystem::PathFromUtf8(std::format("{}.log", ExportConfigurationToString(configuration)));
	}

	std::filesystem::path GetNativeExportSymbolsDirectory(const std::filesystem::path& projectRoot, ExportConfiguration configuration)
	{
		return projectRoot / FileSystem::PathFromUtf8(Utils::NativeExportSymbolsDirectory) / FileSystem::PathFromUtf8(ExportConfigurationToString(configuration));
	}

	NativeBuildOutcome KeepNativeBuildLog(const std::filesystem::path& projectRoot, ExportConfiguration configuration, NativeBuildOutcome outcome)
	{
		if (outcome.Log.empty() || !FileSystem::Exists(outcome.Log))
			return outcome;
		const std::filesystem::path kept = GetNativeExportLogPath(projectRoot, configuration);
		Status copied = FileSystem::CreateDirectories(kept.parent_path());
		if (copied.has_value())
			copied = Utils::CopyNativeExportFile(outcome.Log, kept);
		if (!copied.has_value())
		{
			ENGINE_WARN("Cannot keep the log '{}' of the export's failed native build: {}", FileSystem::PathToUtf8(outcome.Log), copied.error());
			return outcome;
		}
		outcome.Log = kept;
		return outcome;
	}

	Result<std::filesystem::path> ArchiveNativeRuntimeSymbols(const std::filesystem::path& projectRoot, ExportConfiguration configuration,
		const std::filesystem::path& executable)
	{
		std::vector<std::filesystem::path> files;
		for (const std::string_view extension : { ".pdb", ".map" })
		{
			std::filesystem::path file = executable;
			file.replace_extension(FileSystem::PathFromUtf8(extension));
			if (!FileSystem::Exists(file))
				return MakeError(ErrorCode::NotFound, "the linker wrote no '{}' beside '{}'", FileSystem::PathToUtf8(file.filename()), FileSystem::PathToUtf8(executable));
			files.push_back(std::move(file));
		}
		const std::filesystem::path directory = GetNativeExportSymbolsDirectory(projectRoot, configuration);
		if (FileSystem::Exists(directory))
			ENGINE_TRY(FileSystem::Remove(directory));
		ENGINE_TRY(FileSystem::CreateDirectories(directory));
		for (const std::filesystem::path& file : files)
			ENGINE_TRY(Utils::CopyNativeExportFile(file, directory / file.filename()));
		return directory;
	}

}
