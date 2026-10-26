#pragma once

#include "EditorCore/Autosave/Autosave.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class LoadedProject;

	namespace Utils {

		// Internal, CPU-only preflight shared by Autosave and project.open. No mounts or live editor mutation.
		struct AutosaveRecoveryData
		{
			AutosaveRecoveryInfo Info{};
			std::string SceneText{};
		};

		[[nodiscard]] Result<std::optional<AutosaveRecoveryData>> ReadAutosaveRecovery(const LoadedProject& project,
			std::string_view generation = {});

		// M20 (Docs/Decisions/0031-m20-contract.md decision 15): ReadAutosaveRecovery's offer for every scene, one per scene
		// source (its path, or its untitled document's token), each the newest generation of that scene that differs from its
		// source, ordered by the manifest sequence of those generations, oldest first (Autosave::FindRecoveries). Reads, newest
		// first, the metadata of every generation until each of its scenes has its offer, and the whole generation of each
		// scene still without one. Errors: as ReadAutosaveRecovery's, for any generation it reads, but for a stale generation
		// (Conflict from the project or source verification): its scene has no offer and is logged at Warn, and the conflict
		// is returned only when no scene has an offer.
		[[nodiscard]] Result<std::vector<AutosaveRecoveryData>> ReadAutosaveRecoveries(const LoadedProject& project);

		// One generation's documents: the text of its Metadata.json and of its Scene.json.
		struct AutosaveGenerationDocuments
		{
			std::string Metadata{};
			std::string Scene{};
		};

		// The documents of a project's autosave held in memory: Library/Autosave/Manifest.json (nullopt when it does not
		// exist) and each generation's documents by generation ("g-<16 hex digits>").
		struct AutosaveRecoveryDocuments
		{
			std::optional<std::string> Manifest{};
			std::map<std::string, AutosaveGenerationDocuments, std::less<>> Generations{};
		};

		// ReadAutosaveRecovery's reading and checks of the documents, from memory (the in-memory entry point of the
		// AutosaveRecovery fuzz case, Docs/Decisions/0024-m15-contract.md decision 13): the documents' size limits and UTF-8,
		// the manifest, then each generation of the manifest, newest first, with its metadata and its payload's size and
		// checksum, and the offer of the newest generation that differs from its source's fingerprint. What needs the files
		// themselves (`projectFile` canonical, no linked or redirected directory, the project and source files still
		// matching their fingerprints) is ReadAutosaveRecovery's alone; `projectFile` is taken as the canonical project
		// file. Errors: as ReadAutosaveRecovery; NotFound for a generation the manifest lists without documents.
		[[nodiscard]] Result<std::optional<AutosaveRecoveryData>> ParseAutosaveRecovery(const std::filesystem::path& projectFile,
			const AutosaveRecoveryDocuments& documents, std::string_view generation = {});

	}

}
