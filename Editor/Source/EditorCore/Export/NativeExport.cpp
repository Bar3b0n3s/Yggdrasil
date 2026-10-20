#include "EditorPCH.h"
#include "EditorCore/Export/NativeExport.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Native/NativeModuleHost.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Platform/SharedLibrary.h"

#include <format>

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

}
