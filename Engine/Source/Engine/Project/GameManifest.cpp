#include "EnginePCH.h"
#include "Engine/Project/GameManifest.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Platform/Paths.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <utility>

// Reading is strict and stops at the first problem, which is reported as a located error carrying one issue at the same
// JSON pointer, so a caller finds the offending member from either. Window and Simulation are read through the reflection
// registry with the project loader's rules (StructInfo::FromJson; Strict, so an unknown member is an error), which
// reports every bad field of the section as an issue.

namespace Engine {

	namespace Utils {

		// The manifest's keys, in the order SaveToString writes them (§14.1).
		constexpr std::array<std::string_view, 9> ManifestKeys = { "Format", "Version", "Name", "EngineVersion", "StartScene", "Paks", "Window",
			"Simulation", "Testing" };
		constexpr std::array<std::string_view, 2> ManifestPakKeys = { "Path", "XXH64" };
		// The file names of the two paks, in their order (§14.1).
		constexpr std::array<std::string_view, 2> ManifestPakFileNames = { "Engine.pak", "Game.pak" };
		// The oldest readable manifest version (there has been no other yet).
		constexpr uint32_t MinimumManifestVersion = 1;
		constexpr size_t HashDigits = 16;

		// An error of `code` located at `pointer`, with one issue there.
		[[nodiscard]] static Error MakeManifestError(ErrorCode code, std::string_view pointer, std::string message, std::string hint = {})
		{
			ErrorLocation location;
			location.JsonPointer = std::string(pointer);
			ErrorIssue issue;
			issue.JsonPointer = std::string(pointer);
			issue.Message = message;
			issue.Hint = hint;
			return Error(code, std::move(message)).WithHint(std::move(hint)).WithLocation(std::move(location)).WithIssue(std::move(issue));
		}

		// `error`, given an issue at its location's pointer when it has a pointer and no issue (JsonReader's typed reads
		// locate their errors without issues).
		[[nodiscard]] static Error WithLocatedIssue(Error error)
		{
			if (!error.GetIssues().empty() || !error.GetLocation().JsonPointer.has_value())
				return error;
			ErrorIssue issue;
			issue.JsonPointer = *error.GetLocation().JsonPointer;
			issue.Message = error.GetMessageText();
			issue.Hint = error.GetHint();
			return std::move(error).WithIssue(std::move(issue));
		}

		// The registry of the project settings types the manifest embeds. The serializer needs no EngineContext (the Runtime
		// reads the manifest before the ProcessContext exists), so it builds its own, which is cheap: a few structs.
		[[nodiscard]] static Scope<TypeRegistry> CreateSettingsRegistry()
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			RegisterProjectSettingsTypes(*registry);
			registry->Freeze();
			return registry;
		}

		template<typename T>
		[[nodiscard]] static const StructInfo& GetSettingsType(const TypeRegistry& registry)
		{
			const StructInfo* type = registry.FindStruct<T>();
			ENGINE_CORE_VERIFY(type != nullptr, "RegisterProjectSettingsTypes registers every settings section");
			return *type;
		}

		[[nodiscard]] static std::string FormatHash(uint64_t hash)
		{
			return std::format("{:016x}", hash);
		}

		// 16 lowercase hex digits (§4.8: 64-bit values are never JSON numbers).
		[[nodiscard]] static Result<uint64_t> ReadHash(const JsonReader& reader)
		{
			ENGINE_TRY_ASSIGN(const std::string text, reader.ReadString());
			const bool lowercaseHex = std::all_of(text.begin(), text.end(), [](char character)
			{
				return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
			});
			uint64_t value = 0;
			const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
			if (text.size() != HashDigits || !lowercaseHex || parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
			{
				return std::unexpected(MakeManifestError(ErrorCode::Validation, reader.GetPointer(),
					std::format("expected 16 lowercase hex digits, got '{}'", text)));
			}
			return value;
		}

		// Validation at the first member of `reader` (an object) that is not in `keys`.
		[[nodiscard]] static Status ExpectKnownMembers(const JsonReader& reader, std::span<const std::string_view> keys)
		{
			ENGINE_TRY_ASSIGN(const std::vector<std::string> unknown, reader.FindUnknownMembers(keys));
			if (unknown.empty())
				return {};
			return std::unexpected(MakeManifestError(ErrorCode::Validation, JsonReader::AppendPointer(reader.GetPointer(), unknown.front()),
				std::format("unknown key '{}' in the game manifest", unknown.front())));
		}

		// Window or Simulation, read through the registry with the project loader's rules (missing fields keep their
		// defaults; ranges and validators as in a .eproj), strictly.
		template<typename T>
		[[nodiscard]] static Status ReadSettingsSection(const TypeRegistry& registry, const JsonReader& root, std::string_view key, T& settings)
		{
			ENGINE_TRY_ASSIGN(const JsonReader section, root.GetMember(key));
			ENGINE_TRY(section.ExpectType(JsonType::Object));
			ReadContext context;
			context.Strict = true;
			return GetSettingsType<T>(registry).FromJson(&settings, section, context);
		}

		[[nodiscard]] static Result<GameManifestPak> ReadPak(const JsonReader& reader, size_t index)
		{
			ENGINE_TRY(reader.ExpectType(JsonType::Object));
			ENGINE_TRY(ExpectKnownMembers(reader, ManifestPakKeys));
			GameManifestPak pak;
			ENGINE_TRY_ASSIGN(const JsonReader path, reader.GetMember("Path"));
			ENGINE_TRY_ASSIGN(pak.Path, path.ReadString());
			if (pak.Path.empty())
				return std::unexpected(MakeManifestError(ErrorCode::Validation, path.GetPointer(), "the pak path is empty"));
			if (const Status valid = VfsPath::ValidateRelativePath(pak.Path); !valid)
			{
				return std::unexpected(MakeManifestError(ErrorCode::Validation, path.GetPointer(),
					std::format("'{}' is not a pak path relative to the manifest: {}", pak.Path, valid.error().GetMessageText()),
					"write a path such as \"Data/Game.pak\", with forward slashes"));
			}
			const std::string_view expected = ManifestPakFileNames[index];
			const size_t slash = pak.Path.rfind('/');
			const std::string_view fileName = slash == std::string::npos ? std::string_view(pak.Path) : std::string_view(pak.Path).substr(slash + 1);
			if (fileName != expected)
			{
				return std::unexpected(MakeManifestError(ErrorCode::Validation, path.GetPointer(),
					std::format("pak {} must be {}, not '{}'", index, expected, pak.Path), "list Data/Engine.pak, then Data/Game.pak"));
			}
			ENGINE_TRY_ASSIGN(const JsonReader hash, reader.GetMember("XXH64"));
			ENGINE_TRY_ASSIGN(pak.Hash, ReadHash(hash));
			return pak;
		}

		[[nodiscard]] static Result<GameManifest> ReadManifest(const Json& document)
		{
			const JsonReader root(document);
			ENGINE_TRY(root.ReadFormatHeader(GameManifest::FormatName, MinimumManifestVersion, GameManifest::CurrentVersion));
			ENGINE_TRY(ExpectKnownMembers(root, ManifestKeys));

			GameManifest manifest;
			ENGINE_TRY_ASSIGN(const JsonReader name, root.GetMember("Name"));
			ENGINE_TRY_ASSIGN(manifest.Name, name.ReadString());
			if (const Status valid = Paths::ValidateAppName(manifest.Name); !valid)
			{
				return std::unexpected(MakeManifestError(ErrorCode::Validation, name.GetPointer(),
					std::format("'{}' is not a valid game name: {}", manifest.Name, valid.error().GetMessageText()),
					"the name is the executable's and the user-data folder's: use letters, digits, spaces, '-' and '_'"));
			}

			ENGINE_TRY_ASSIGN(const JsonReader engineVersion, root.GetMember("EngineVersion"));
			ENGINE_TRY_ASSIGN(manifest.EngineVersion, engineVersion.ReadString());
			if (manifest.EngineVersion.empty())
				return std::unexpected(MakeManifestError(ErrorCode::Validation, engineVersion.GetPointer(), "the engine version is empty"));

			ENGINE_TRY_ASSIGN(const JsonReader startScene, root.GetMember("StartScene"));
			ENGINE_TRY_ASSIGN(const uint64_t startSceneValue, ReadHash(startScene));
			manifest.StartScene = UUID(startSceneValue);
			if (!manifest.StartScene.IsValid())
				return std::unexpected(MakeManifestError(ErrorCode::Validation, startScene.GetPointer(), "the start scene is the null handle"));

			ENGINE_TRY_ASSIGN(const JsonReader paks, root.GetMember("Paks"));
			ENGINE_TRY_ASSIGN(const size_t pakCount, paks.GetArraySize());
			if (pakCount != ManifestPakFileNames.size())
			{
				return std::unexpected(MakeManifestError(ErrorCode::Validation, paks.GetPointer(),
					std::format("Paks must list exactly {} paks, Engine.pak then Game.pak; it lists {}", ManifestPakFileNames.size(), pakCount)));
			}
			for (size_t index = 0; index < pakCount; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader pak, paks.GetElement(index));
				ENGINE_TRY_ASSIGN(GameManifestPak read, ReadPak(pak, index));
				manifest.Paks.push_back(std::move(read));
			}

			const Scope<TypeRegistry> registry = CreateSettingsRegistry();
			ENGINE_TRY(ReadSettingsSection(*registry, root, "Window", manifest.Window));
			ENGINE_TRY(ReadSettingsSection(*registry, root, "Simulation", manifest.Simulation));

			ENGINE_TRY_ASSIGN(const JsonReader testing, root.GetMember("Testing"));
			ENGINE_TRY_ASSIGN(manifest.Testing, testing.ReadBool());
			if (manifest.Testing)
			{
				// Nothing is accepted and ignored: this Runtime has no testing runner yet (Docs/Decisions/0012-m7-decisions.md
				// decision 16).
				return std::unexpected(MakeManifestError(ErrorCode::Unsupported, testing.GetPointer(), "testing exports are not supported yet",
					"export the game with testing: false"));
			}
			return manifest;
		}

		[[nodiscard]] static Result<Json> WriteManifest(const GameManifest& manifest)
		{
			const Scope<TypeRegistry> registry = CreateSettingsRegistry();
			Json document = Json::object();
			document["Format"] = std::string(GameManifest::FormatName);
			document["Version"] = GameManifest::CurrentVersion;
			document["Name"] = manifest.Name;
			document["EngineVersion"] = manifest.EngineVersion;
			document["StartScene"] = manifest.StartScene.ToString();
			Json paks = Json::array();
			for (const GameManifestPak& pak : manifest.Paks)
			{
				Json entry = Json::object();
				entry["Path"] = pak.Path;
				entry["XXH64"] = FormatHash(pak.Hash);
				paks.push_back(std::move(entry));
			}
			document["Paks"] = std::move(paks);
			ENGINE_TRY_ASSIGN(Json window, GetSettingsType<WindowSettings>(*registry).ToJson(&manifest.Window));
			document["Window"] = std::move(window);
			ENGINE_TRY_ASSIGN(Json simulation, GetSettingsType<SimulationSettings>(*registry).ToJson(&manifest.Simulation));
			document["Simulation"] = std::move(simulation);
			document["Testing"] = manifest.Testing;
			return document;
		}

	}

	Result<std::string> GameManifestSerializer::SaveToString(const GameManifest& manifest)
	{
		ENGINE_TRY_ASSIGN(const Json document, Utils::WriteManifest(manifest));
		ENGINE_TRY_ASSIGN(std::string text, JsonWriter::Write(document, JsonStyle::Pretty));
		// What a reader would refuse is never written, so a written manifest always reads back equal.
		const Result<GameManifest> read = Utils::ReadManifest(document);
		if (!read)
			return std::unexpected(Utils::WithLocatedIssue(read.error()));
		return text;
	}

	Result<GameManifest> GameManifestSerializer::LoadFromString(std::string_view text)
	{
		Result<Json> document = JsonReader::Parse(text);
		if (!document)
			return std::unexpected(std::move(document).error());
		Result<GameManifest> manifest = Utils::ReadManifest(*document);
		if (!manifest)
			return std::unexpected(Utils::WithLocatedIssue(std::move(manifest).error()));
		return manifest;
	}

	Result<GameManifest> GameManifestSerializer::LoadFromFile(const std::filesystem::path& path)
	{
		const std::string file = FileSystem::PathToUtf8(path);
		Result<std::string> text = FileSystem::ReadText(path);
		if (!text)
		{
			if (text.error().GetCode() == ErrorCode::NotFound)
				return MakeError(ErrorCode::NotFound, "the game manifest '{}' does not exist", file);
			ErrorLocation location;
			location.File = file;
			return std::unexpected(std::move(text).error().WithLocation(std::move(location)));
		}
		Result<GameManifest> manifest = LoadFromString(*text);
		if (!manifest)
		{
			ErrorLocation location;
			location.File = file;
			return std::unexpected(std::move(manifest).error().WithLocation(std::move(location)));
		}
		return manifest;
	}

}
