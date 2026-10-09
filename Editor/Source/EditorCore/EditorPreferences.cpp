#include "EditorPCH.h"
#include "EditorCore/EditorPreferences.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static Result<Json> ReadUtilityPreferencesDocument(const VirtualFileSystem& vfs)
		{
			ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Parse("user://Editor.json"));
			Result<std::string> text = vfs.ReadText(path);
			if (!text)
			{
				if (text.error().GetCode() == ErrorCode::NotFound)
					return Json{ { "Format", "EditorPreferences" }, { "Version", 1 }, { "RecentProjects", Json::array() } };
				return std::unexpected(std::move(text).error());
			}
			ENGINE_TRY_ASSIGN(Json document, JsonReader::Parse(*text));
			const JsonReader reader(document);
			ENGINE_TRY(reader.ReadFormatHeader("EditorPreferences", 1, 1));
			if (const auto recent = reader.FindMember("RecentProjects"))
			{
				ENGINE_TRY_ASSIGN(const size_t count, recent->GetArraySize());
				for (size_t index = 0; index < count; ++index)
				{
					ENGINE_TRY_ASSIGN(const JsonReader entry, recent->GetElement(index));
					ENGINE_TRY(entry.ReadString());
				}
			}
			return document;
		}

		[[nodiscard]] static Result<EditorPreferences> DecodeUtilityPreferences(const Json& document)
		{
			EditorPreferences preferences;
			const JsonReader reader(document);
			if (const auto allowed = reader.FindMember("AllowAiAutomation"))
			{
				ENGINE_TRY_ASSIGN(preferences.AllowAiAutomation, allowed->ReadBool());
			}
			if (const auto executable = reader.FindMember("ExternalEditorExecutable"))
			{
				ENGINE_TRY_ASSIGN(preferences.ExternalEditorExecutable, executable->ReadString());
			}
			if (const auto arguments = reader.FindMember("ExternalEditorArguments"))
			{
				ENGINE_TRY_ASSIGN(const size_t count, arguments->GetArraySize());
				for (size_t index = 0; index < count; ++index)
				{
					ENGINE_TRY_ASSIGN(const JsonReader argument, arguments->GetElement(index));
					ENGINE_TRY_ASSIGN(std::string value, argument.ReadString());
					preferences.ExternalEditorArguments.push_back(std::move(value));
				}
			}
			return preferences;
		}

	}

	Result<EditorPreferences> ReadEditorPreferences(const VirtualFileSystem& vfs)
	{
		ENGINE_TRY_ASSIGN(const Json document, Utils::ReadUtilityPreferencesDocument(vfs));
		return Utils::DecodeUtilityPreferences(document);
	}

	Status WriteEditorPreferences(VirtualFileSystem& vfs, const EditorPreferences& preferences)
	{
		if (!vfs.IsMounted("user"))
			return MakeError(ErrorCode::InvalidState, "editor preferences require a user:// mount");
		ENGINE_TRY_ASSIGN(Json document, Utils::ReadUtilityPreferencesDocument(vfs));
		ENGINE_TRY(Utils::DecodeUtilityPreferences(document));
		// Validate new strings before using JSON's lossless double serializer. JsonWriter rounds floats to float32,
		// which would silently alter unknown preference members belonging to a future editor version.
		ENGINE_TRY(JsonWriter::Write(Json{ { "Executable", preferences.ExternalEditorExecutable }, { "Arguments", preferences.ExternalEditorArguments } }));
		document["AllowAiAutomation"] = preferences.AllowAiAutomation;
		document["ExternalEditorExecutable"] = preferences.ExternalEditorExecutable;
		document["ExternalEditorArguments"] = preferences.ExternalEditorArguments;
		const std::string text = document.dump(1, '\t') + '\n';
		ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Parse("user://Editor.json"));
		return vfs.WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size())));
	}

	Result<ProcessSpecification> BuildEditorSourceProcessSpecification(const EditorPreferences& preferences,
		const std::filesystem::path& path, uint32_t line)
	{
		if (preferences.ExternalEditorExecutable.empty())
			return MakeError(ErrorCode::InvalidState, "configure an external source editor in preferences");
		const std::string source = FileSystem::PathToUtf8(path);
		const auto valid = [](std::string_view value)
		{
			return IsValidUtf8(value) && value.find('\0') == std::string_view::npos;
		};
		if (source.empty() || !valid(source) || !valid(preferences.ExternalEditorExecutable))
			return MakeError(ErrorCode::InvalidArgument, "source path and editor executable must be nonempty UTF-8 without NUL");
		ProcessSpecification process;
		process.Executable = FileSystem::PathFromUtf8(preferences.ExternalEditorExecutable);
		const std::string lineText = std::to_string(line == 0 ? 1 : line);
		if (preferences.ExternalEditorArguments.empty())
		{
			process.Arguments.push_back(source);
			return process;
		}
		bool hasPath = false;
		for (const std::string& argument : preferences.ExternalEditorArguments)
		{
			if (!valid(argument))
				return MakeError(ErrorCode::InvalidArgument, "source editor arguments must be UTF-8 without NUL");
			std::string expanded;
			for (size_t offset = 0; offset < argument.size();)
			{
				const std::string_view remaining = std::string_view(argument).substr(offset);
				if (remaining.starts_with("{path}"))
				{
					expanded += source;
					offset += 6;
					hasPath = true;
				}
				else if (remaining.starts_with("{line}"))
				{
					expanded += lineText;
					offset += 6;
				}
				else
					expanded += argument[offset++];
			}
			process.Arguments.push_back(std::move(expanded));
		}
		if (!hasPath)
			return MakeError(ErrorCode::InvalidArgument, "source editor arguments need a {{path}} placeholder");
		return process;
	}

}
