#include "EditorPCH.h"
#include "EditorCore/Automation/ExportMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Export/Private/ExportPaths.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <format>
#include <utility>

namespace Engine {

	namespace {

		// project.export's report as the method returns it (§14.2 step 8).
		[[nodiscard]] ProjectExportResult MakeExportResult(const ExportReport& report)
		{
			ProjectExportResult result;
			result.OutputDirectory = FileSystem::PathToUtf8(report.OutputDirectory);
			result.Executable = FileSystem::PathToUtf8(report.Executable);
			result.Config = report.Configuration;
			result.Files.reserve(report.Files.size());
			for (const ExportedFile& file : report.Files)
			{
				result.Files.push_back({
					.Path = file.Path,
					.Size = ToAutomationCounter(file.Size),
					.Hash = std::format("{:016x}", file.Hash),
				});
			}
			result.GameAssetCount = report.GameAssetCount;
			result.EngineEntryCount = report.EngineEntryCount;
			result.Warnings = report.Warnings;
			result.SmokeTestRan = report.SmokeTestRan;
			result.SmokeTestExitCode = report.SmokeTestExitCode;
			result.Seconds = static_cast<float>(report.Seconds);
			return result;
		}

		// project.export's operation: the exporter, advanced once per frame (§13.2 "Pending operations"); a disconnect
		// cancels it, which removes the staging directory and leaves an earlier export untouched.
		class ProjectExportOperation final : public PendingOperation
		{
		public:
			explicit ProjectExportOperation(Scope<Exporter> exporter)
				: m_Exporter(std::move(exporter))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				std::optional<Result<ExportReport>> outcome = m_Exporter->Poll();
				if (!outcome.has_value())
					return std::nullopt;
				if (!outcome->has_value())
					return Result<Json>(std::unexpected(std::move(*outcome).error()));
				return context.SerializeResult(MakeExportResult(**outcome));
			}

			void Cancel(MethodContext& /*context*/) override
			{
				m_Exporter->Cancel();
			}

			[[nodiscard]] std::string GetPhase() const override
			{
				return std::format("Export:{}", ExportPhaseToString(m_Exporter->GetPhase()));
			}
		private:
			Scope<Exporter> m_Exporter;
		};

	}

	namespace Automation {

		Result<Scope<PendingOperation>> ProjectExport(EditorMethodContext& context, const ProjectExportParams& params)
		{
			if (context.HasParam("testing"))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::Unsupported, "/testing", "testing exports arrive with M15",
					"leave testing out: M7 exports games without the test harness"));
			}
			if (Status directory = Utils::CheckExportOutputDirectory(params.OutDir); !directory.has_value())
			{
				return std::unexpected(
					Utils::MakeParamError(ErrorCode::InvalidArgument, "/outDir", directory.error().GetMessageText(), directory.error().GetHint()));
			}
			const std::filesystem::path& binaryRoot = context.GetServer().GetSpecification().ExportBinaryRoot;
			if (binaryRoot.empty())
				return MakeError(ErrorCode::Unsupported, "this editor exports nothing: it was started without the directory of the build outputs");

			// The smoke test runs ExportSpecification's default of 300 frames (§14.2 step 7).
			ExportSpecification specification;
			specification.Configuration = params.Config;
			specification.OutputDirectory = params.OutDir;
			specification.BinaryRoot = binaryRoot;
			specification.SmokeTest = params.SmokeTest;
			ENGINE_TRY_ASSIGN(Scope<Exporter> exporter, Exporter::Start(context.GetEditor(), specification));
			return Scope<PendingOperation>(CreateScope<ProjectExportOperation>(std::move(exporter)));
		}

	}

	void RegisterExportMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<ExportConfiguration>("ExportConfiguration", "The build configuration an export targets (§2.2).")
			.Entry(ExportConfiguration::Debug, "Debug", "Engine asserts on, unoptimized, with the automation server.")
			.Entry(ExportConfiguration::Release, "Release", "Engine asserts on, optimized, with the automation server.")
			.Entry(ExportConfiguration::Dist, "Dist", "The shipping configuration: asserts off, no automation server.");

		registry.Struct<ProjectExportParams>("ProjectExportParams", "The params of project.export.")
			.Field("config", &ProjectExportParams::Config, "The build configuration whose Runtime and shaders the export packages.")
			.Field("outDir", &ProjectExportParams::OutDir,
				"The project-relative output directory below Build/; empty or absent: Build/<Platform>-<Config>/<Name>. An earlier export there "
				"is replaced once the new one is complete.")
			.Field("smokeTest", &ProjectExportParams::SmokeTest,
				"Run the exported game headless for 300 frames with --expect-no-errors before it replaces the output directory, and require exit "
				"code 0.")
			.Field("testing", &ProjectExportParams::Testing, "A testing export (M15): refused while present.");

		registry.Struct<ExportedFileSummary>("ExportedFileSummary", "One file of an exported game.")
			.Field("path", &ExportedFileSummary::Path, "The path relative to the output directory, with forward slashes.")
			.Field("size", &ExportedFileSummary::Size, "The size in bytes.", { .Unit = "bytes" })
			.Field("hash", &ExportedFileSummary::Hash, "The XXH64 of the bytes, as 16 lowercase hex digits.");

		registry.Struct<ProjectExportResult>("ProjectExportResult", "What project.export produced (§14.2).")
			.Field("outputDirectory", &ProjectExportResult::OutputDirectory, "The absolute path of the exported game's folder.")
			.Field("executable", &ProjectExportResult::Executable, "The absolute path of its executable, named after the project.")
			.Field("config", &ProjectExportResult::Config, "The configuration exported.")
			.Field("files", &ProjectExportResult::Files, "Every file of the package, sorted by path.")
			.Field("gameAssetCount", &ProjectExportResult::GameAssetCount, "The cooked assets in Data/Game.pak.")
			.Field("engineEntryCount", &ProjectExportResult::EngineEntryCount, "The entries of Data/Engine.pak: shader files and built-ins.")
			.Field("warnings", &ProjectExportResult::Warnings, "What the export left out or could not check.")
			.Field("smokeTestRan", &ProjectExportResult::SmokeTestRan,
				"Whether the smoke test ran: false without smokeTest, and for a Dist export from an editor without a graphics device.")
			.Field("smokeTestExitCode", &ProjectExportResult::SmokeTestExitCode, "The smoke test's exit code; 0 when it did not run.")
			.Field("seconds", &ProjectExportResult::Seconds, "How long the export took (wall-clock time, for information).", { .Unit = "s" });
	}

	void RegisterExportMethods(MethodRegistry& methods)
	{
		Json example = Json::object();
		example["config"] = "Release";
		example["smokeTest"] = true;
		methods.AddPending<EditorMethodContext, ProjectExportParams, ProjectExportResult>(
			{
				.Name = "project.export",
				.Description = "Exports the open project as a standalone game below Build/: the configuration's Runtime renamed after the project, "
							   "the app-local CRT, Game.json, Data/Engine.pak and Data/Game.pak, optionally smoke-tested; resolves with the report.",
				.RequiredParams = { "config" },
				.ExposeAsTool = true,
				.Mutates = true,
				.TimeoutSeconds = 900,
				.Examples = { { .Description = "Export a Release build and smoke-test it.", .Params = example } },
			},
			&Automation::ProjectExport);
	}

}
