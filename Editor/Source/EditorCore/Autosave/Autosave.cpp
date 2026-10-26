#include "EditorPCH.h"
#include "EditorCore/Autosave/Autosave.h"

#include "EditorCore/Autosave/Private/AutosaveRecoveryData.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Core/VfsPath.h"
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
#include <map>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		constexpr uint64_t MaxAutosavePayloadBytes = 256ull * 1024 * 1024;
		constexpr uint64_t MaxAutosaveMetadataBytes = 1024 * 1024;
		// AutosaveSpecification::RetainedGenerations' bound, per scene.
		constexpr uint32_t MaxAutosaveRetainedGenerations = 32;
		// Retention keeps the generations of at most MaxOpenScenes scenes (M20, decision 15), so a manifest holds at most
		// this many.
		constexpr uint32_t MaxAutosaveGenerations = MaxAutosaveRetainedGenerations * static_cast<uint32_t>(MaxOpenScenes);
		constexpr uint32_t AutosaveWriterBit = 1;
		constexpr uint32_t AutosaveClosedBit = 2;
		constexpr int32_t NoAutosaveSlot = -1;
		// The project file and each open scene's source: the fingerprints Publish keeps between generation writes.
		constexpr size_t MaxCachedAutosaveFingerprints = MaxOpenScenes + 1;
		static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<int32_t>::is_always_lock_free);

		using AutosaveReadObserver = std::function<void(const std::filesystem::path&)>;

		struct AutosaveFingerprint
		{
			bool Exists = false;
			uint64_t Hash = 0;
			uint64_t Size = 0;
			uint64_t ModificationTime = 0;
			bool operator==(const AutosaveFingerprint&) const = default;
		};

		// What a stat reports about a fingerprinted file: equality only, like ModificationTime itself.
		struct AutosaveFileStamp
		{
			bool Exists = false;
			uint64_t Size = 0;
			uint64_t ModificationTime = 0;
			bool operator==(const AutosaveFileStamp&) const = default;
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
			uint64_t Epoch = 0; // the open scene's document: its OpenSceneId, never reused (M20)
		};

		// A completed write of one or more generations: the result, the manifest it committed, the generation each snapshot
		// became, and the first failure of a scene that was left out.
		struct AutosaveGenerationWrite
		{
			AutosaveWriteResult Result{};
			AutosaveManifest Manifest{};
			std::vector<std::pair<const AutosaveSnapshot*, std::string>> Generations{};
			std::optional<Error> Failure{};
		};

		// The scene a generation belongs to (M20, decision 15): its project-relative path, or its untitled document's token.
		static std::string AutosaveSceneKey(const AutosaveSnapshot& snapshot)
		{
			return snapshot.ScenePath.empty() ? std::format("untitled:{}", snapshot.UntitledToken) : std::format("scene:{}", snapshot.ScenePath);
		}

		static AutosaveFileStamp StampOf(const AutosaveFingerprint& fingerprint)
		{
			return AutosaveFileStamp{ .Exists = fingerprint.Exists, .Size = fingerprint.Size, .ModificationTime = fingerprint.ModificationTime };
		}

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
		static Result<std::string> ReadAutosaveBytes(const std::filesystem::path& path, uint64_t limit, const AutosaveReadObserver& observe)
		{
			ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(path));
			if (info.IsDirectory || info.Size > limit)
				return MakeError(ErrorCode::Validation, "autosave file '{}' exceeds its size limit or is a directory", FileSystem::PathToUtf8(path));
			if (observe)
				observe(path);
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

		static Result<std::string> ReadAutosaveText(const std::filesystem::path& path, uint64_t limit, const AutosaveReadObserver& observe)
		{
			ENGINE_TRY_ASSIGN(std::string text, ReadAutosaveBytes(path, limit, observe));
			if (!IsValidUtf8(text))
				return MakeError(ErrorCode::Validation, "autosave file '{}' is not UTF-8", FileSystem::PathToUtf8(path));
			return text;
		}

		// The size and opaque write time of a fingerprinted file, without reading it; a missing file does not exist.
		static Result<AutosaveFileStamp> ReadAutosaveStamp(const std::filesystem::path& path)
		{
			const Result<FileInfo> info = FileSystem::GetInfo(path);
			if (!info)
			{
				if (info.error().GetCode() == ErrorCode::NotFound)
					return AutosaveFileStamp{};
				return std::unexpected(info.error());
			}
			if (info->IsDirectory || info->Size > MaxAutosavePayloadBytes)
				return MakeError(ErrorCode::Validation, "autosave source '{}' is not a bounded file", FileSystem::PathToUtf8(path));
			return AutosaveFileStamp{ .Exists = true, .Size = info->Size, .ModificationTime = info->ModificationTime };
		}

		static Result<AutosaveFingerprint> ReadAutosaveFingerprint(const std::filesystem::path& path, const AutosaveReadObserver& observe)
		{
			ENGINE_TRY_ASSIGN(const AutosaveFileStamp before, ReadAutosaveStamp(path));
			if (!before.Exists)
				return AutosaveFingerprint{};
			ENGINE_TRY_ASSIGN(const std::string bytes, ReadAutosaveBytes(path, MaxAutosavePayloadBytes, observe));
			ENGINE_TRY_ASSIGN(const FileInfo after, FileSystem::GetInfo(path));
			if (before.Size != after.Size || before.ModificationTime != after.ModificationTime || bytes.size() != after.Size)
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

		// The checks ReadAutosaveBytes and ReadAutosaveText make on a document's bytes, for text held in memory.
		static Status CheckAutosaveText(std::string_view text, uint64_t limit, std::string_view name)
		{
			if (text.size() > limit)
				return MakeError(ErrorCode::Validation, "autosave file '{}' exceeds its size limit or is a directory", name);
			if (!IsValidUtf8(text))
				return MakeError(ErrorCode::Validation, "autosave file '{}' is not UTF-8", name);
			return {};
		}

		// The manifest (Library/Autosave/Manifest.json) read from its text: the format, the generation limit and each
		// generation's identity, newest first. Pure.
		static Result<AutosaveManifest> ParseAutosaveManifest(std::string_view text)
		{
			ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
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

		static Result<AutosaveManifest> ReadAutosaveManifest(const std::filesystem::path& root, const AutosaveReadObserver& observe)
		{
			const std::filesystem::path manifestPath = root / "Library" / "Autosave" / "Manifest.json";
			ENGINE_TRY(CheckAutosavePath(root, manifestPath));
			const Result<std::string> text = ReadAutosaveText(manifestPath, MaxAutosaveMetadataBytes, observe);
			if (!text)
			{
				if (text.error().GetCode() == ErrorCode::NotFound)
					return AutosaveManifest{};
				return std::unexpected(text.error());
			}
			return ParseAutosaveManifest(*text);
		}

		static Result<std::string> AutosaveManifestText(const AutosaveManifest& manifest)
		{
			Json entries = Json::array();
			for (const AutosaveManifestEntry& entry : manifest.Generations)
				entries.push_back(Json{ { "Generation", entry.Generation }, { "Sequence", entry.Sequence } });
			return JsonWriter::Write(Json{ { "Format", "AutosaveManifest" }, { "Version", 1 }, { "Sequence", manifest.Sequence },
				{ "Generations", std::move(entries) } });
		}

		// The full verification of a snapshot against `project`, the project file's current fingerprint: the project and
		// source bytes still hash to the snapshot's fingerprints.
		static Status ValidateAutosaveSource(const AutosaveSnapshot& snapshot, const AutosaveFingerprint& project, const AutosaveReadObserver& observe)
		{
			if (project != snapshot.ProjectFingerprint)
				return MakeError(ErrorCode::Conflict, "the project changed after the autosave snapshot was captured");
			if (!snapshot.ScenePath.empty())
			{
				ENGINE_TRY(CheckAutosavePath(snapshot.Root, snapshot.Source));
				ENGINE_TRY_ASSIGN(const AutosaveFingerprint source, ReadAutosaveFingerprint(snapshot.Source, observe));
				if (source != snapshot.SourceFingerprint)
					return MakeError(ErrorCode::Conflict, "scene '{}' changed after the autosave snapshot was captured", snapshot.ScenePath);
			}
			return {};
		}

		// The current fingerprint of `snapshot`'s project file, for the verification of its snapshots.
		static Result<AutosaveFingerprint> ReadAutosaveProjectFingerprint(const AutosaveSnapshot& snapshot, const AutosaveReadObserver& observe)
		{
			ENGINE_TRY(CheckAutosavePath(snapshot.Root, snapshot.ProjectFile));
			return ReadAutosaveFingerprint(snapshot.ProjectFile, observe);
		}

		static Status ValidateAutosaveSource(const AutosaveSnapshot& snapshot, const AutosaveReadObserver& observe)
		{
			ENGINE_TRY_ASSIGN(const AutosaveFingerprint project, ReadAutosaveProjectFingerprint(snapshot, observe));
			return ValidateAutosaveSource(snapshot, project, observe);
		}

		// The cheap check before a generation write: the files' sizes and opaque write times. The write verifies the bytes
		// once, before it publishes its manifest (ValidateAutosaveSource).
		static Status CheckAutosaveStamps(const AutosaveSnapshot& snapshot)
		{
			ENGINE_TRY_ASSIGN(const AutosaveFileStamp project, ReadAutosaveStamp(snapshot.ProjectFile));
			if (project != StampOf(snapshot.ProjectFingerprint))
				return MakeError(ErrorCode::Conflict, "the project changed after the autosave snapshot was captured");
			if (!snapshot.ScenePath.empty())
			{
				ENGINE_TRY_ASSIGN(const AutosaveFileStamp source, ReadAutosaveStamp(snapshot.Source));
				if (source != StampOf(snapshot.SourceFingerprint))
					return MakeError(ErrorCode::Conflict, "scene '{}' changed after the autosave snapshot was captured", snapshot.ScenePath);
			}
			return {};
		}

		// A generation's Metadata.json: the snapshot it describes (without the scene text) and what its Scene.json must be.
		struct AutosaveGenerationMetadata
		{
			AutosaveSnapshot Snapshot{};
			uint64_t PayloadSize = 0;
			uint64_t PayloadHash = 0;
		};

		// The generation `entry` of `projectFile`'s autosave from its Metadata.json text: the format, the identity of the
		// project and the generation, the fingerprints, the scene's path or untitled token, the captured revision and the
		// payload's promised size. Pure.
		static Result<AutosaveGenerationMetadata> ParseAutosaveGenerationMetadata(const std::filesystem::path& projectFile,
			const AutosaveManifestEntry& entry, std::string_view text)
		{
			AutosaveGenerationMetadata result;
			AutosaveSnapshot& snapshot = result.Snapshot;
			snapshot.Root = projectFile.parent_path();
			snapshot.ProjectFile = projectFile;
			snapshot.AutosaveRoot = snapshot.Root / "Library" / "Autosave";
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
			ENGINE_TRY_ASSIGN(result.PayloadSize, reader.ReadMember<uint64_t>("PayloadSize"));
			ENGINE_TRY_ASSIGN(result.PayloadHash, reader.ReadMember<uint64_t>("PayloadHash"));
			if (!dirty || snapshot.Revision == 0 || !snapshot.ProjectFingerprint.Exists || result.PayloadSize > MaxAutosavePayloadBytes)
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
			return result;
		}

		// A generation's Scene.json text against its metadata's promise.
		static Status CheckAutosavePayload(const AutosaveGenerationMetadata& metadata, std::string_view text)
		{
			if (text.size() != metadata.PayloadSize || XXH64(text) != metadata.PayloadHash)
				return MakeError(ErrorCode::Validation, "autosave payload size or checksum differs from its metadata");
			return {};
		}

		// The generation `entry`'s Metadata.json alone, without its payload or a verification of its source.
		static Result<AutosaveGenerationMetadata> ReadAutosaveGenerationMetadata(const std::filesystem::path& projectFile,
			const AutosaveManifestEntry& entry, const AutosaveReadObserver& observe)
		{
			const std::filesystem::path root = projectFile.parent_path();
			const std::filesystem::path metadataPath = root / "Library" / "Autosave" / FileSystem::PathFromUtf8(entry.Generation) / "Metadata.json";
			ENGINE_TRY(CheckAutosavePath(root, metadataPath));
			ENGINE_TRY_ASSIGN(const std::string text, ReadAutosaveText(metadataPath, MaxAutosaveMetadataBytes, observe));
			return ParseAutosaveGenerationMetadata(projectFile, entry, text);
		}

		static Result<AutosaveSnapshot> ReadAutosaveGeneration(const std::filesystem::path& projectFile, const AutosaveManifestEntry& entry,
			const AutosaveReadObserver& observe, bool verifySource = true)
		{
			const std::filesystem::path root = projectFile.parent_path();
			const std::filesystem::path payload = root / "Library" / "Autosave" / FileSystem::PathFromUtf8(entry.Generation) / "Scene.json";
			ENGINE_TRY(CheckAutosavePath(root, payload));
			ENGINE_TRY_ASSIGN(AutosaveGenerationMetadata metadata, ReadAutosaveGenerationMetadata(projectFile, entry, observe));
			AutosaveSnapshot& snapshot = metadata.Snapshot;
			ENGINE_TRY_ASSIGN(snapshot.Text, ReadAutosaveText(payload, MaxAutosavePayloadBytes, observe));
			ENGINE_TRY(CheckAutosavePayload(metadata, snapshot.Text));
			if (verifySource)
				ENGINE_TRY(ValidateAutosaveSource(snapshot, observe));
			return std::move(snapshot);
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
			ENGINE_TRY_ASSIGN(const AutosaveManifest manifest, ReadAutosaveManifest(projectFile.parent_path(), {}));
			for (const AutosaveManifestEntry& entry : manifest.Generations)
			{
				if (!generation.empty() && entry.Generation != generation)
					continue;
				ENGINE_TRY_ASSIGN(AutosaveSnapshot snapshot, ReadAutosaveGeneration(projectFile, entry, {}));
				AutosaveRecoveryInfo info = MakeAutosaveRecoveryInfo(snapshot, entry.Generation);
				if (!info.NewerThanSource)
					continue;
				return std::optional<AutosaveRecoveryData>(AutosaveRecoveryData{ .Info = std::move(info), .SceneText = std::move(snapshot.Text) });
			}
			return std::optional<AutosaveRecoveryData>{};
		}

		Result<std::vector<AutosaveRecoveryData>> ReadAutosaveRecoveries(const LoadedProject& project)
		{
			ENGINE_TRY_ASSIGN(const std::filesystem::path projectFile, CanonicalAutosaveProject(project));
			ENGINE_TRY_ASSIGN(const AutosaveManifest manifest, ReadAutosaveManifest(projectFile.parent_path(), {}));
			// The scenes that have their offer, or whose newest generation is stale.
			std::vector<std::string> settled;
			std::vector<AutosaveRecoveryData> recoveries;
			std::optional<Error> stale; // the conflict of the newest stale generation
			// Newest first: each scene's first generation that differs from its source is its offer.
			for (const AutosaveManifestEntry& entry : manifest.Generations)
			{
				ENGINE_TRY_ASSIGN(const AutosaveGenerationMetadata metadata, ReadAutosaveGenerationMetadata(projectFile, entry, {}));
				const std::string key = AutosaveSceneKey(metadata.Snapshot);
				if (std::ranges::find(settled, key) != settled.end())
					continue;
				ENGINE_TRY_ASSIGN(AutosaveSnapshot snapshot, ReadAutosaveGeneration(projectFile, entry, {}, false));
				// A generation whose project or source changed after it was captured (a scene closed with its changes
				// discarded, its file changed since) is stale, and so are that scene's older generations: the scene has no
				// offer, and the other scenes are still offered. With no offer at all, the conflict is the answer, as it is
				// FindRecovery's. Every other error fails the listing.
				Status source = ValidateAutosaveSource(snapshot, {});
				if (!source)
				{
					if (source.error().GetCode() != ErrorCode::Conflict)
						return std::unexpected(std::move(source).error());
					const std::string_view scene = snapshot.ScenePath.empty() ? std::string_view(snapshot.SceneName) : std::string_view(snapshot.ScenePath);
					ENGINE_WARN("Autosave: the unsaved changes of '{}' are not offered for recovery: {}", scene, source.error().ToString());
					if (!stale)
						stale = std::move(source).error();
					settled.push_back(key);
					continue;
				}
				AutosaveRecoveryInfo info = MakeAutosaveRecoveryInfo(snapshot, entry.Generation);
				if (!info.NewerThanSource)
					continue;
				settled.push_back(key);
				recoveries.push_back(AutosaveRecoveryData{ .Info = std::move(info), .SceneText = std::move(snapshot.Text) });
			}
			if (recoveries.empty() && stale)
				return std::unexpected(std::move(*stale));
			// Oldest first, so the scenes of one save come back in the tab order they were written in.
			std::ranges::reverse(recoveries);
			return recoveries;
		}

		Result<std::optional<AutosaveRecoveryData>> ParseAutosaveRecovery(const std::filesystem::path& projectFile,
			const AutosaveRecoveryDocuments& documents, std::string_view generation)
		{
			if (!generation.empty() && !IsAutosaveGeneration(generation))
				return MakeError(ErrorCode::Validation, "invalid autosave generation identifier");
			AutosaveManifest manifest;
			if (documents.Manifest.has_value())
			{
				ENGINE_TRY(CheckAutosaveText(*documents.Manifest, MaxAutosaveMetadataBytes, "Manifest.json"));
				ENGINE_TRY_ASSIGN(manifest, ParseAutosaveManifest(*documents.Manifest));
			}
			for (const AutosaveManifestEntry& entry : manifest.Generations)
			{
				if (!generation.empty() && entry.Generation != generation)
					continue;
				const auto found = documents.Generations.find(entry.Generation);
				if (found == documents.Generations.end())
					return MakeError(ErrorCode::NotFound, "autosave generation '{}' has no documents", entry.Generation);
				ENGINE_TRY(CheckAutosaveText(found->second.Metadata, MaxAutosaveMetadataBytes, "Metadata.json"));
				ENGINE_TRY_ASSIGN(AutosaveGenerationMetadata metadata, ParseAutosaveGenerationMetadata(projectFile, entry, found->second.Metadata));
				ENGINE_TRY(CheckAutosaveText(found->second.Scene, MaxAutosavePayloadBytes, "Scene.json"));
				metadata.Snapshot.Text = found->second.Scene;
				ENGINE_TRY(CheckAutosavePayload(metadata, metadata.Snapshot.Text));
				AutosaveRecoveryInfo info = MakeAutosaveRecoveryInfo(metadata.Snapshot, entry.Generation);
				if (!info.NewerThanSource)
					continue;
				return std::optional<AutosaveRecoveryData>(AutosaveRecoveryData{ .Info = std::move(info), .SceneText = std::move(metadata.Snapshot.Text) });
			}
			return std::optional<AutosaveRecoveryData>{};
		}

		struct PreparedAutosaveRecovery
		{
			AutosaveSnapshot Baseline{};
			Scope<Scene> RecoveredScene{};
			std::optional<VfsPath> ScenePath{};
			std::filesystem::path ProjectIdentity{};
			std::string SceneKey{}; // AutosaveSceneKey of the recovered scene
		};

		// Every fallible native read, identity check and scene load finishes before the host's state is changed.
		static Result<PreparedAutosaveRecovery> PrepareAutosaveRecovery(EditorContext& editor, const LoadedProject& project,
			const AutosaveRecoveryInfo& recovery, const AutosaveReadObserver& observe)
		{
			if (!IsAutosaveGeneration(recovery.Generation))
				return MakeError(ErrorCode::Validation, "invalid autosave generation identifier");
			PreparedAutosaveRecovery prepared;
			ENGINE_TRY_ASSIGN(prepared.ProjectIdentity, CanonicalAutosaveProject(project));
			ENGINE_TRY_ASSIGN(const AutosaveManifest manifest, ReadAutosaveManifest(prepared.ProjectIdentity.parent_path(), observe));
			const auto entry = std::find_if(manifest.Generations.begin(), manifest.Generations.end(), [&recovery](const auto& value)
			{
				return value.Generation == recovery.Generation;
			});
			if (entry == manifest.Generations.end())
				return MakeError(ErrorCode::NotFound, "the offered autosave is no longer recoverable");
			ENGINE_TRY_ASSIGN(prepared.Baseline, ReadAutosaveGeneration(prepared.ProjectIdentity, *entry, observe));
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

			// Recheck after temporary CPU construction. A replaced token or baseline is as stale as replaced payload bytes.
			ENGINE_TRY_ASSIGN(const AutosaveManifest checkedManifest, ReadAutosaveManifest(prepared.ProjectIdentity.parent_path(), observe));
			const auto checkedEntry = std::find_if(checkedManifest.Generations.begin(), checkedManifest.Generations.end(), [&recovery](const auto& value)
			{
				return value.Generation == recovery.Generation;
			});
			if (checkedEntry == checkedManifest.Generations.end())
				return MakeError(ErrorCode::NotFound, "the offered recovery was retired during validation");
			ENGINE_TRY_ASSIGN(const AutosaveSnapshot checked, ReadAutosaveGeneration(prepared.ProjectIdentity, *checkedEntry, observe));
			const AutosaveSnapshot& baseline = prepared.Baseline;
			if (checked.ProjectFingerprint != baseline.ProjectFingerprint || checked.SourceFingerprint != baseline.SourceFingerprint || checked.ScenePath != baseline.ScenePath || checked.SceneName != baseline.SceneName || checked.UntitledToken != baseline.UntitledToken || checked.Revision != baseline.Revision || checked.Text != baseline.Text)
				return MakeError(ErrorCode::Conflict, "the offered recovery changed while it was validated");
			prepared.Baseline.Text.clear();
			prepared.SceneKey = AutosaveSceneKey(prepared.Baseline);
			return prepared;
		}

		static Status WriteAutosaveBytes(const AutosaveSpecification& specification, const std::filesystem::path& path, std::string_view text)
		{
			const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
			return specification.WriteFile ? specification.WriteFile(path, bytes) : FileSystem::WriteFileAtomic(path, bytes, { .KeepBackup = false });
		}

		// Removes a generation directory the committed manifest no longer names. Best effort: the manifest is already the
		// durable state, so a directory that is gone or held by a scanner never fails the write; a later sweep retries it.
		// Linked or redirected entries are never followed (CheckAutosavePath).
		static void RemoveRetiredAutosaveGeneration(const std::filesystem::path& root, const std::filesystem::path& directory)
		{
			if (CheckAutosavePath(root, directory))
				static_cast<void>(FileSystem::Remove(directory));
		}

		// Main thread, under the writer lease, after a manifest was committed: removes every generation directory that
		// manifest does not name (a retirement whose removal failed, a write that failed before its manifest).
		static void SweepAutosaveGenerations(const std::filesystem::path& root, const AutosaveManifest& manifest)
		{
			const std::filesystem::path autosaveRoot = root / "Library" / "Autosave";
			const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(autosaveRoot, false);
			if (!entries)
				return;
			for (const std::filesystem::path& entry : *entries)
			{
				const std::string name = FileSystem::PathToUtf8(entry.filename());
				const bool named = std::ranges::any_of(manifest.Generations, [&name](const AutosaveManifestEntry& kept)
				{
					return kept.Generation == name;
				});
				if (IsAutosaveGeneration(name) && !named)
					RemoveRetiredAutosaveGeneration(root, autosaveRoot / FileSystem::PathFromUtf8(name));
			}
		}

		// Retention (M20, decision 15): keeps the newest `retained` generations of each scene, for the MaxOpenScenes scenes
		// whose newest generations are the newest, and returns the entries it retired. `keys` caches each generation's scene
		// (AutosaveSceneKey); a generation not in it has its metadata read, and one whose metadata cannot be read counts as a
		// scene of its own, so it is retired once enough newer scenes were written.
		static std::vector<AutosaveManifestEntry> RetainAutosaveGenerations(AutosaveManifest& manifest, const std::filesystem::path& projectFile,
			uint32_t retained, std::map<std::string, std::string, std::less<>>& keys, const AutosaveReadObserver& observe)
		{
			std::vector<std::pair<std::string, uint32_t>> scenes; // each scene's key and kept generations, newest scene first
			std::vector<AutosaveManifestEntry> kept;
			std::vector<AutosaveManifestEntry> retired;
			for (const AutosaveManifestEntry& entry : manifest.Generations)
			{
				auto key = keys.find(entry.Generation);
				if (key == keys.end())
				{
					const Result<AutosaveGenerationMetadata> metadata = ReadAutosaveGenerationMetadata(projectFile, entry, observe);
					key = keys.emplace(entry.Generation, metadata ? AutosaveSceneKey(metadata->Snapshot) : std::format("generation:{}", entry.Generation)).first;
				}
				auto scene = std::ranges::find(scenes, key->second, &std::pair<std::string, uint32_t>::first);
				if (scene == scenes.end())
				{
					if (scenes.size() == MaxOpenScenes)
					{
						retired.push_back(entry);
						continue;
					}
					scenes.emplace_back(key->second, 0);
					scene = scenes.end() - 1;
				}
				if (scene->second == retained)
				{
					retired.push_back(entry);
					continue;
				}
				++scene->second;
				kept.push_back(entry);
			}
			manifest.Generations = std::move(kept);
			return retired;
		}

		// Writes one generation per snapshot (one scene each, in the given order, so their sequences follow it) on top of
		// `manifest`, the committed manifest, then publishes the manifest once, LAST, with retention per scene. A scene that
		// cannot be written (a changed project or source, a failed payload write) is left out with its failure, its
		// directory swept later; the others are published. Errors: the first scene's failure when none could be written;
		// an exhausted sequence; the manifest's write.
		static Result<AutosaveGenerationWrite> WriteAutosaveGenerations(std::span<const AutosaveSnapshot* const> snapshots, AutosaveManifest manifest,
			const AutosaveSpecification& specification, AutosaveReason reason, std::map<std::string, std::string, std::less<>>& keys)
		{
			ENGINE_ASSERT(!snapshots.empty(), "WriteAutosaveGenerations needs a snapshot");
			const AutosaveSnapshot& first = *snapshots.front();
			AutosaveGenerationWrite result;
			const auto fail = [&result](Error error)
			{
				if (!result.Failure.has_value())
					result.Failure = std::move(error);
			};
			const auto writeGeneration = [&specification, reason](const AutosaveSnapshot& snapshot, uint64_t sequence, const std::string& generation) -> Status
			{
				const std::filesystem::path directory = snapshot.AutosaveRoot / FileSystem::PathFromUtf8(generation);
				const std::filesystem::path payload = directory / "Scene.json";
				const std::filesystem::path metadata = directory / "Metadata.json";
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
				return WriteAutosaveBytes(specification, metadata, metadataText);
			};

			// Each scene's payload and metadata, in the snapshots' order.
			std::vector<std::pair<const AutosaveSnapshot*, AutosaveManifestEntry>> written;
			for (const AutosaveSnapshot* snapshot : snapshots)
			{
				if (Status stamps = CheckAutosaveStamps(*snapshot); !stamps)
				{
					fail(std::move(stamps).error());
					continue;
				}
				if (manifest.Sequence == std::numeric_limits<uint64_t>::max())
					return MakeError(ErrorCode::InvalidState, "autosave generation sequence is exhausted");
				const uint64_t sequence = ++manifest.Sequence;
				AutosaveManifestEntry entry{ .Generation = std::format("g-{:016x}", sequence), .Sequence = sequence };
				if (Status generation = writeGeneration(*snapshot, sequence, entry.Generation); !generation)
				{
					fail(std::move(generation).error());
					continue;
				}
				written.emplace_back(snapshot, std::move(entry));
			}

			// The one full verification of the write, after its payloads, the project file read once: a scene whose source
			// changed meanwhile publishes nothing.
			if (!written.empty())
			{
				const Result<AutosaveFingerprint> project = ReadAutosaveProjectFingerprint(first, specification.ReadObserver);
				std::erase_if(written, [&project, &specification, &fail](const auto& entry)
				{
					Status valid = project ? ValidateAutosaveSource(*entry.first, *project, specification.ReadObserver) : Status(std::unexpected(project.error()));
					if (valid)
						return false;
					fail(std::move(valid).error());
					return true;
				});
			}
			if (written.empty())
			{
				ENGINE_ASSERT(result.Failure.has_value(), "an autosave write without a generation names its failure");
				return std::unexpected(std::move(*result.Failure));
			}

			// Newest first, as the manifest holds them.
			for (const auto& [snapshot, entry] : written)
			{
				manifest.Generations.insert(manifest.Generations.begin(), entry);
				keys.insert_or_assign(entry.Generation, AutosaveSceneKey(*snapshot));
			}
			const std::vector<AutosaveManifestEntry> retired = RetainAutosaveGenerations(manifest, first.ProjectFile, specification.RetainedGenerations, keys,
				specification.ReadObserver);
			ENGINE_TRY_ASSIGN(const std::string manifestText, AutosaveManifestText(manifest));
			const std::filesystem::path manifestPath = first.AutosaveRoot / "Manifest.json";
			ENGINE_TRY(CheckAutosavePath(first.Root, manifestPath));
			ENGINE_TRY(WriteAutosaveBytes(specification, manifestPath, manifestText));
			// Retirement happens only after publication; a failed new write never removes the previous manifest's payloads.
			for (const AutosaveManifestEntry& entry : retired)
				RemoveRetiredAutosaveGeneration(first.Root, first.AutosaveRoot / FileSystem::PathFromUtf8(entry.Generation));
			std::erase_if(keys, [&manifest](const auto& key)
			{
				return std::ranges::find(manifest.Generations, key.first, &AutosaveManifestEntry::Generation) == manifest.Generations.end();
			});

			result.Result.Written = true;
			for (const auto& [snapshot, entry] : written)
			{
				if (!snapshot->ScenePath.empty())
					result.Result.Files.push_back(snapshot->ScenePath);
				result.Generations.emplace_back(snapshot, entry.Generation);
			}
			result.Result.Generation = written.back().second.Generation;
			result.Manifest = std::move(manifest);
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
		// One publication: the immutable snapshots of every dirty open scene, in tab order (M20, decision 15). A snapshot is
		// shared by the publications that hold it, never changed after it was published, and freed on the main thread once
		// no publication holds it; the fatal path only reads the publication it claimed.
		struct Slot
		{
			std::atomic<SlotState> Status{ SlotState::Free };
			std::vector<Ref<const Utils::AutosaveSnapshot>> Snapshots{};
		};
		static_assert(std::atomic<SlotState>::is_always_lock_free);

		// What decides a scene's Publish outcome besides file bytes: equal keys give the same outcome (Publish's comment).
		struct PublishKey
		{
			const LoadedProject* Project = nullptr; // identity only
			OpenSceneId SceneId = NoOpenScene;
			uint64_t Revision = 0;
			bool Dirty = false;
			bool ChangedOnDisk = false;
			std::string ScenePath{};
			Utils::AutosaveFileStamp ProjectStamp{};
			Utils::AutosaveFileStamp SourceStamp{};
			bool operator==(const PublishKey&) const = default;
		};
		// The main thread's state of one open scene.
		struct SceneState
		{
			OpenSceneId Id = NoOpenScene;
			std::optional<Utils::AutosaveSnapshot> Base{}; // identity and source baseline; never owns scene bytes
			std::optional<PublishKey> LastKey{};           // the key of LastResult
			Status LastResult{};                           // the scene's last Publish outcome
			Ref<const Utils::AutosaveSnapshot> Snapshot{}; // its published snapshot, null when it has none (clean)
		};
		struct CachedFingerprint
		{
			std::filesystem::path Path{};
			Utils::AutosaveFingerprint Fingerprint{};
		};
		// The newest generation this service wrote for one scene and the snapshot it holds; touched only under the writer
		// lease.
		struct WrittenGeneration
		{
			std::string Key{}; // Utils::AutosaveSceneKey
			std::string Generation{};
			std::filesystem::path ProjectFile{};
			std::string ScenePath{};
			std::string UntitledToken{};
			Utils::AutosaveFingerprint ProjectFingerprint{};
			Utils::AutosaveFingerprint SourceFingerprint{};
			uint64_t Revision = 0;
			uint64_t Epoch = 0;
		};
		// The project-level part of a publication, captured once per Publish that needs it.
		struct ProjectCapture
		{
			std::filesystem::path ProjectFile{};
			Utils::AutosaveFingerprint Fingerprint{};
		};

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
		std::vector<SceneState> Scenes{};              // main thread: the open scenes of the last Publish, in tab order
		std::vector<CachedFingerprint> Fingerprints{}; // main thread, Publish only; the fatal path always reads the bytes
		// Under the writer lease: the newest generation written per scene, and the scene of each generation the writes and
		// the retention read (Utils::AutosaveSceneKey by generation).
		std::vector<WrittenGeneration> LastWritten{};
		std::map<std::string, std::string, std::less<>> GenerationKeys{};

		void AssertMain() const { ENGINE_ASSERT(MainThread == std::this_thread::get_id(), "Autosave requires the main thread"); }
		// Main thread: frees the publication `index` unless the fatal path claimed it meanwhile, then drops its snapshots.
		void FreeSlot(size_t index)
		{
			SlotState expected = SlotState::Published;
			if (Slots[index].Status.compare_exchange_strong(expected, SlotState::Free, std::memory_order_acq_rel))
				Slots[index].Snapshots.clear();
		}
		void Withdraw()
		{
			const int32_t old = Published.exchange(Utils::NoAutosaveSlot, std::memory_order_acq_rel);
			if (old != Utils::NoAutosaveSlot)
				FreeSlot(static_cast<size_t>(old));
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

		SceneState* FindScene(OpenSceneId id)
		{
			const auto scene = std::ranges::find(Scenes, id, &SceneState::Id);
			return scene != Scenes.end() ? &*scene : nullptr;
		}
		SceneState& GetScene(OpenSceneId id)
		{
			if (SceneState* scene = FindScene(id); scene != nullptr)
				return *scene;
			return Scenes.emplace_back(SceneState{ .Id = id });
		}
		// Forgets every scene's cached Publish outcome, so the next call recomputes it.
		void ForgetPublication()
		{
			for (SceneState& scene : Scenes)
			{
				scene.LastKey.reset();
				scene.LastResult = {};
			}
		}
		// Withdraws every scene's snapshot and the publication.
		void WithdrawAll()
		{
			for (SceneState& scene : Scenes)
				scene.Snapshot.reset();
			Withdraw();
		}
		// Brings the scene states in step with the editor's open scenes (their tab order; a closed scene's state goes) and
		// publishes their snapshots as one publication, unless it holds exactly those already.
		Status UpdatePublication()
		{
			const std::vector<OpenSceneId> open = Editor->GetOpenScenes();
			std::vector<SceneState> ordered;
			ordered.reserve(open.size());
			for (const OpenSceneId id : open)
			{
				if (SceneState* scene = FindScene(id); scene != nullptr)
					ordered.push_back(std::move(*scene));
			}
			Scenes = std::move(ordered);
			std::vector<Ref<const Utils::AutosaveSnapshot>> snapshots;
			for (const SceneState& scene : Scenes)
			{
				if (scene.Snapshot != nullptr)
					snapshots.push_back(scene.Snapshot);
			}
			const int32_t current = Published.load(std::memory_order_acquire);
			if (current != Utils::NoAutosaveSlot && Slots[static_cast<size_t>(current)].Snapshots == snapshots)
				return {};
			if (snapshots.empty())
			{
				Withdraw();
				return {};
			}
			for (size_t index = 0; index < Slots.size(); ++index)
			{
				Slot& slot = Slots[index];
				// A publication that a finished fatal writer released after it was replaced is reclaimed here.
				if (static_cast<int32_t>(index) != Published.load(std::memory_order_acquire))
					FreeSlot(index);
				SlotState expected = SlotState::Free;
				if (!slot.Status.compare_exchange_strong(expected, SlotState::Preparing, std::memory_order_acq_rel))
					continue;
				slot.Snapshots = std::move(snapshots);
				slot.Status.store(SlotState::Published, std::memory_order_release);
				const int32_t old = Published.exchange(static_cast<int32_t>(index), std::memory_order_acq_rel);
				if (old != Utils::NoAutosaveSlot)
					FreeSlot(static_cast<size_t>(old));
				return {};
			}
			return MakeError(ErrorCode::InvalidState, "autosave has no free publication slot");
		}
		// Whether the scene `id` (a snapshot's Epoch) published without error at the last Publish.
		bool IsPublishedWithoutError(uint64_t id) const
		{
			const auto scene = std::ranges::find(Scenes, static_cast<OpenSceneId>(id), &SceneState::Id);
			return scene != Scenes.end() && scene->LastResult.has_value() && scene->Snapshot != nullptr;
		}
		bool HasPublishedWithoutError() const
		{
			return std::ranges::any_of(Scenes, [](const SceneState& scene)
			{
				return scene.LastResult.has_value() && scene.Snapshot != nullptr;
			});
		}
		void AdoptRecovery(Utils::PreparedAutosaveRecovery prepared, OpenSceneId id)
		{
			SceneState& scene = GetScene(id);
			prepared.Baseline.Epoch = id;
			scene.Snapshot.reset();
			scene.LastKey.reset();
			scene.LastResult = {};
			ProjectIdentity = std::move(prepared.ProjectIdentity);
			ProjectFingerprint = prepared.Baseline.ProjectFingerprint;
			scene.Base = std::move(prepared.Baseline);
			static_cast<void>(UpdatePublication());
		}

		// The key of the open scene `info`, or nullopt when a file cannot be inspected (the slow path reports why).
		std::optional<PublishKey> MakePublishKey(const OpenSceneInfo& info, const std::optional<Utils::AutosaveFileStamp>& projectStamp) const
		{
			if (!projectStamp.has_value())
				return std::nullopt;
			const EditorContext& editor = *Editor;
			PublishKey key;
			key.Project = &editor.GetProject();
			key.SceneId = info.Id;
			key.Revision = info.Revision;
			key.Dirty = info.Dirty;
			key.ChangedOnDisk = info.ChangedOnDisk;
			key.ProjectStamp = *projectStamp;
			if (info.Path)
				key.ScenePath = info.Path->GetPath();
			if (!key.ScenePath.empty())
			{
				const Result<Utils::AutosaveFileStamp> source = Utils::ReadAutosaveStamp(editor.GetProject().GetRoot() / FileSystem::PathFromUtf8(key.ScenePath));
				if (!source)
					return std::nullopt;
				key.SourceStamp = *source;
			}
			return key;
		}

		// The fingerprint of `path`, reading and hashing its bytes only when its size or write time changed since the last
		// read through this cache.
		Result<Utils::AutosaveFingerprint> ReadCachedFingerprint(const std::filesystem::path& path)
		{
			ENGINE_TRY_ASSIGN(const Utils::AutosaveFileStamp stamp, Utils::ReadAutosaveStamp(path));
			if (!stamp.Exists)
				return Utils::AutosaveFingerprint{};
			const auto cached = std::ranges::find(Fingerprints, path, &CachedFingerprint::Path);
			if (cached != Fingerprints.end() && Utils::StampOf(cached->Fingerprint) == stamp)
				return cached->Fingerprint;
			ENGINE_TRY_ASSIGN(const Utils::AutosaveFingerprint fingerprint, Utils::ReadAutosaveFingerprint(path, Specification.ReadObserver));
			if (cached != Fingerprints.end())
				Fingerprints.erase(cached);
			if (fingerprint.Exists)
			{
				if (Fingerprints.size() == Utils::MaxCachedAutosaveFingerprints)
					Fingerprints.erase(Fingerprints.begin());
				Fingerprints.push_back(CachedFingerprint{ .Path = path, .Fingerprint = fingerprint });
			}
			return fingerprint;
		}

		// Whether `snapshot` is already the newest generation of its scene in `manifest`, written by this service, and still
		// recoverable as far as a stat can tell (unchanged project and source). Under the writer lease.
		bool IsNewestWritten(const Utils::AutosaveSnapshot& snapshot, const Utils::AutosaveManifest& manifest) const
		{
			const std::string key = Utils::AutosaveSceneKey(snapshot);
			const auto written = std::ranges::find_if(LastWritten, [&snapshot, &key](const WrittenGeneration& candidate)
			{
				return candidate.Key == key && candidate.ProjectFile == snapshot.ProjectFile;
			});
			if (written == LastWritten.end() || written->ScenePath != snapshot.ScenePath || written->UntitledToken != snapshot.UntitledToken
				|| written->ProjectFingerprint != snapshot.ProjectFingerprint || written->SourceFingerprint != snapshot.SourceFingerprint
				|| written->Revision != snapshot.Revision || written->Epoch != snapshot.Epoch)
				return false;
			if (!Utils::CheckAutosaveStamps(snapshot))
				return false;
			// Still in the manifest, and no newer generation there belongs to the same scene, or to one this service cannot
			// name.
			for (const Utils::AutosaveManifestEntry& entry : manifest.Generations)
			{
				if (entry.Generation == written->Generation)
					return true;
				const auto known = GenerationKeys.find(entry.Generation);
				if (known == GenerationKeys.end() || known->second == key)
					return false;
			}
			return false;
		}
		// Under the writer lease, after a write: each written scene's newest generation, and only those the manifest names.
		void RecordWritten(const Utils::AutosaveGenerationWrite& written)
		{
			for (const auto& [snapshot, generation] : written.Generations)
			{
				const std::string key = Utils::AutosaveSceneKey(*snapshot);
				std::erase_if(LastWritten, [&key, snapshot](const WrittenGeneration& entry)
				{
					return entry.Key == key && entry.ProjectFile == snapshot->ProjectFile;
				});
				LastWritten.push_back(WrittenGeneration{ .Key = key,
					.Generation = generation,
					.ProjectFile = snapshot->ProjectFile,
					.ScenePath = snapshot->ScenePath,
					.UntitledToken = snapshot->UntitledToken,
					.ProjectFingerprint = snapshot->ProjectFingerprint,
					.SourceFingerprint = snapshot->SourceFingerprint,
					.Revision = snapshot->Revision,
					.Epoch = snapshot->Epoch });
			}
			ForgetRetiredWrites(written.Manifest);
		}
		void ForgetRetiredWrites(const Utils::AutosaveManifest& manifest)
		{
			std::erase_if(LastWritten, [&manifest](const WrittenGeneration& entry)
			{
				return std::ranges::find(manifest.Generations, entry.Generation, &Utils::AutosaveManifestEntry::Generation) == manifest.Generations.end();
			});
		}
		// Under the writer lease: writes `snapshots` in their order, one generation each, and publishes the manifest once.
		// With `skipWritten`, a snapshot that is already the newest generation of its scene this service wrote is not
		// written again. Nothing to write: Written is false.
		Result<Utils::AutosaveGenerationWrite> WriteSnapshots(std::span<const Ref<const Utils::AutosaveSnapshot>> snapshots, AutosaveReason reason, bool skipWritten)
		{
			if (snapshots.empty())
				return Utils::AutosaveGenerationWrite{};
			ENGINE_TRY_ASSIGN(Utils::AutosaveManifest manifest, Utils::ReadAutosaveManifest(snapshots.front()->Root, Specification.ReadObserver));
			std::vector<const Utils::AutosaveSnapshot*> pending;
			for (const Ref<const Utils::AutosaveSnapshot>& snapshot : snapshots)
			{
				if (!skipWritten || !IsNewestWritten(*snapshot, manifest))
					pending.push_back(snapshot.get());
			}
			if (pending.empty())
				return Utils::AutosaveGenerationWrite{};
			ENGINE_TRY_ASSIGN(Utils::AutosaveGenerationWrite written, Utils::WriteAutosaveGenerations(pending, std::move(manifest), Specification, reason, GenerationKeys));
			RecordWritten(written);
			return written;
		}

		// The project-level part of a publication: the canonical identity and the admitted fingerprint of the project file.
		Result<ProjectCapture> CaptureProject()
		{
			EditorContext& editor = *Editor;
			ProjectCapture capture;
			ENGINE_TRY_ASSIGN(capture.ProjectFile, Utils::CanonicalAutosaveProject(editor.GetProject()));
			if (ProjectIdentity && *ProjectIdentity != capture.ProjectFile)
				return MakeError(ErrorCode::Conflict, "the open project's canonical identity changed without a completed reset");
			ProjectIdentity = capture.ProjectFile;
			ENGINE_TRY_ASSIGN(capture.Fingerprint, ReadCachedFingerprint(capture.ProjectFile));
			if (!capture.Fingerprint.Exists)
				return MakeError(ErrorCode::Conflict, "the autosave project no longer exists");
			if (ProjectFingerprint && *ProjectFingerprint != capture.Fingerprint)
			{
				// The editor's own .eproj writes (settings commands with their undo, redo and batch rollback, project.upgrade)
				// leave the file holding exactly the canonical serialization of the loaded settings, even when several
				// happened since the last publication. Admit any changed identity with those bytes; an external rewrite to
				// anything else cannot silently replace the identity captured when this project was opened.
				ENGINE_TRY_ASSIGN(const std::string settingsText, ProjectSerializer::SaveToString(editor.GetProject().GetSettings(), editor.GetTypeRegistry()));
				if (capture.Fingerprint.Size != settingsText.size() || capture.Fingerprint.Hash != XXH64(settingsText))
					return MakeError(ErrorCode::Conflict, "the project file changed outside the editor's settings writes");
			}
			ProjectFingerprint = capture.Fingerprint;
			Enabled.store(true, std::memory_order_release);
			return capture;
		}

		// One open scene's publication when its key changed (or could not be computed): its baseline, and its snapshot when
		// it is dirty and differs from its source.
		Status PublishScene(SceneState& state, const Scene& scene, const OpenSceneInfo& info, const ProjectCapture& project)
		{
			Utils::AutosaveSnapshot snapshot;
			snapshot.ProjectFile = project.ProjectFile;
			snapshot.Root = snapshot.ProjectFile.parent_path();
			snapshot.AutosaveRoot = snapshot.Root / "Library" / "Autosave";
			snapshot.Epoch = info.Id;
			snapshot.Revision = info.Revision;
			snapshot.SceneName = scene.GetName();
			snapshot.ProjectFingerprint = project.Fingerprint;
			if (info.Path)
			{
				snapshot.ScenePath = info.Path->GetPath();
				snapshot.Source = snapshot.Root / FileSystem::PathFromUtf8(snapshot.ScenePath);
				ENGINE_TRY(Utils::CheckAutosavePath(snapshot.Root, snapshot.Source));
			}
			const bool dirty = info.Dirty;
			std::optional<Utils::AutosaveSnapshot>& base = state.Base;
			const bool sameDocument = base && base->ProjectFile == snapshot.ProjectFile && base->Epoch == snapshot.Epoch && base->ScenePath == snapshot.ScenePath;
			if (sameDocument && !dirty)
			{
				// A clean open scene can still be stale on disk. Keep its loaded baseline until explicit save or reload;
				// adopting external bytes here would incorrectly call later edits derived from the changed source.
				base->ProjectFingerprint = snapshot.ProjectFingerprint;
				state.Snapshot.reset();
				return {};
			}
			if (!sameDocument)
			{
				if (info.ChangedOnDisk)
				{
					// The file no longer holds what the open scene was loaded from: it cannot become this document's baseline.
					base.reset();
					state.Snapshot.reset();
					if (!dirty)
						return {};
					return MakeError(ErrorCode::Conflict, "scene '{}' changed on disk after it was opened; reload or save it to resume autosave", snapshot.ScenePath);
				}
				if (!snapshot.Source.empty())
				{
					ENGINE_TRY_ASSIGN(snapshot.SourceFingerprint, ReadCachedFingerprint(snapshot.Source));
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
				snapshot.SourceFingerprint = base->SourceFingerprint;
				snapshot.UntitledToken = base->UntitledToken;
				if (!snapshot.Source.empty())
				{
					ENGINE_TRY_ASSIGN(const Utils::AutosaveFingerprint current, ReadCachedFingerprint(snapshot.Source));
					if (current != snapshot.SourceFingerprint)
						return MakeError(ErrorCode::Conflict, "scene '{}' changed after the autosave snapshot was captured", snapshot.ScenePath);
				}
			}
			base = snapshot;
			if (!dirty)
			{
				state.Snapshot.reset();
				return {};
			}
			if (const Utils::AutosaveSnapshot* previous = state.Snapshot.get(); previous != nullptr)
			{
				if (previous->Revision == snapshot.Revision && previous->Epoch == snapshot.Epoch && previous->ScenePath == snapshot.ScenePath
					&& previous->ProjectFingerprint == snapshot.ProjectFingerprint && previous->SourceFingerprint == snapshot.SourceFingerprint)
					return {};
			}
			ENGINE_TRY_ASSIGN(snapshot.Text, SceneSerializer::SaveToString(scene));
			if (snapshot.Text.size() > Utils::MaxAutosavePayloadBytes)
				return MakeError(ErrorCode::Validation, "autosave scene exceeds the 256 MiB recovery limit");
			if (snapshot.SourceFingerprint.Exists && snapshot.SourceFingerprint.Size == snapshot.Text.size() && snapshot.SourceFingerprint.Hash == XXH64(snapshot.Text))
			{
				state.Snapshot.reset();
				return {};
			}
			state.Snapshot = CreateRef<const Utils::AutosaveSnapshot>(std::move(snapshot));
			return {};
		}

		// Installs a prepared recovery as the open scene of its source: replacing that open scene (by path, or an untitled
		// scene by its document token) and making it active, otherwise as an additional open scene. Returns its new id.
		Result<OpenSceneId> InstallRecovery(Utils::PreparedAutosaveRecovery& prepared)
		{
			EditorContext& editor = *Editor;
			OpenSceneId source = NoOpenScene;
			if (prepared.ScenePath.has_value())
			{
				source = editor.FindOpenScene(*prepared.ScenePath);
			}
			else
			{
				for (const OpenSceneId id : editor.GetOpenScenes())
				{
					const SceneState* scene = FindScene(id);
					const Result<OpenSceneInfo> info = editor.DescribeOpenScene(id);
					if (scene != nullptr && scene->Base && info && !info->Path && scene->Base->ScenePath.empty() && scene->Base->UntitledToken == prepared.Baseline.UntitledToken)
						source = id;
				}
			}
			if (source == NoOpenScene)
				return editor.AddScene(std::move(prepared.RecoveredScene), std::move(prepared.ScenePath), true);
			ENGINE_TRY(editor.ActivateScene(source));
			editor.SetScene(std::move(prepared.RecoveredScene), std::move(prepared.ScenePath), true);
			return editor.GetActiveSceneId();
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
		const std::vector<OpenSceneId> open = editor.HasProject() ? editor.GetOpenScenes() : std::vector<OpenSceneId>{};
		if (!editor.HasProject() || editor.IsReadOnly() || editor.IsDryRun() || open.empty())
		{
			state.Enabled.store(false, std::memory_order_release);
			state.WithdrawAll();
			state.ForgetPublication();
			return {};
		}
		if (!std::isfinite(state.Specification.IntervalSeconds) || state.Specification.IntervalSeconds <= 0 || state.Specification.RetainedGenerations == 0
			|| state.Specification.RetainedGenerations > Utils::MaxAutosaveRetainedGenerations)
			return MakeError(ErrorCode::Validation, "autosave requires a positive finite interval and 1 to 32 retained generations");
		// An unfinished gesture owns live preview changes. Preserve the preceding committed snapshots until it finishes.
		if (editor.GetTransaction() != nullptr)
			return {};

		// Each open scene on its own (decision 15). A scene whose key did not change keeps its outcome (or its Conflict)
		// without touching any file's bytes, path canonicalization or serialization; the project is captured once, and only
		// for a scene whose key changed.
		const Result<Utils::AutosaveFileStamp> projectFile = Utils::ReadAutosaveStamp(editor.GetProject().GetProjectFile());
		const std::optional<Utils::AutosaveFileStamp> projectStamp = projectFile ? std::optional(*projectFile) : std::nullopt;
		std::optional<Result<State::ProjectCapture>> capture;
		Status first;
		for (const OpenSceneId id : open)
		{
			const Scene* scene = editor.FindScene(id);
			Result<OpenSceneInfo> info = editor.DescribeOpenScene(id);
			ENGINE_ASSERT(scene != nullptr && info.has_value(), "Autosave: open scene {} has no document", id);
			State::SceneState& sceneState = state.GetScene(id);
			// An unfinished gesture in this scene keeps its preceding committed snapshot.
			if (scene == nullptr || !info || scene->GetChangeTracker().IsTracking())
				continue;
			std::optional<State::PublishKey> key = state.MakePublishKey(*info, projectStamp);
			if (!key || sceneState.LastKey != key)
			{
				sceneState.LastKey.reset();
				if (!capture)
					capture = state.CaptureProject();
				Status published = *capture ? state.PublishScene(sceneState, *scene, *info, **capture) : Status(std::unexpected(capture->error()));
				sceneState.LastResult = published;
				// A conflict lasts until a file or the editor state changes, which changes the key; other failures retry.
				if (key && (published || published.error().GetCode() == ErrorCode::Conflict))
					sceneState.LastKey = std::move(key);
			}
			if (!sceneState.LastResult && first)
				first = sceneState.LastResult;
		}
		ENGINE_TRY(state.UpdatePublication());
		return first;
	}

	Result<AutosaveWriteResult> Autosave::Update(double nowSeconds)
	{
		State& state = *m_State;
		state.AssertMain();
		if (!std::isfinite(nowSeconds) || nowSeconds < 0 || (state.HasClock && nowSeconds < state.LastNow))
			return MakeError(ErrorCode::InvalidArgument, "autosave clock must be finite, nonnegative and monotonic");
		state.HasClock = true;
		state.LastNow = nowSeconds;
		// A scene that cannot be published reports its failure; the other open scenes are still written when it is time.
		const Status published = Publish();
		if (!published && !state.HasPublishedWithoutError())
			return std::unexpected(published.error());
		const auto idle = [&published]() -> Result<AutosaveWriteResult>
		{
			if (!published)
				return std::unexpected(published.error());
			return AutosaveWriteResult{};
		};
		if (!state.Enabled.load(std::memory_order_acquire))
			return idle();
		if (nowSeconds - state.LastAttempt < state.Specification.IntervalSeconds)
			return idle();
		const EditorContext& editor = *state.Editor;
		if (editor.HasScene() && (editor.GetTransaction() != nullptr || editor.GetScene().GetChangeTracker().IsTracking()))
			return idle();
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
		// Every open scene that published is written; a scene that could not be published reports its failure afterwards.
		const Status published = Publish();
		if (!published && !state.HasPublishedWithoutError())
			return std::unexpected(published.error());
		uint32_t expected = 0;
		if (!state.Gate.compare_exchange_strong(expected, Utils::AutosaveWriterBit, std::memory_order_acq_rel))
			return MakeError(ErrorCode::InvalidState, "autosave is closing or another writer owns the recovery files");
		State::WriterLease lease{ state, state.ClaimSlot() };
		if (lease.Index == Utils::NoAutosaveSlot)
		{
			if (!published)
				return std::unexpected(published.error());
			return AutosaveWriteResult{};
		}
		std::vector<Ref<const Utils::AutosaveSnapshot>> snapshots;
		for (const Ref<const Utils::AutosaveSnapshot>& snapshot : state.Slots[static_cast<size_t>(lease.Index)].Snapshots)
		{
			if (state.IsPublishedWithoutError(snapshot->Epoch))
				snapshots.push_back(snapshot);
		}
		// A final snapshot of what the newest generation of a scene already holds would only evict an older one.
		const bool skipWritten = reason == AutosaveReason::FatalError || reason == AutosaveReason::Exit;
		ENGINE_TRY_ASSIGN(Utils::AutosaveGenerationWrite written, state.WriteSnapshots(snapshots, reason, skipWritten));
		if (written.Result.Written)
			Utils::SweepAutosaveGenerations(snapshots.front()->Root, written.Manifest);
		if (written.Failure.has_value())
			return std::unexpected(std::move(*written.Failure));
		if (!published)
			return std::unexpected(published.error());
		return std::move(written.Result);
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
			// Every published dirty open scene, from the claimed publication only (decision 15), with one manifest
			// publication; a scene already written as the newest generation of its scene is not written again.
			const std::vector<Ref<const Utils::AutosaveSnapshot>>& snapshots = state.Slots[static_cast<size_t>(lease.Index)].Snapshots;
			const Result<Utils::AutosaveGenerationWrite> written = state.WriteSnapshots(snapshots, AutosaveReason::FatalError, true);
			if (!written || written->Failure.has_value())
				return AutosaveFatalResult::IoFailure;
			return AutosaveFatalResult::Saved;
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

	Result<std::vector<AutosaveRecoveryInfo>> Autosave::FindRecoveries() const
	{
		m_State->AssertMain();
		EditorContext& editor = *m_State->Editor;
		if (!editor.HasProject())
			return std::vector<AutosaveRecoveryInfo>{};
		ENGINE_TRY_ASSIGN(const std::vector<Utils::AutosaveRecoveryData> recoveries, Utils::ReadAutosaveRecoveries(editor.GetProject()));
		std::vector<AutosaveRecoveryInfo> offers;
		offers.reserve(recoveries.size());
		for (const Utils::AutosaveRecoveryData& recovery : recoveries)
		{
			const Scope<Scene> scene = editor.CreateScene(recovery.Info.SceneName);
			LoadReport report;
			ENGINE_TRY(SceneSerializer::LoadFromString(*scene, recovery.SceneText, {}, report));
			if (scene->GetName() != recovery.Info.SceneName)
				return MakeError(ErrorCode::Validation, "autosave scene identity disagrees with its payload");
			offers.push_back(recovery.Info);
		}
		return offers;
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
		// The test runner owns the edit scene for the duration of its run; EditorContext::SetScene would ignore the install.
		if (editor.GetPlay().IsTestRunActive())
			return MakeError(ErrorCode::InvalidState, "a test run owns the editor; recover after it finishes");
		if (editor.HasScene() && editor.GetScene().GetChangeTracker().IsTracking())
			return MakeError(ErrorCode::InvalidState, "finish the active edit before recovering");
		uint32_t expected = state.Gate.load(std::memory_order_acquire);
		if ((expected & Utils::AutosaveWriterBit) != 0 || !state.Gate.compare_exchange_strong(expected, expected | Utils::AutosaveWriterBit, std::memory_order_acq_rel))
			return MakeError(ErrorCode::InvalidState, "another autosave writer owns the recovery files");
		State::WriterLease lease{ state };
		ENGINE_TRY_ASSIGN(auto prepared, Utils::PrepareAutosaveRecovery(editor, editor.GetProject(), recovery, state.Specification.ReadObserver));
		ENGINE_TRY_ASSIGN(const OpenSceneId id, state.InstallRecovery(prepared));
		state.AdoptRecovery(std::move(prepared), id);
		state.LastWritten.clear();
		return {};
	}

	Status Autosave::OpenRecoveredProject(Scope<LoadedProject> project, const AutosaveRecoveryInfo& recovery)
	{
		return OpenRecoveredProject(std::move(project), std::span<const AutosaveRecoveryInfo>(&recovery, 1));
	}

	Status Autosave::OpenRecoveredProject(Scope<LoadedProject> project, std::span<const AutosaveRecoveryInfo> recoveries)
	{
		State& state = *m_State;
		state.AssertMain();
		EditorContext& editor = *state.Editor;
		if (project == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "recovery needs an already-locked project");
		if (project->IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "read-only projects cannot adopt a recovery");
		const bool bound = std::ranges::any_of(state.Scenes, [](const State::SceneState& scene)
		{
			return scene.Base.has_value();
		});
		if (editor.HasProject() || editor.IsDryRun() || editor.GetTransaction() != nullptr || state.Closing || bound || state.ProjectIdentity || state.Published.load(std::memory_order_acquire) != Utils::NoAutosaveSlot || state.Enabled.load(std::memory_order_acquire))
			return MakeError(ErrorCode::InvalidState, "launcher recovery requires a quiescent service after its prior project was reset");
		if (recoveries.empty())
			return MakeError(ErrorCode::InvalidArgument, "launcher recovery needs at least one recovery offer");
		if (recoveries.size() > MaxOpenScenes)
			return MakeError(ErrorCode::InvalidArgument, "launcher recovery takes at most {} offers, the scenes the editor holds open", MaxOpenScenes);
		uint32_t expected = state.Gate.load(std::memory_order_acquire);
		if ((expected & Utils::AutosaveWriterBit) != 0 || !state.Gate.compare_exchange_strong(expected, expected | Utils::AutosaveWriterBit, std::memory_order_acq_rel))
			return MakeError(ErrorCode::InvalidState, "another autosave writer owns the recovery files");
		// The lease also prevents a fatal claim from observing partial adoption. Closed admission stays closed until
		// the host's later Publish, and all expected failures precede the scene/baseline installation below.
		State::WriterLease lease{ state };
		std::vector<Utils::PreparedAutosaveRecovery> prepared;
		prepared.reserve(recoveries.size());
		for (const AutosaveRecoveryInfo& recovery : recoveries)
		{
			ENGINE_TRY_ASSIGN(Utils::PreparedAutosaveRecovery scene, Utils::PrepareAutosaveRecovery(editor, *project, recovery, state.Specification.ReadObserver));
			const bool repeated = std::ranges::any_of(prepared, [&scene](const Utils::PreparedAutosaveRecovery& other)
			{
				return other.SceneKey == scene.SceneKey;
			});
			if (repeated)
			{
				const std::string_view name = scene.Baseline.ScenePath.empty() ? std::string_view(scene.Baseline.SceneName) : std::string_view(scene.Baseline.ScenePath);
				return MakeError(ErrorCode::InvalidArgument, "two recovery offers name the scene '{}'", name);
			}
			prepared.push_back(std::move(scene));
		}
		ENGINE_TRY(editor.OpenProject(std::move(project)));
		// Every scene opens dirty with an empty history, in the given order; the first is active. None of these can fail:
		// the project was just opened with no scene, and the offers are fewer than MaxOpenScenes and name distinct sources.
		std::vector<OpenSceneId> ids;
		for (Utils::PreparedAutosaveRecovery& scene : prepared)
		{
			const Result<OpenSceneId> added = editor.AddScene(std::move(scene.RecoveredScene), scene.ScenePath, true);
			ENGINE_VERIFY(added.has_value(), "installing a recovered scene into a just-opened project failed: {}", added ? std::string() : added.error().ToString());
			ids.push_back(*added);
		}
		const Status activated = editor.ActivateScene(ids.front());
		ENGINE_VERIFY(activated.has_value(), "activating the first recovered scene failed: {}", activated ? std::string() : activated.error().ToString());
		for (size_t index = 0; index < prepared.size(); ++index)
			state.AdoptRecovery(std::move(prepared[index]), ids[index]);
		state.LastWritten.clear();
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
		if (!editor.HasProject() || !editor.HasScene() || !editor.GetScenePath() || editor.IsSceneDirty())
			return {};
		// The scene just saved: the addressed one (decision 15).
		const OpenSceneId saved = editor.GetAddressedSceneId();
		State::SceneState* scene = state.FindScene(saved);
		if (scene == nullptr || !scene->Base)
			return {};
		uint32_t expected = 0;
		if (!state.Gate.compare_exchange_strong(expected, Utils::AutosaveWriterBit, std::memory_order_acq_rel))
			return MakeError(ErrorCode::InvalidState, "autosave is closing or another recovery writer is active");
		State::WriterLease lease{ state };
		const Utils::AutosaveReadObserver& observe = state.Specification.ReadObserver;
		ENGINE_TRY_ASSIGN(const std::filesystem::path projectFile, Utils::CanonicalAutosaveProject(editor.GetProject()));
		const std::filesystem::path root = projectFile.parent_path();
		const std::string savedPath(editor.GetScenePath()->GetPath());
		const std::filesystem::path savedFile = root / FileSystem::PathFromUtf8(savedPath);
		ENGINE_TRY(Utils::CheckAutosavePath(root, savedFile));
		ENGINE_TRY_ASSIGN(const std::string actual, Utils::ReadAutosaveText(savedFile, Utils::MaxAutosavePayloadBytes, observe));
		ENGINE_TRY_ASSIGN(const std::string current, SceneSerializer::SaveToString(editor.GetScene()));
		if (actual != current)
			return MakeError(ErrorCode::Conflict, "the source file does not contain the explicitly saved scene");
		const Utils::AutosaveSnapshot& base = *scene->Base;
		if (base.ProjectFile != projectFile || base.Epoch != saved)
			return {};
		const auto forget = [&state, scene]()
		{
			scene->Snapshot.reset();
			scene->LastKey.reset();
			scene->LastResult = {};
			scene->Base.reset();
			static_cast<void>(state.UpdatePublication());
		};
		ENGINE_TRY_ASSIGN(Utils::AutosaveManifest manifest, Utils::ReadAutosaveManifest(root, observe));
		std::vector<Utils::AutosaveManifestEntry> removed;
		std::vector<Utils::AutosaveManifestEntry> kept;
		for (const Utils::AutosaveManifestEntry& entry : manifest.Generations)
		{
			// A generation that cannot be read or belongs to another canonical project (a moved or renamed one) cannot
			// match the saved source: it is kept for its own owner or for retention, never a reason to fail the save.
			const Result<Utils::AutosaveSnapshot> snapshot = Utils::ReadAutosaveGeneration(projectFile, entry, observe, false);
			const bool sameSource = snapshot && snapshot->ScenePath == savedPath && base.ScenePath == savedPath && snapshot->SourceFingerprint == base.SourceFingerprint;
			const bool sameUntitled = snapshot && snapshot->ScenePath.empty() && base.ScenePath.empty() && !base.UntitledToken.empty() && snapshot->UntitledToken == base.UntitledToken;
			(sameSource || sameUntitled ? removed : kept).push_back(entry);
		}
		if (removed.empty())
		{
			forget();
			return {};
		}
		manifest.Generations = std::move(kept);
		ENGINE_TRY_ASSIGN(const std::string text, Utils::AutosaveManifestText(manifest));
		const std::filesystem::path manifestPath = root / "Library" / "Autosave" / "Manifest.json";
		ENGINE_TRY(Utils::CheckAutosavePath(root, manifestPath));
		ENGINE_TRY(Utils::WriteAutosaveBytes(state.Specification, manifestPath, text));
		state.ForgetRetiredWrites(manifest);
		for (const auto& entry : removed)
			Utils::RemoveRetiredAutosaveGeneration(root, manifestPath.parent_path() / FileSystem::PathFromUtf8(entry.Generation));
		Utils::SweepAutosaveGenerations(root, manifest);
		forget();
		return {};
	}

	bool Autosave::Reset()
	{
		State& state = *m_State;
		state.AssertMain();
		state.Closing = true;
		state.Gate.fetch_or(Utils::AutosaveClosedBit, std::memory_order_acq_rel);
		state.Enabled.store(false, std::memory_order_release);
		state.WithdrawAll();
		state.ForgetPublication();
		if ((state.Gate.load(std::memory_order_acquire) & Utils::AutosaveWriterBit) != 0)
			return false;
		for (State::Slot& slot : state.Slots)
		{
			slot.Status.store(State::SlotState::Free, std::memory_order_relaxed);
			slot.Snapshots.clear();
		}
		state.Scenes.clear();
		state.ProjectIdentity.reset();
		state.ProjectFingerprint.reset();
		state.Fingerprints.clear();
		state.LastWritten.clear();
		state.GenerationKeys.clear();
		state.HasClock = false;
		state.LastNow = 0;
		state.LastAttempt = 0;
		state.Closing = false;
		state.CanReopen = true;
		return true;
	}

}
