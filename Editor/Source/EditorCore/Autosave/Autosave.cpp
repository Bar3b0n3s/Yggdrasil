#include "EditorPCH.h"
#include "EditorCore/Autosave/Autosave.h"

#include "EditorCore/Autosave/Private/AutosaveRecoveryData.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Platform/SecureRandom.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace Engine {

	namespace Utils {

		constexpr uint64_t MaxAutosavePayloadBytes = 256ull * 1024 * 1024;
		constexpr uint64_t MaxAutosaveMetadataBytes = 1024 * 1024;
		constexpr uint32_t MaxAutosaveGenerations = 32;
		constexpr uint32_t AutosaveWriterBit = 1;
		constexpr uint32_t AutosaveClosedBit = 2;
		constexpr int32_t NoAutosaveSlot = -1;
		static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<int32_t>::is_always_lock_free);

		struct AutosaveFingerprint
		{
			bool Exists = false;
			uint64_t Hash = 0;
			uint64_t Size = 0;
			uint64_t ModificationTime = 0;
			bool operator==(const AutosaveFingerprint&) const = default;
		};

		struct AutosaveManifestEntry
		{
			std::string Generation{};
			uint64_t Sequence = 0;
		};

		struct AutosaveManifest
		{
			uint64_t Sequence = 0;
			std::vector<AutosaveManifestEntry> Generations{};
		};

		struct AutosaveSnapshot
		{
			std::filesystem::path Root{};
			std::filesystem::path ProjectFile{};
			std::filesystem::path AutosaveRoot{};
			std::filesystem::path Source{};
			std::string ScenePath{};
			std::string SceneName{};
			std::string UntitledToken{};
			std::string Text{};
			AutosaveFingerprint ProjectFingerprint{};
			AutosaveFingerprint SourceFingerprint{};
			uint64_t Revision = 0;
			uint64_t Epoch = 0;
		};

		static Status CheckAutosavePath(const std::filesystem::path& root, const std::filesystem::path& path)
		{
			const std::filesystem::path relative = path.lexically_relative(root);
			if (relative.empty() || relative.is_absolute())
				return MakeError(ErrorCode::Validation, "autosave path '{}' is outside its project", FileSystem::PathToUtf8(path));
			ENGINE_TRY_ASSIGN(const VfsPath checked, VfsPath::Create("project", FileSystem::PathToUtf8(relative)));
			if (checked.GetPath() != FileSystem::PathToUtf8(relative))
				return MakeError(ErrorCode::Validation, "autosave path is not canonical");
			std::filesystem::path current = root;
			for (const std::filesystem::path& part : relative)
			{
				current /= part;
				std::error_code error;
				const std::filesystem::file_status status = std::filesystem::symlink_status(current, error);
				if (error && error != std::errc::no_such_file_or_directory)
					return MakeError(ErrorCode::Io, "cannot inspect autosave path '{}': {}", FileSystem::PathToUtf8(current), error.message());
				if (std::filesystem::is_symlink(status))
					return MakeError(ErrorCode::Validation, "autosave refuses linked path '{}'", FileSystem::PathToUtf8(current));
			}
			std::error_code error;
			const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, error);
			if (error)
				return MakeError(ErrorCode::Io, "cannot resolve autosave path '{}': {}", FileSystem::PathToUtf8(path), error.message());
			if (resolved != path.lexically_normal())
				return MakeError(ErrorCode::Validation, "autosave refuses redirected path '{}'", FileSystem::PathToUtf8(path));
			return {};
		}

		static Result<std::filesystem::path> CanonicalAutosaveProject(const LoadedProject& project)
		{
			std::error_code error;
			const std::filesystem::path root = std::filesystem::canonical(project.GetRoot(), error);
			if (error)
				return MakeError(ErrorCode::Io, "cannot resolve autosave project '{}': {}", FileSystem::PathToUtf8(project.GetProjectFile()), error.message());
			const std::filesystem::path projectFile = root / project.GetProjectFile().filename();
			ENGINE_TRY(CheckAutosavePath(root, projectFile));
			return projectFile;
		}

		// The allocation is bounded before opening the stream. A concurrently grown file cannot expand this buffer.
		static Result<std::string> ReadAutosaveBytes(const std::filesystem::path& path, uint64_t limit)
		{
			ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(path));
			if (info.IsDirectory || info.Size > limit)
				return MakeError(ErrorCode::Validation, "autosave file '{}' exceeds its size limit or is a directory", FileSystem::PathToUtf8(path));
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return MakeError(ErrorCode::Io, "cannot read autosave file '{}'", FileSystem::PathToUtf8(path));
			std::string text(static_cast<size_t>(info.Size), '\0');
			stream.read(text.data(), static_cast<std::streamsize>(text.size()));
			if (stream.gcount() != static_cast<std::streamsize>(text.size()))
				return MakeError(ErrorCode::Io, "autosave file '{}' changed or could not be read completely", FileSystem::PathToUtf8(path));
			if (stream.peek() != std::char_traits<char>::eof())
				return MakeError(ErrorCode::Conflict, "autosave file '{}' grew while it was read", FileSystem::PathToUtf8(path));
			if (stream.bad())
				return MakeError(ErrorCode::Io, "autosave file '{}' could not be read completely", FileSystem::PathToUtf8(path));
			return text;
		}

		static Result<std::string> ReadAutosaveText(const std::filesystem::path& path, uint64_t limit)
		{
			ENGINE_TRY_ASSIGN(std::string text, ReadAutosaveBytes(path, limit));
			if (!IsValidUtf8(text))
				return MakeError(ErrorCode::Validation, "autosave file '{}' is not UTF-8", FileSystem::PathToUtf8(path));
			return text;
		}

		static Result<AutosaveFingerprint> ReadAutosaveFingerprint(const std::filesystem::path& path)
		{
			const Result<FileInfo> before = FileSystem::GetInfo(path);
			if (!before)
			{
				if (before.error().GetCode() == ErrorCode::NotFound)
					return AutosaveFingerprint{};
				return std::unexpected(before.error());
			}
			if (before->IsDirectory || before->Size > MaxAutosavePayloadBytes)
				return MakeError(ErrorCode::Validation, "autosave source '{}' is not a bounded file", FileSystem::PathToUtf8(path));
			ENGINE_TRY_ASSIGN(const std::string bytes, ReadAutosaveBytes(path, MaxAutosavePayloadBytes));
			ENGINE_TRY_ASSIGN(const FileInfo after, FileSystem::GetInfo(path));
			if (before->Size != after.Size || before->ModificationTime != after.ModificationTime || bytes.size() != after.Size)
				return MakeError(ErrorCode::Conflict, "autosave source '{}' changed while it was read", FileSystem::PathToUtf8(path));
			return AutosaveFingerprint{ .Exists = true, .Hash = XXH64(bytes), .Size = after.Size, .ModificationTime = after.ModificationTime };
		}

		static Json AutosaveFingerprintJson(const AutosaveFingerprint& fingerprint)
		{
			return Json{ { "Exists", fingerprint.Exists }, { "Hash", fingerprint.Hash }, { "Size", fingerprint.Size },
				{ "ModificationTime", fingerprint.ModificationTime } };
		}

		static Result<AutosaveFingerprint> ParseAutosaveFingerprint(const JsonReader& reader)
		{
			AutosaveFingerprint result;
			ENGINE_TRY_ASSIGN(result.Exists, reader.ReadMember<bool>("Exists"));
			ENGINE_TRY_ASSIGN(result.Hash, reader.ReadMember<uint64_t>("Hash"));
			ENGINE_TRY_ASSIGN(result.Size, reader.ReadMember<uint64_t>("Size"));
			ENGINE_TRY_ASSIGN(result.ModificationTime, reader.ReadMember<uint64_t>("ModificationTime"));
			if (result.Size > MaxAutosavePayloadBytes || (!result.Exists && (result.Hash || result.Size || result.ModificationTime)))
				return MakeError(ErrorCode::Validation, "invalid autosave source fingerprint");
			return result;
		}

		static bool IsAutosaveGeneration(std::string_view generation)
		{
			return generation.size() == 18 && generation.starts_with("g-")
				&& std::all_of(generation.begin() + 2, generation.end(), [](char character)
			{
				return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
			});
		}

		static Result<AutosaveManifest> ReadAutosaveManifest(const std::filesystem::path& root)
		{
			const std::filesystem::path manifestPath = root / "Library" / "Autosave" / "Manifest.json";
			ENGINE_TRY(CheckAutosavePath(root, manifestPath));
			const Result<std::string> text = ReadAutosaveText(manifestPath, MaxAutosaveMetadataBytes);
			if (!text)
			{
				if (text.error().GetCode() == ErrorCode::NotFound)
					return AutosaveManifest{};
				return std::unexpected(text.error());
			}
			ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(*text));
			const JsonReader reader(document);
			ENGINE_TRY(reader.ReadFormatHeader("AutosaveManifest", 1, 1));
			AutosaveManifest result;
			ENGINE_TRY_ASSIGN(result.Sequence, reader.ReadMember<uint64_t>("Sequence"));
			ENGINE_TRY_ASSIGN(const JsonReader entries, reader.GetMember("Generations"));
			ENGINE_TRY_ASSIGN(const size_t count, entries.GetArraySize());
			if (count > MaxAutosaveGenerations)
				return MakeError(ErrorCode::Validation, "autosave manifest exceeds its generation limit");
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader entry, entries.GetElement(index));
				AutosaveManifestEntry parsed;
				ENGINE_TRY_ASSIGN(parsed.Generation, entry.ReadMember<std::string>("Generation"));
				ENGINE_TRY_ASSIGN(parsed.Sequence, entry.ReadMember<uint64_t>("Sequence"));
				if (!IsAutosaveGeneration(parsed.Generation) || parsed.Sequence == 0 || parsed.Sequence > result.Sequence
					|| parsed.Generation != std::format("g-{:016x}", parsed.Sequence))
					return MakeError(ErrorCode::Validation, "invalid autosave generation identity");
				for (const AutosaveManifestEntry& previous : result.Generations)
					if (previous.Sequence == parsed.Sequence)
						return MakeError(ErrorCode::Validation, "duplicate autosave generation identity");
				result.Generations.push_back(std::move(parsed));
			}
			std::sort(result.Generations.begin(), result.Generations.end(), [](const AutosaveManifestEntry& left, const AutosaveManifestEntry& right)
			{
				return left.Sequence > right.Sequence;
			});
			return result;
		}

		static Result<std::string> AutosaveManifestText(const AutosaveManifest& manifest)
		{
			Json entries = Json::array();
			for (const AutosaveManifestEntry& entry : manifest.Generations)
				entries.push_back(Json{ { "Generation", entry.Generation }, { "Sequence", entry.Sequence } });
			return JsonWriter::Write(Json{ { "Format", "AutosaveManifest" }, { "Version", 1 }, { "Sequence", manifest.Sequence },
				{ "Generations", std::move(entries) } });
		}

		static Status ValidateAutosaveSource(const AutosaveSnapshot& snapshot)
		{
			ENGINE_TRY(CheckAutosavePath(snapshot.Root, snapshot.ProjectFile));
			ENGINE_TRY_ASSIGN(const AutosaveFingerprint project, ReadAutosaveFingerprint(snapshot.ProjectFile));
			if (project != snapshot.ProjectFingerprint)
				return MakeError(ErrorCode::Conflict, "the project changed after the autosave snapshot was captured");
			if (!snapshot.ScenePath.empty())
			{
				ENGINE_TRY(CheckAutosavePath(snapshot.Root, snapshot.Source));
				ENGINE_TRY_ASSIGN(const AutosaveFingerprint source, ReadAutosaveFingerprint(snapshot.Source));
				if (source != snapshot.SourceFingerprint)
					return MakeError(ErrorCode::Conflict, "scene '{}' changed after the autosave snapshot was captured", snapshot.ScenePath);
			}
			return {};
		}

		static Result<AutosaveSnapshot> ReadAutosaveGeneration(const std::filesystem::path& projectFile, const AutosaveManifestEntry& entry, bool verifySource = true)
		{
			AutosaveSnapshot snapshot;
			snapshot.Root = projectFile.parent_path();
			snapshot.ProjectFile = projectFile;
			snapshot.AutosaveRoot = snapshot.Root / "Library" / "Autosave";
			const std::filesystem::path directory = snapshot.AutosaveRoot / FileSystem::PathFromUtf8(entry.Generation);
			const std::filesystem::path metadata = directory / "Metadata.json";
			const std::filesystem::path payload = directory / "Scene.json";
			ENGINE_TRY(CheckAutosavePath(snapshot.Root, metadata));
			ENGINE_TRY(CheckAutosavePath(snapshot.Root, payload));
			ENGINE_TRY_ASSIGN(const std::string text, ReadAutosaveText(metadata, MaxAutosaveMetadataBytes));
			ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
			const JsonReader reader(document);
			ENGINE_TRY(reader.ReadFormatHeader("AutosaveGeneration", 1, 1));
			ENGINE_TRY_ASSIGN(const uint64_t sequence, reader.ReadMember<uint64_t>("Sequence"));
			ENGINE_TRY_ASSIGN(const std::string projectIdentity, reader.ReadMember<std::string>("ProjectFile"));
			if (projectIdentity != FileSystem::PathToUtf8(projectFile))
				return MakeError(ErrorCode::Conflict, "autosave belongs to another canonical project path");
			if (sequence != entry.Sequence)
				return MakeError(ErrorCode::Validation, "autosave generation sequence differs from its committed manifest");
			ENGINE_TRY_ASSIGN(const JsonReader projectFingerprint, reader.GetMember("ProjectFingerprint"));
			ENGINE_TRY_ASSIGN(snapshot.ProjectFingerprint, ParseAutosaveFingerprint(projectFingerprint));
			ENGINE_TRY_ASSIGN(const JsonReader sourceFingerprint, reader.GetMember("SourceFingerprint"));
			ENGINE_TRY_ASSIGN(snapshot.SourceFingerprint, ParseAutosaveFingerprint(sourceFingerprint));
			ENGINE_TRY_ASSIGN(snapshot.ScenePath, reader.ReadMember<std::string>("ScenePath"));
			ENGINE_TRY_ASSIGN(snapshot.SceneName, reader.ReadMember<std::string>("SceneName"));
			ENGINE_TRY_ASSIGN(snapshot.UntitledToken, reader.ReadMember<std::string>("UntitledToken"));
			ENGINE_TRY_ASSIGN(snapshot.Revision, reader.ReadMember<uint64_t>("DirtyRevision"));
			ENGINE_TRY_ASSIGN(const bool dirty, reader.ReadMember<bool>("Dirty"));
			ENGINE_TRY_ASSIGN(const uint64_t size, reader.ReadMember<uint64_t>("PayloadSize"));
			ENGINE_TRY_ASSIGN(const uint64_t hash, reader.ReadMember<uint64_t>("PayloadHash"));
			if (!dirty || snapshot.Revision == 0 || !snapshot.ProjectFingerprint.Exists || size > MaxAutosavePayloadBytes)
				return MakeError(ErrorCode::Validation, "autosave lacks a valid captured dirty revision or payload size");
			if (snapshot.ScenePath.empty())
			{
				if (snapshot.UntitledToken.size() != 32 || !std::all_of(snapshot.UntitledToken.begin(), snapshot.UntitledToken.end(), [](char character)
				{
					return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
				}) || snapshot.SourceFingerprint.Exists)
					return MakeError(ErrorCode::Validation, "invalid untitled autosave identity");
			}
			else
			{
				ENGINE_TRY_ASSIGN(const VfsPath source, VfsPath::Create("project", snapshot.ScenePath));
				if (source.GetPath() != snapshot.ScenePath || source.GetExtension() != ".scene"
					|| !snapshot.UntitledToken.empty())
					return MakeError(ErrorCode::Validation, "autosave scene path must be a canonical project-relative .scene path");
				snapshot.Source = snapshot.Root / FileSystem::PathFromUtf8(snapshot.ScenePath);
			}
			ENGINE_TRY_ASSIGN(snapshot.Text, ReadAutosaveText(payload, MaxAutosavePayloadBytes));
			if (snapshot.Text.size() != size || XXH64(snapshot.Text) != hash)
				return MakeError(ErrorCode::Validation, "autosave payload size or checksum differs from its metadata");
			if (verifySource)
				ENGINE_TRY(ValidateAutosaveSource(snapshot));
			return snapshot;
		}

		static AutosaveRecoveryInfo MakeAutosaveRecoveryInfo(const AutosaveSnapshot& snapshot, std::string generation)
		{
			AutosaveRecoveryInfo info;
			info.Generation = std::move(generation);
			info.ScenePath = snapshot.ScenePath;
			info.SceneName = snapshot.SceneName;
			info.SceneRevision = snapshot.Revision;
			if (!snapshot.ScenePath.empty())
				info.Files.push_back(snapshot.ScenePath);
			info.NewerThanSource = !snapshot.SourceFingerprint.Exists || snapshot.SourceFingerprint.Size != snapshot.Text.size()
				|| snapshot.SourceFingerprint.Hash != XXH64(snapshot.Text);
			return info;
		}

		Result<std::optional<AutosaveRecoveryData>> ReadAutosaveRecovery(const LoadedProject& project, std::string_view generation)
		{
			if (!generation.empty() && !IsAutosaveGeneration(generation))
				return MakeError(ErrorCode::Validation, "invalid autosave generation identifier");
			ENGINE_TRY_ASSIGN(const std::filesystem::path projectFile, CanonicalAutosaveProject(project));
			ENGINE_TRY_ASSIGN(const AutosaveManifest manifest, ReadAutosaveManifest(projectFile.parent_path()));
			for (const AutosaveManifestEntry& entry : manifest.Generations)
			{
				if (!generation.empty() && entry.Generation != generation)
					continue;
				ENGINE_TRY_ASSIGN(AutosaveSnapshot snapshot, ReadAutosaveGeneration(projectFile, entry));
				AutosaveRecoveryInfo info = MakeAutosaveRecoveryInfo(snapshot, entry.Generation);
				if (!info.NewerThanSource)
					continue;
				return std::optional<AutosaveRecoveryData>(AutosaveRecoveryData{ .Info = std::move(info), .SceneText = std::move(snapshot.Text) });
			}
			return std::optional<AutosaveRecoveryData>{};
		}

		struct PreparedAutosaveRecovery
		{
			AutosaveSnapshot Baseline{};
			Scope<Scene> RecoveredScene{};
			std::optional<VfsPath> ScenePath{};
			std::filesystem::path ProjectIdentity{};
			std::string SettingsText{};
		};

		// Every fallible native read, identity check and scene load finishes before the host's state is changed.
		static Result<PreparedAutosaveRecovery> PrepareAutosaveRecovery(EditorContext& editor, const LoadedProject& project,
			const AutosaveRecoveryInfo& recovery)
		{
			if (!IsAutosaveGeneration(recovery.Generation))
				return MakeError(ErrorCode::Validation, "invalid autosave generation identifier");
			PreparedAutosaveRecovery prepared;
			ENGINE_TRY_ASSIGN(prepared.ProjectIdentity, CanonicalAutosaveProject(project));
			ENGINE_TRY_ASSIGN(const AutosaveManifest manifest, ReadAutosaveManifest(prepared.ProjectIdentity.parent_path()));
			const auto entry = std::find_if(manifest.Generations.begin(), manifest.Generations.end(), [&recovery](const auto& value)
			{
				return value.Generation == recovery.Generation;
			});
			if (entry == manifest.Generations.end())
				return MakeError(ErrorCode::NotFound, "the offered autosave is no longer recoverable");
			ENGINE_TRY_ASSIGN(prepared.Baseline, ReadAutosaveGeneration(prepared.ProjectIdentity, *entry));
			const AutosaveRecoveryInfo found = MakeAutosaveRecoveryInfo(prepared.Baseline, recovery.Generation);
			if (!found.NewerThanSource)
				return MakeError(ErrorCode::NotFound, "the offered autosave no longer differs from its source");
			if (found.ScenePath != recovery.ScenePath || found.SceneName != recovery.SceneName || found.SceneRevision != recovery.SceneRevision || found.Files != recovery.Files || found.NewerThanSource != recovery.NewerThanSource)
				return MakeError(ErrorCode::Validation, "the recovery offer does not match its committed metadata");
			prepared.RecoveredScene = editor.CreateScene(found.SceneName);
			LoadReport report;
			ENGINE_TRY(SceneSerializer::LoadFromString(*prepared.RecoveredScene, prepared.Baseline.Text, {}, report));
			if (prepared.RecoveredScene->GetName() != found.SceneName)
				return MakeError(ErrorCode::Validation, "autosave scene identity disagrees with its payload");
			if (!found.ScenePath.empty())
			{
				ENGINE_TRY_ASSIGN(prepared.ScenePath, VfsPath::Create("project", found.ScenePath));
			}
			ENGINE_TRY_ASSIGN(prepared.SettingsText, ProjectSerializer::SaveToString(project.GetSettings(), editor.GetTypeRegistry()));

			// Recheck after temporary CPU construction. A replaced token or baseline is as stale as replaced payload bytes.
			ENGINE_TRY_ASSIGN(const AutosaveManifest checkedManifest, ReadAutosaveManifest(prepared.ProjectIdentity.parent_path()));
			const auto checkedEntry = std::find_if(checkedManifest.Generations.begin(), checkedManifest.Generations.end(), [&recovery](const auto& value)
			{
				return value.Generation == recovery.Generation;
			});
			if (checkedEntry == checkedManifest.Generations.end())
				return MakeError(ErrorCode::NotFound, "the offered recovery was retired during validation");
			ENGINE_TRY_ASSIGN(const AutosaveSnapshot checked, ReadAutosaveGeneration(prepared.ProjectIdentity, *checkedEntry));
			const AutosaveSnapshot& baseline = prepared.Baseline;
			if (checked.ProjectFingerprint != baseline.ProjectFingerprint || checked.SourceFingerprint != baseline.SourceFingerprint || checked.ScenePath != baseline.ScenePath || checked.SceneName != baseline.SceneName || checked.UntitledToken != baseline.UntitledToken || checked.Revision != baseline.Revision || checked.Text != baseline.Text)
				return MakeError(ErrorCode::Conflict, "the offered recovery changed while it was validated");
			prepared.Baseline.Text.clear();
			return prepared;
		}

		static Status WriteAutosaveBytes(const AutosaveSpecification& specification, const std::filesystem::path& path, std::string_view text)
		{
			const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
			return specification.WriteFile ? specification.WriteFile(path, bytes) : FileSystem::WriteFileAtomic(path, bytes, { .KeepBackup = false });
		}

		static Result<AutosaveWriteResult> WriteAutosaveGeneration(const AutosaveSnapshot& snapshot, const AutosaveSpecification& specification, AutosaveReason reason)
		{
			ENGINE_TRY(ValidateAutosaveSource(snapshot));
			ENGINE_TRY_ASSIGN(AutosaveManifest manifest, ReadAutosaveManifest(snapshot.Root));
			if (manifest.Sequence == std::numeric_limits<uint64_t>::max())
				return MakeError(ErrorCode::InvalidState, "autosave generation sequence is exhausted");
			const uint64_t sequence = manifest.Sequence + 1;
			const std::string generation = std::format("g-{:016x}", sequence);
			const std::filesystem::path directory = snapshot.AutosaveRoot / FileSystem::PathFromUtf8(generation);
			const std::filesystem::path payload = directory / "Scene.json";
			const std::filesystem::path metadata = directory / "Metadata.json";
			const std::filesystem::path manifestPath = snapshot.AutosaveRoot / "Manifest.json";
			ENGINE_TRY(CheckAutosavePath(snapshot.Root, payload));
			ENGINE_TRY(FileSystem::CreateDirectories(directory));
			ENGINE_TRY(CheckAutosavePath(snapshot.Root, payload));
			ENGINE_TRY(WriteAutosaveBytes(specification, payload, snapshot.Text));
			const Json document = {
				{ "Format", "AutosaveGeneration" },
				{ "Version", 1 },
				{ "Sequence", sequence },
				{ "ProjectFile", FileSystem::PathToUtf8(snapshot.ProjectFile) },
				{ "ProjectFingerprint", AutosaveFingerprintJson(snapshot.ProjectFingerprint) },
				{ "SourceFingerprint", AutosaveFingerprintJson(snapshot.SourceFingerprint) },
				{ "ScenePath", snapshot.ScenePath },
				{ "SceneName", snapshot.SceneName },
				{ "UntitledToken", snapshot.UntitledToken },
				{ "DirtyRevision", snapshot.Revision },
				{ "Dirty", true },
				{ "Reason", static_cast<uint32_t>(reason) },
				{ "PayloadSize", snapshot.Text.size() },
				{ "PayloadHash", XXH64(snapshot.Text) },
			};
			ENGINE_TRY_ASSIGN(const std::string metadataText, JsonWriter::Write(document));
			ENGINE_TRY(CheckAutosavePath(snapshot.Root, metadata));
			ENGINE_TRY(WriteAutosaveBytes(specification, metadata, metadataText));
			manifest.Sequence = sequence;
			manifest.Generations.insert(manifest.Generations.begin(), { .Generation = generation, .Sequence = sequence });
			std::vector<AutosaveManifestEntry> removed;
			if (manifest.Generations.size() > specification.RetainedGenerations)
			{
				removed.assign(manifest.Generations.begin() + specification.RetainedGenerations, manifest.Generations.end());
				manifest.Generations.resize(specification.RetainedGenerations);
			}
			ENGINE_TRY_ASSIGN(const std::string manifestText, AutosaveManifestText(manifest));
			ENGINE_TRY(ValidateAutosaveSource(snapshot));
			ENGINE_TRY(CheckAutosavePath(snapshot.Root, manifestPath));
			ENGINE_TRY(WriteAutosaveBytes(specification, manifestPath, manifestText));
			// Retirement happens only after publication; a failed new write never removes the previous manifest's payloads.
			for (const AutosaveManifestEntry& entry : removed)
			{
				const std::filesystem::path oldDirectory = snapshot.AutosaveRoot / FileSystem::PathFromUtf8(entry.Generation);
				ENGINE_TRY(CheckAutosavePath(snapshot.Root, oldDirectory));
				ENGINE_TRY(FileSystem::Remove(oldDirectory));
			}
			AutosaveWriteResult result{ .Written = true, .Generation = generation };
			if (!snapshot.ScenePath.empty())
				result.Files.push_back(snapshot.ScenePath);
			return result;
		}

	}

	struct Autosave::State
	{
		enum class SlotState : uint32_t
		{
			Free,
			Preparing,
			Published,
			Claimed
		};
		struct Slot
		{
			std::atomic<SlotState> Status{ SlotState::Free };
			Utils::AutosaveSnapshot Snapshot{};
		};
		static_assert(std::atomic<SlotState>::is_always_lock_free);

		EditorContext* Editor = nullptr; // back-reference: the host keeps the context and its project lock alive through Reset
		AutosaveSpecification Specification{};
		std::thread::id MainThread = std::this_thread::get_id();
		std::array<Slot, 3> Slots{};
		std::atomic<int32_t> Published{ Utils::NoAutosaveSlot };
		std::atomic<uint32_t> Gate{ 0 };
		std::atomic<bool> Enabled{ false };
		bool Closing = false;
		bool CanReopen = false;
		bool HasClock = false;
		double LastNow = 0;
		double LastAttempt = 0;
		std::optional<std::filesystem::path> ProjectIdentity{};
		std::optional<Utils::AutosaveFingerprint> ProjectFingerprint{};
		std::string ProjectSettingsText{};
		std::optional<Utils::AutosaveSnapshot> Base{}; // main-thread identity and source baseline; never owns scene bytes

		void AssertMain() const { ENGINE_ASSERT(MainThread == std::this_thread::get_id(), "Autosave requires the main thread"); }
		void Withdraw()
		{
			const int32_t old = Published.exchange(Utils::NoAutosaveSlot, std::memory_order_acq_rel);
			if (old != Utils::NoAutosaveSlot)
			{
				SlotState expected = SlotState::Published;
				Slots[static_cast<size_t>(old)].Status.compare_exchange_strong(expected, SlotState::Free, std::memory_order_acq_rel);
			}
		}
		void Release(int32_t index) noexcept
		{
			if (index != Utils::NoAutosaveSlot)
			{
				// This is the writer's final access to the slot. Only the main thread reclaims stale publications.
				// A second load/CAS here would risk acting on a later reuse of this index (ABA).
				Slots[static_cast<size_t>(index)].Status.store(SlotState::Published, std::memory_order_release);
			}
			Gate.fetch_and(~Utils::AutosaveWriterBit, std::memory_order_release);
		}
		struct WriterLease
		{
			State& Owner;
			int32_t Index = Utils::NoAutosaveSlot;
			~WriterLease() { Owner.Release(Index); }
		};
		int32_t ClaimSlot()
		{
			const int32_t index = Published.load(std::memory_order_acquire);
			if (index == Utils::NoAutosaveSlot)
				return index;
			SlotState expected = SlotState::Published;
			if (!Slots[static_cast<size_t>(index)].Status.compare_exchange_strong(expected, SlotState::Claimed, std::memory_order_acq_rel))
				return Utils::NoAutosaveSlot;
			return index;
		}
		void AdoptRecovery(Utils::PreparedAutosaveRecovery prepared)
		{
			Editor->SetScene(std::move(prepared.RecoveredScene), std::move(prepared.ScenePath), true);
			prepared.Baseline.Epoch = Editor->GetRevision() - Editor->GetScene().GetRevision();
			Withdraw();
			ProjectIdentity = std::move(prepared.ProjectIdentity);
			ProjectFingerprint = prepared.Baseline.ProjectFingerprint;
			ProjectSettingsText = std::move(prepared.SettingsText);
			Base = std::move(prepared.Baseline);
		}
	};

	Autosave::Autosave(EditorContext& context, const AutosaveSpecification& specification)
		: m_State(CreateScope<State>())
	{
		m_State->Editor = &context;
		m_State->Specification = specification;
	}

	Autosave::~Autosave()
	{
		ENGINE_VERIFY(Reset(), "The host must quiesce fatal autosave writers before destroying Autosave");
	}

	Status Autosave::Publish()
	{
		State& state = *m_State;
		state.AssertMain();
		if (state.Closing)
			return MakeError(ErrorCode::InvalidState, "autosave reset is waiting for its prior writer");
		if (state.CanReopen)
		{
			state.Gate.store(0, std::memory_order_release);
			state.CanReopen = false;
		}
		EditorContext& editor = *state.Editor;
		if (!editor.HasProject() || editor.IsReadOnly() || editor.IsDryRun() || !editor.HasScene())
		{
			state.Enabled.store(false, std::memory_order_release);
			state.Withdraw();
			return {};
		}
		if (!std::isfinite(state.Specification.IntervalSeconds) || state.Specification.IntervalSeconds <= 0 || state.Specification.RetainedGenerations == 0 || state.Specification.RetainedGenerations > Utils::MaxAutosaveGenerations)
			return MakeError(ErrorCode::Validation, "autosave requires a positive finite interval and 1 to 32 retained generations");
		// An unfinished gesture owns live preview changes. Preserve the preceding committed snapshot until it finishes.
		if (editor.GetTransaction() != nullptr || editor.GetScene().GetChangeTracker().IsTracking())
			return {};

		Utils::AutosaveSnapshot snapshot;
		ENGINE_TRY_ASSIGN(snapshot.ProjectFile, Utils::CanonicalAutosaveProject(editor.GetProject()));
		if (state.ProjectIdentity && *state.ProjectIdentity != snapshot.ProjectFile)
			return MakeError(ErrorCode::Conflict, "the open project's canonical identity changed without a completed reset");
		state.ProjectIdentity = snapshot.ProjectFile;
		snapshot.Root = snapshot.ProjectFile.parent_path();
		snapshot.AutosaveRoot = snapshot.Root / "Library" / "Autosave";
		snapshot.Epoch = editor.GetRevision() - editor.GetScene().GetRevision();
		snapshot.Revision = editor.GetRevision();
		snapshot.SceneName = editor.GetScene().GetName();
		ENGINE_TRY_ASSIGN(snapshot.ProjectFingerprint, Utils::ReadAutosaveFingerprint(snapshot.ProjectFile));
		if (!snapshot.ProjectFingerprint.Exists)
			return MakeError(ErrorCode::Conflict, "the autosave project no longer exists");
		ENGINE_TRY_ASSIGN(std::string settingsText, ProjectSerializer::SaveToString(editor.GetProject().GetSettings(), editor.GetTypeRegistry()));
		if (state.ProjectFingerprint && *state.ProjectFingerprint != snapshot.ProjectFingerprint)
		{
			// ProjectSettingsCommand updates the loaded settings only after its immediate atomic native write succeeds.
			// Admit exactly that observed settings change; an external rewrite (even equal bytes with a new timestamp)
			// cannot silently replace the identity captured when this project was opened.
			if (settingsText == state.ProjectSettingsText || snapshot.ProjectFingerprint.Size != settingsText.size() || snapshot.ProjectFingerprint.Hash != XXH64(settingsText))
				return MakeError(ErrorCode::Conflict, "the project file changed outside its committed settings write");
		}
		state.ProjectFingerprint = snapshot.ProjectFingerprint;
		state.ProjectSettingsText = std::move(settingsText);
		if (editor.GetScenePath())
		{
			snapshot.ScenePath = editor.GetScenePath()->GetPath();
			snapshot.Source = snapshot.Root / FileSystem::PathFromUtf8(snapshot.ScenePath);
			ENGINE_TRY(Utils::CheckAutosavePath(snapshot.Root, snapshot.Source));
		}
		const bool sameDocument = state.Base && state.Base->ProjectFile == snapshot.ProjectFile && state.Base->Epoch == snapshot.Epoch && state.Base->ScenePath == snapshot.ScenePath;
		if (sameDocument && !editor.IsSceneDirty())
		{
			// A clean open scene can still be stale on disk. Keep its loaded baseline until explicit save or reload;
			// adopting external bytes here would incorrectly call later edits derived from the changed source.
			state.Enabled.store(true, std::memory_order_release);
			state.Base->ProjectFingerprint = snapshot.ProjectFingerprint;
			state.Withdraw();
			return {};
		}
		if (!sameDocument)
		{
			if (!snapshot.Source.empty())
			{
				ENGINE_TRY_ASSIGN(snapshot.SourceFingerprint, Utils::ReadAutosaveFingerprint(snapshot.Source));
			}
			else
			{
				std::array<std::byte, 16> token{};
				ENGINE_TRY(SecureRandom::Fill(token));
				for (std::byte value : token)
					snapshot.UntitledToken += std::format("{:02x}", std::to_integer<uint8_t>(value));
			}
		}
		else
		{
			snapshot.SourceFingerprint = state.Base->SourceFingerprint;
			snapshot.UntitledToken = state.Base->UntitledToken;
		}
		ENGINE_TRY(Utils::ValidateAutosaveSource(snapshot));
		state.Base = snapshot;
		state.Enabled.store(true, std::memory_order_release);
		if (!editor.IsSceneDirty())
		{
			state.Withdraw();
			return {};
		}
		const int32_t published = state.Published.load(std::memory_order_acquire);
		if (published != Utils::NoAutosaveSlot)
		{
			const auto& previous = state.Slots[static_cast<size_t>(published)].Snapshot;
			if (previous.Revision == snapshot.Revision && previous.Epoch == snapshot.Epoch && previous.ScenePath == snapshot.ScenePath && previous.ProjectFingerprint == snapshot.ProjectFingerprint && previous.SourceFingerprint == snapshot.SourceFingerprint)
				return {};
		}
		ENGINE_TRY_ASSIGN(snapshot.Text, SceneSerializer::SaveToString(editor.GetScene()));
		if (snapshot.Text.size() > Utils::MaxAutosavePayloadBytes)
			return MakeError(ErrorCode::Validation, "autosave scene exceeds the 256 MiB recovery limit");
		if (snapshot.SourceFingerprint.Exists && snapshot.SourceFingerprint.Size == snapshot.Text.size() && snapshot.SourceFingerprint.Hash == XXH64(snapshot.Text))
		{
			state.Withdraw();
			return {};
		}
		for (size_t index = 0; index < state.Slots.size(); ++index)
		{
			State::Slot& slot = state.Slots[index];
			if (static_cast<int32_t>(index) != state.Published.load(std::memory_order_acquire))
			{
				State::SlotState stale = State::SlotState::Published;
				slot.Status.compare_exchange_strong(stale, State::SlotState::Free, std::memory_order_acq_rel);
			}
			State::SlotState expected = State::SlotState::Free;
			if (!slot.Status.compare_exchange_strong(expected, State::SlotState::Preparing, std::memory_order_acq_rel))
				continue;
			slot.Snapshot = std::move(snapshot);
			slot.Status.store(State::SlotState::Published, std::memory_order_release);
			const int32_t old = state.Published.exchange(static_cast<int32_t>(index), std::memory_order_acq_rel);
			if (old != Utils::NoAutosaveSlot)
			{
				expected = State::SlotState::Published;
				state.Slots[static_cast<size_t>(old)].Status.compare_exchange_strong(expected, State::SlotState::Free, std::memory_order_acq_rel);
			}
			return {};
		}
		return MakeError(ErrorCode::InvalidState, "autosave has no free publication slot");
	}

	Result<AutosaveWriteResult> Autosave::Update(double nowSeconds)
	{
		State& state = *m_State;
		state.AssertMain();
		if (!std::isfinite(nowSeconds) || nowSeconds < 0 || (state.HasClock && nowSeconds < state.LastNow))
			return MakeError(ErrorCode::InvalidArgument, "autosave clock must be finite, nonnegative and monotonic");
		state.HasClock = true;
		state.LastNow = nowSeconds;
		ENGINE_TRY(Publish());
		if (!state.Enabled.load(std::memory_order_acquire))
			return AutosaveWriteResult{};
		if (nowSeconds - state.LastAttempt < state.Specification.IntervalSeconds)
			return AutosaveWriteResult{};
		const EditorContext& editor = *state.Editor;
		if (editor.HasScene() && (editor.GetTransaction() != nullptr || editor.GetScene().GetChangeTracker().IsTracking()))
			return AutosaveWriteResult{};
		state.LastAttempt = nowSeconds;
		return Save(AutosaveReason::Periodic);
	}

	Result<AutosaveWriteResult> Autosave::Save(AutosaveReason reason)
	{
		State& state = *m_State;
		state.AssertMain();
		const EditorContext& editor = *state.Editor;
		if (editor.IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "read-only projects cannot write autosaves");
		if (editor.IsDryRun())
			return MakeError(ErrorCode::InvalidState, "dry runs cannot write autosaves");
		if (editor.HasScene() && (editor.GetTransaction() != nullptr || editor.GetScene().GetChangeTracker().IsTracking()))
			return MakeError(ErrorCode::InvalidState, "finish the active edit before saving a recovery");
		ENGINE_TRY(Publish());
		uint32_t expected = 0;
		if (!state.Gate.compare_exchange_strong(expected, Utils::AutosaveWriterBit, std::memory_order_acq_rel))
			return MakeError(ErrorCode::InvalidState, "autosave is closing or another writer owns the recovery files");
		State::WriterLease lease{ state, state.ClaimSlot() };
		if (lease.Index == Utils::NoAutosaveSlot)
			return AutosaveWriteResult{};
		return Utils::WriteAutosaveGeneration(state.Slots[static_cast<size_t>(lease.Index)].Snapshot, state.Specification, reason);
	}

	AutosaveFatalResult Autosave::WriteFatalSnapshot() noexcept
	{
		State& state = *m_State;
		uint32_t expected = 0;
		if (!state.Gate.compare_exchange_strong(expected, Utils::AutosaveWriterBit, std::memory_order_acq_rel))
			return (expected & Utils::AutosaveClosedBit) != 0 ? AutosaveFatalResult::Disabled : AutosaveFatalResult::Busy;
		State::WriterLease lease{ state };
		if (!state.Enabled.load(std::memory_order_acquire))
			return AutosaveFatalResult::Disabled;
		lease.Index = state.ClaimSlot();
		if (lease.Index == Utils::NoAutosaveSlot)
			return state.Published.load(std::memory_order_acquire) == Utils::NoAutosaveSlot ? AutosaveFatalResult::NothingDirty : AutosaveFatalResult::Busy;
		// Fatal-error boundary: no exception may escape into terminate/FatalError a second time. The lease's
		// non-allocating destructor releases closing admission even when native I/O runs out of memory.
		try
		{
			return Utils::WriteAutosaveGeneration(state.Slots[static_cast<size_t>(lease.Index)].Snapshot, state.Specification, AutosaveReason::FatalError)
				? AutosaveFatalResult::Saved
				: AutosaveFatalResult::IoFailure;
		}
		catch (...)
		{
			return AutosaveFatalResult::IoFailure;
		}
	}

	Result<std::optional<AutosaveRecoveryInfo>> Autosave::FindRecovery() const
	{
		m_State->AssertMain();
		EditorContext& editor = *m_State->Editor;
		if (!editor.HasProject())
			return std::optional<AutosaveRecoveryInfo>{};
		ENGINE_TRY_ASSIGN(const auto recovery, Utils::ReadAutosaveRecovery(editor.GetProject()));
		if (!recovery)
			return std::optional<AutosaveRecoveryInfo>{};
		const Scope<Scene> scene = editor.CreateScene(recovery->Info.SceneName);
		LoadReport report;
		ENGINE_TRY(SceneSerializer::LoadFromString(*scene, recovery->SceneText, {}, report));
		if (scene->GetName() != recovery->Info.SceneName)
			return MakeError(ErrorCode::Validation, "autosave scene identity disagrees with its payload");
		return std::optional<AutosaveRecoveryInfo>(recovery->Info);
	}

	Status Autosave::Recover(const AutosaveRecoveryInfo& recovery)
	{
		State& state = *m_State;
		state.AssertMain();
		EditorContext& editor = *state.Editor;
		if (editor.IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "read-only projects cannot adopt a recovery");
		if (!editor.HasProject() || editor.IsDryRun() || editor.GetTransaction() != nullptr || state.Closing)
			return MakeError(ErrorCode::InvalidState, "recovery requires an open writable project outside a transaction or reset");
		if (editor.HasScene() && editor.GetScene().GetChangeTracker().IsTracking())
			return MakeError(ErrorCode::InvalidState, "finish the active edit before recovering");
		uint32_t expected = state.Gate.load(std::memory_order_acquire);
		if ((expected & Utils::AutosaveWriterBit) != 0 || !state.Gate.compare_exchange_strong(expected, expected | Utils::AutosaveWriterBit, std::memory_order_acq_rel))
			return MakeError(ErrorCode::InvalidState, "another autosave writer owns the recovery files");
		State::WriterLease lease{ state };
		ENGINE_TRY_ASSIGN(auto prepared, Utils::PrepareAutosaveRecovery(editor, editor.GetProject(), recovery));
		state.AdoptRecovery(std::move(prepared));
		return {};
	}

	Status Autosave::OpenRecoveredProject(Scope<LoadedProject> project, const AutosaveRecoveryInfo& recovery)
	{
		State& state = *m_State;
		state.AssertMain();
		EditorContext& editor = *state.Editor;
		if (project == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "recovery needs an already-locked project");
		if (project->IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "read-only projects cannot adopt a recovery");
		if (editor.HasProject() || editor.IsDryRun() || editor.GetTransaction() != nullptr || state.Closing || state.Base || state.ProjectIdentity || state.Published.load(std::memory_order_acquire) != Utils::NoAutosaveSlot || state.Enabled.load(std::memory_order_acquire))
			return MakeError(ErrorCode::InvalidState, "launcher recovery requires a quiescent service after its prior project was reset");
		uint32_t expected = state.Gate.load(std::memory_order_acquire);
		if ((expected & Utils::AutosaveWriterBit) != 0 || !state.Gate.compare_exchange_strong(expected, expected | Utils::AutosaveWriterBit, std::memory_order_acq_rel))
			return MakeError(ErrorCode::InvalidState, "another autosave writer owns the recovery files");
		// The lease also prevents a fatal claim from observing partial adoption. Closed admission stays closed until
		// the host's later Publish, and all expected failures precede the scene/baseline installation below.
		State::WriterLease lease{ state };
		ENGINE_TRY_ASSIGN(auto prepared, Utils::PrepareAutosaveRecovery(editor, *project, recovery));
		ENGINE_TRY(editor.OpenProject(std::move(project)));
		state.AdoptRecovery(std::move(prepared));
		return {};
	}

	Status Autosave::DiscardSavedRecovery()
	{
		State& state = *m_State;
		state.AssertMain();
		EditorContext& editor = *state.Editor;
		if (editor.IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "read-only projects cannot discard recoveries");
		if (editor.IsDryRun() || state.Closing || editor.GetTransaction() != nullptr)
			return MakeError(ErrorCode::InvalidState, "cannot discard recoveries during a dry run, reset or transaction");
		if (!editor.HasProject() || !editor.HasScene() || !editor.GetScenePath() || editor.IsSceneDirty() || !state.Base)
			return {};
		uint32_t expected = 0;
		if (!state.Gate.compare_exchange_strong(expected, Utils::AutosaveWriterBit, std::memory_order_acq_rel))
			return MakeError(ErrorCode::InvalidState, "autosave is closing or another recovery writer is active");
		State::WriterLease lease{ state };
		ENGINE_TRY_ASSIGN(const std::filesystem::path projectFile, Utils::CanonicalAutosaveProject(editor.GetProject()));
		const std::filesystem::path root = projectFile.parent_path();
		const std::string savedPath(editor.GetScenePath()->GetPath());
		const std::filesystem::path savedFile = root / FileSystem::PathFromUtf8(savedPath);
		ENGINE_TRY(Utils::CheckAutosavePath(root, savedFile));
		ENGINE_TRY_ASSIGN(const std::string actual, Utils::ReadAutosaveText(savedFile, Utils::MaxAutosavePayloadBytes));
		ENGINE_TRY_ASSIGN(const std::string current, SceneSerializer::SaveToString(editor.GetScene()));
		if (actual != current)
			return MakeError(ErrorCode::Conflict, "the source file does not contain the explicitly saved scene");
		const uint64_t epoch = editor.GetRevision() - editor.GetScene().GetRevision();
		if (state.Base->ProjectFile != projectFile || state.Base->Epoch != epoch)
			return {};
		ENGINE_TRY_ASSIGN(Utils::AutosaveManifest manifest, Utils::ReadAutosaveManifest(root));
		std::vector<Utils::AutosaveManifestEntry> removed;
		std::vector<Utils::AutosaveManifestEntry> kept;
		for (const Utils::AutosaveManifestEntry& entry : manifest.Generations)
		{
			ENGINE_TRY_ASSIGN(const auto snapshot, Utils::ReadAutosaveGeneration(projectFile, entry, false));
			const bool sameSource = snapshot.ScenePath == savedPath && state.Base->ScenePath == savedPath && snapshot.SourceFingerprint == state.Base->SourceFingerprint;
			const bool sameUntitled = snapshot.ScenePath.empty() && state.Base->ScenePath.empty() && !state.Base->UntitledToken.empty() && snapshot.UntitledToken == state.Base->UntitledToken;
			(sameSource || sameUntitled ? removed : kept).push_back(entry);
		}
		if (removed.empty())
		{
			state.Withdraw();
			state.Base.reset();
			return {};
		}
		manifest.Generations = std::move(kept);
		ENGINE_TRY_ASSIGN(const std::string text, Utils::AutosaveManifestText(manifest));
		const std::filesystem::path manifestPath = root / "Library" / "Autosave" / "Manifest.json";
		ENGINE_TRY(Utils::CheckAutosavePath(root, manifestPath));
		ENGINE_TRY(Utils::WriteAutosaveBytes(state.Specification, manifestPath, text));
		for (const auto& entry : removed)
		{
			const std::filesystem::path directory = manifestPath.parent_path() / FileSystem::PathFromUtf8(entry.Generation);
			ENGINE_TRY(Utils::CheckAutosavePath(root, directory));
			ENGINE_TRY(FileSystem::Remove(directory));
		}
		state.Withdraw();
		state.Base.reset();
		return {};
	}

	bool Autosave::Reset()
	{
		State& state = *m_State;
		state.AssertMain();
		state.Closing = true;
		state.Gate.fetch_or(Utils::AutosaveClosedBit, std::memory_order_acq_rel);
		state.Enabled.store(false, std::memory_order_release);
		state.Withdraw();
		if ((state.Gate.load(std::memory_order_acquire) & Utils::AutosaveWriterBit) != 0)
			return false;
		for (State::Slot& slot : state.Slots)
		{
			slot.Status.store(State::SlotState::Free, std::memory_order_relaxed);
			slot.Snapshot = {};
		}
		state.Base.reset();
		state.ProjectIdentity.reset();
		state.ProjectFingerprint.reset();
		state.ProjectSettingsText.clear();
		state.HasClock = false;
		state.LastNow = 0;
		state.LastAttempt = 0;
		state.Closing = false;
		state.CanReopen = true;
		return true;
	}

}
