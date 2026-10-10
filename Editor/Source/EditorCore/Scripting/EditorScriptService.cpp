#include "EditorPCH.h"
#include "EditorCore/Scripting/EditorScriptService.h"

#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Importers/ScriptImporter.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <algorithm>
#include <format>
#include <optional>
#include <tuple>

namespace Engine {

	namespace {

		Status ValidateScriptPath(const EditorContext& editor, const VfsPath& path)
		{
			if (!editor.HasProject())
				return MakeError(ErrorCode::InvalidState, "no project is open");
			if (path.GetScheme() != "project" || !path.GetPath().starts_with("Assets/") || path.GetExtension() != ".luau")
				return std::unexpected(Error(ErrorCode::InvalidArgument, "scripts must be .luau files below project://Assets").WithLocation({ .File = path.ToString(), .JsonPointer = std::nullopt, .Entity = {} }));
			return {};
		}

		void SortScriptDiagnostics(std::vector<ScriptDiagnostic>& diagnostics)
		{
			std::ranges::sort(diagnostics, [](const ScriptDiagnostic& left, const ScriptDiagnostic& right)
			{
				return std::tie(left.File, left.Line, left.Column, left.Code, left.Message, left.EndLine, left.EndColumn, left.Severity)
					< std::tie(right.File, right.Line, right.Column, right.Code, right.Message, right.EndLine, right.EndColumn, right.Severity);
			});
			diagnostics.erase(std::unique(diagnostics.begin(), diagnostics.end()), diagnostics.end());
		}

		class ScriptCheckReader final : public IScriptModuleReader
		{
		public:
			explicit ScriptCheckReader(const EditorScriptService& service)
				: m_Service(service)
			{
			}
			Result<std::string> ReadModule(const VfsPath& path) override
			{
				auto source = m_Service.Read(path);
				if (!source && !m_Failure.has_value())
					m_Failure = source.error();
				return source;
			}
			[[nodiscard]] const std::optional<Error>& GetFailure() const { return m_Failure; }
		private:
			const EditorScriptService& m_Service; // Borrowed by one synchronous Check invocation.
			std::optional<Error> m_Failure{};
		};

		Result<std::vector<ScriptDiagnostic>> CheckWrittenScript(EditorContext& editor, IScriptDiagnosticsProvider& diagnostics,
			const VfsPath& path, std::string_view source, const AssetMetadata& metadata)
		{
			std::vector<ImportAssetLookupEntry> assets;
			for (const AssetRecord* record : editor.GetAssets().GetRegistry().GetRecords())
				assets.push_back({ .SourcePath = record->SourcePath, .Handle = record->Metadata.Handle, .Kind = record->Metadata.Kind, .Type = record->Metadata.Type, .Owner = record->Metadata.Owner });
			std::ranges::sort(assets, {}, &ImportAssetLookupEntry::SourcePath);
			// Detached real import: checks current bytes even while lockstep defers publication. AssetWriter alone
			// schedules the manager's normal import/publication; this validation never replaces a live artifact.
			ImportContext context({ .Vfs = &editor.GetVfs(), .SourcePath = path, .SourceBytes = AsBytes(source), .Registry = &editor.GetTypeRegistry(), .Assets = assets, .ScriptDiagnostics = &diagnostics });
			const Result<ImportResult> imported = ScriptImporter{}.Import(context, metadata);
			std::vector<ScriptDiagnostic> findings;
			if (const auto check = context.GetScriptCheck(); check.has_value())
				findings = check->Diagnostics;
			if (!imported)
			{
				const Error& error = imported.error();
				if (error.GetCode() == ErrorCode::Io || error.GetCode() == ErrorCode::PermissionDenied || error.GetCode() == ErrorCode::InvalidState
					|| error.GetCode() == ErrorCode::Unsupported)
					return std::unexpected(error);
				findings.push_back({ .Severity = DiagnosticSeverity::Error,
					.Code = error.GetCode() == ErrorCode::CompileFailed ? "SCRIPT_COMPILE_ERROR" : "SCRIPT_IMPORT_ERROR",
					.File = error.GetLocation().File.empty() ? std::string(path.GetPath()) : error.GetLocation().File,
					.Line = error.GetLocation().Line,
					.Column = error.GetLocation().Column,
					.Message = error.GetMessageText() });
			}
			SortScriptDiagnostics(findings);
			return findings;
		}

	}

	EditorScriptService::EditorScriptService(EditorContext& editor, IScriptDiagnosticsProvider& diagnostics)
		: m_Editor(&editor), m_Diagnostics(&diagnostics)
	{
	}

	Result<EditorScriptWriteResult> EditorScriptService::Create(const VfsPath& path, ScriptTemplate scriptTemplate)
	{
		ENGINE_TRY(ValidateScriptPath(*m_Editor, path));
		if (m_Editor->IsReadOnly() && !m_Editor->IsDryRun())
			return MakeError(ErrorCode::PermissionDenied, "the project is read-only");
		const auto existing = m_Editor->GetVfs().GetInfo(path);
		if (existing)
			return MakeError(ErrorCode::AlreadyExists, "'{}' already exists", path.ToString());
		if (existing.error().GetCode() != ErrorCode::NotFound)
			return std::unexpected(existing.error());
		std::string_view templateName;
		switch (scriptTemplate)
		{
			case ScriptTemplate::Behaviour: templateName = "Behaviour.luau"; break;
			case ScriptTemplate::Module:    templateName = "Module.luau"; break;
			case ScriptTemplate::Test:      templateName = "Test.luau"; break;
		}
		if (templateName.empty())
			return MakeError(ErrorCode::InvalidArgument, "unknown script template");
		ENGINE_TRY_ASSIGN(const VfsPath templatePath, VfsPath::Create("engine", std::format("Templates/Scripts/{}", templateName)));
		ENGINE_TRY_ASSIGN(std::string source, m_Editor->GetVfs().ReadText(templatePath));
		std::string name = "Script_";
		for (const char character : path.GetStem())
			name.push_back((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
						|| (character >= '0' && character <= '9') || character == '_'
					? character
					: '_');
		constexpr std::string_view Placeholder = "{{Name}}";
		for (size_t position = source.find(Placeholder); position != std::string::npos; position = source.find(Placeholder, position + name.size()))
			source.replace(position, Placeholder.size(), name);
		return Write(path, source);
	}

	Result<std::string> EditorScriptService::Read(const VfsPath& path) const
	{
		ENGINE_TRY(ValidateScriptPath(*m_Editor, path));
		return m_Editor->GetVfs().ReadText(path);
	}

	Result<EditorScriptWriteResult> EditorScriptService::Write(const VfsPath& path, std::string_view source)
	{
		ENGINE_TRY(ValidateScriptPath(*m_Editor, path));
		if (m_Editor->IsReadOnly() && !m_Editor->IsDryRun())
			return MakeError(ErrorCode::PermissionDenied, "the project is read-only");
		if (!IsValidUtf8(source))
			return MakeError(ErrorCode::InvalidArgument, "script source is not valid UTF-8");
		VirtualFileSystem& vfs = m_Editor->GetVfs();
		EditorAssetManager& assets = m_Editor->GetAssets();
		assets.WaitIdle();
		std::vector<AssetFileEdit> edits;
		std::vector<VfsPath> missingDirectories;
		for (VfsPath directory = path.GetParent(); !directory.IsRoot(); directory = directory.GetParent())
		{
			const auto info = vfs.GetInfo(directory);
			if (info)
			{
				if (!info->IsDirectory)
					return MakeError(ErrorCode::AlreadyExists, "'{}' is a file", directory.ToString());
				break;
			}
			if (info.error().GetCode() != ErrorCode::NotFound)
				return std::unexpected(info.error());
			missingDirectories.push_back(directory);
		}
		for (auto directory = missingDirectories.rbegin(); directory != missingDirectories.rend(); ++directory)
			edits.push_back({ .Path = *directory, .Kind = AssetFileKind::Directory, .Before = std::nullopt, .After = Buffer{} });
		const auto previous = vfs.ReadFile(path);
		if (!previous && previous.error().GetCode() != ErrorCode::NotFound)
			return std::unexpected(previous.error());
		if (!previous || AsStringView(*previous) != source)
			edits.push_back({ .Path = path, .Before = previous ? std::optional<Buffer>(*previous) : std::nullopt, .After = Buffer(AsBytes(source).begin(), AsBytes(source).end()) });
		ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(path));
		AssetMetadata metadata;
		const auto oldMeta = vfs.ReadText(metaPath);
		if (oldMeta)
		{
			ENGINE_TRY_ASSIGN(metadata, ParseAssetMetadata(*oldMeta, metaPath.ToString()));
			if (metadata.Kind != AssetMetaKind::Asset || metadata.Type != AssetType::Script || metadata.Importer != ScriptImporter::Id)
				return MakeError(ErrorCode::InvalidArgument, "'{}' is not a standalone Script meta", metaPath.ToString());
		}
		else
		{
			if (oldMeta.error().GetCode() != ErrorCode::NotFound)
				return std::unexpected(oldMeta.error());
			ENGINE_TRY_ASSIGN(metadata, assets.CreateMetadata(path));
			const std::string text = SerializeAssetMetadata(metadata);
			edits.push_back({ .Path = metaPath, .Before = std::nullopt, .After = Buffer(AsBytes(text).begin(), AsBytes(text).end()) });
		}
		const std::string label = std::format("Write '{}'", path.GetPath());
		EditorTransaction transaction(*m_Editor, label);
		if (!edits.empty())
		{
			ENGINE_TRY_ASSIGN(const uint64_t index, m_Editor->Execute(CreateScope<AssetEditCommand>(label, std::move(edits))));
			static_cast<void>(index);
		}
		ENGINE_TRY_ASSIGN(std::vector<ScriptDiagnostic> findings, CheckWrittenScript(*m_Editor, *m_Diagnostics, path, source, metadata));
		return EditorScriptWriteResult{ .Script = metadata.Handle, .Path = std::string(path.GetPath()), .UndoIndex = transaction.Commit(), .Diagnostics = std::move(findings) };
	}

	Result<EditorScriptCheckResult> EditorScriptService::Check(std::span<const VfsPath> paths) const
	{
		if (!m_Editor->HasProject())
			return MakeError(ErrorCode::InvalidState, "no project is open");
		std::vector<VfsPath> selected(paths.begin(), paths.end());
		if (selected.empty())
		{
			ENGINE_TRY_ASSIGN(const VfsPath root, VfsPath::Create("project", "Assets"));
			ENGINE_TRY_ASSIGN(const std::vector<VfsEntry> entries, m_Editor->GetVfs().List(root, true));
			for (const VfsEntry& entry : entries)
				if (!entry.Info.IsDirectory && entry.Path.GetExtension() == ".luau")
					selected.push_back(entry.Path);
		}
		std::ranges::sort(selected);
		selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
		std::vector<std::string> sources;
		for (const VfsPath& path : selected)
		{
			ENGINE_TRY(ValidateScriptPath(*m_Editor, path));
			ENGINE_TRY_ASSIGN(std::string source, Read(path));
			sources.push_back(std::move(source));
		}
		EditorScriptCheckResult result;
		ScriptCheckReader reader(*this);
		for (size_t index = 0; index < selected.size(); ++index)
		{
			result.Paths.emplace_back(selected[index].GetPath());
			std::vector<ScriptDiagnostic> findings = m_Diagnostics->CheckScript({ .Path = selected[index], .Source = sources[index], .Modules = &reader });
			if (reader.GetFailure().has_value())
				return std::unexpected(*reader.GetFailure());
			result.Diagnostics.insert(result.Diagnostics.end(), findings.begin(), findings.end());
		}
		SortScriptDiagnostics(result.Diagnostics);
		return result;
	}

	Result<AssetRef<ScriptData>> EditorScriptService::GetFields(AssetHandle script) const
	{
		if (!m_Editor->HasProject())
			return MakeError(ErrorCode::InvalidState, "no project is open");
		EditorAssetManager& assets = m_Editor->GetAssets();
		if (assets.GetAssetType(script) == AssetType::None)
			return MakeError(ErrorCode::NotFound, "script asset does not exist");
		if (assets.GetAssetType(script) != AssetType::Script)
			return MakeError(ErrorCode::InvalidArgument, "asset is not a Script");
		const auto loaded = assets.Load(script);
		if (!loaded)
			return std::unexpected(Error(ErrorCode::ImportFailed, loaded.error().GetMessageText()).WithLocation(loaded.error().GetLocation()));
		const AssetRef<ScriptData> data = AssetCast<ScriptData>(*loaded);
		if (!data)
			return MakeError(ErrorCode::ImportFailed, "script loader returned the wrong asset type");
		return data;
	}

}
