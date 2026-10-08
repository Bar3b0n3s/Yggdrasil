#include "EditorPCH.h"
#include "EditorCore/Automation/ObserveMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Engine {

	namespace Utils {

		constexpr std::string_view SkillsTopicPrefix = "skills/";
		constexpr std::string_view ReferenceTopicPrefix = "reference/";
		constexpr std::string_view SkillsDirectory = ".claude/skills";
		constexpr std::string_view SkillFileName = "SKILL.md";
		constexpr std::string_view ReferenceDirectory = "Docs/Reference";

		// A topic name segment: letters, digits, '-' and '_' only, so a topic can never name a path outside its directory.
		static bool IsTopicSegment(std::string_view name)
		{
			return !name.empty() && std::all_of(name.begin(), name.end(), [](char character)
			{
				return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
					|| character == '-' || character == '_';
			});
		}

		// The value of the "description:" line of a SKILL.md front matter; empty when there is none.
		static std::string ReadSkillDescription(std::string_view text)
		{
			if (!text.starts_with("---"))
				return {};
			size_t position = text.find('\n');
			while (position != std::string_view::npos && position + 1 < text.size())
			{
				const size_t start = position + 1;
				const size_t end = text.find('\n', start);
				std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
				if (line.ends_with('\r'))
					line.remove_suffix(1);
				if (line == "---")
					return {};
				constexpr std::string_view Key = "description:";
				if (line.starts_with(Key))
				{
					std::string_view value = line.substr(Key.size());
					while (!value.empty() && value.front() == ' ')
						value.remove_prefix(1);
					return std::string(value);
				}
				position = end;
			}
			return {};
		}

		// The text of a Markdown document's first "# " heading; empty when there is none.
		static std::string ReadFirstHeading(std::string_view text)
		{
			size_t start = 0;
			while (start < text.size())
			{
				const size_t end = text.find('\n', start);
				std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
				if (line.ends_with('\r'))
					line.remove_suffix(1);
				if (line.starts_with("# "))
					return std::string(line.substr(2));
				if (end == std::string_view::npos)
					break;
				start = end + 1;
			}
			return {};
		}

		// The file of a topic, repository-relative ("skills/build-and-test" -> ".claude/skills/build-and-test/SKILL.md"); empty
		// for a name that is not a topic.
		static std::string GetTopicPath(std::string_view topic)
		{
			if (topic.starts_with(SkillsTopicPrefix))
			{
				const std::string_view name = topic.substr(SkillsTopicPrefix.size());
				return IsTopicSegment(name) ? std::format("{}/{}/{}", SkillsDirectory, name, SkillFileName) : std::string();
			}
			if (topic.starts_with(ReferenceTopicPrefix))
			{
				const std::string_view name = topic.substr(ReferenceTopicPrefix.size());
				return IsTopicSegment(name) ? std::format("{}/{}.md", ReferenceDirectory, name) : std::string();
			}
			return {};
		}

		// Every topic under `docsRoot`, sorted by name. Missing directories give no topics.
		static Result<std::vector<DocsTopic>> ListTopics(const std::filesystem::path& docsRoot)
		{
			std::vector<DocsTopic> topics;
			const Result<std::vector<std::filesystem::path>> skills = FileSystem::ListDirectory(docsRoot / FileSystem::PathFromUtf8(SkillsDirectory));
			if (skills.has_value())
			{
				for (const std::filesystem::path& directory : *skills)
				{
					const std::string name = FileSystem::PathToUtf8(directory.filename());
					const Result<std::string> text = FileSystem::ReadText(directory / FileSystem::PathFromUtf8(SkillFileName));
					if (!IsTopicSegment(name) || !text)
						continue;
					topics.push_back(DocsTopic{ std::format("{}{}", SkillsTopicPrefix, name), ReadSkillDescription(*text) });
				}
			}
			else if (skills.error().GetCode() != ErrorCode::NotFound)
			{
				return std::unexpected(ToEditorFileError(skills.error()));
			}

			const Result<std::vector<std::filesystem::path>> references = FileSystem::ListDirectory(docsRoot / FileSystem::PathFromUtf8(ReferenceDirectory));
			if (references.has_value())
			{
				for (const std::filesystem::path& file : *references)
				{
					const std::filesystem::path extension = file.extension();
					const std::string stem = FileSystem::PathToUtf8(file.stem());
					if (extension != std::filesystem::path(".md") || !IsTopicSegment(stem))
						continue;
					const Result<std::string> text = FileSystem::ReadText(file);
					if (!text)
						continue;
					topics.push_back(DocsTopic{ std::format("{}{}", ReferenceTopicPrefix, stem), ReadFirstHeading(*text) });
				}
			}
			else if (references.error().GetCode() != ErrorCode::NotFound)
			{
				return std::unexpected(ToEditorFileError(references.error()));
			}

			std::sort(topics.begin(), topics.end(), [](const DocsTopic& left, const DocsTopic& right)
			{
				return left.Name < right.Name;
			});
			return topics;
		}

	}

	namespace Automation {

		Result<DocsGetResult> DocsGet(EditorMethodContext& context, const DocsGetParams& params)
		{
			const std::filesystem::path& docsRoot = context.GetServer().GetSpecification().DocsRoot;
			if (docsRoot.empty())
				return MakeError(ErrorCode::Unsupported, "docs.get needs the repository root, which this editor was not given");

			DocsGetResult result;
			if (params.Topic.empty())
			{
				ENGINE_TRY_ASSIGN(result.Topics, Utils::ListTopics(docsRoot));
				return result;
			}

			const std::string path = Utils::GetTopicPath(params.Topic);
			if (!path.empty())
			{
				Result<std::string> text = FileSystem::ReadText(docsRoot / FileSystem::PathFromUtf8(path));
				// The same spelling on every host: a topic that matches a file only ignoring case is not that topic.
				if (text.has_value() && FileSystem::VerifyCase(docsRoot, path).has_value())
				{
					result.Topic = params.Topic;
					result.Content = std::move(*text);
					result.Path = path;
					return result;
				}
				if (!text && text.error().GetCode() != ErrorCode::NotFound)
					return std::unexpected(Utils::ToEditorFileError(text.error()));
			}

			ENGINE_TRY_ASSIGN(const std::vector<DocsTopic> topics, Utils::ListTopics(docsRoot));
			std::vector<std::string> names;
			for (const DocsTopic& topic : topics)
				names.push_back(topic.Name);
			const std::vector<std::string> suggestions = FuzzySuggest(params.Topic, std::span<const std::string>(names));
			return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/topic", std::format("no topic '{}'", params.Topic),
				suggestions.empty() ? std::string("docs.get {} lists the topics") : MakeDidYouMeanHint(suggestions)));
		}

	}

	void RegisterEditorObserveMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<DocsGetParams>("DocsGetParams", "The params of docs.get.")
			.Field("topic", &DocsGetParams::Topic, "The topic, such as \"skills/game-building\"; empty for the list of topics.");

		registry.Struct<DocsTopic>("DocsTopic", "One documentation topic.")
			.Field("name", &DocsTopic::Name, "The topic's name, such as \"skills/game-building\" or \"reference/Validation\".")
			.Field("title", &DocsTopic::Title, "A skill's description or a reference document's first heading.");

		registry.Struct<DocsGetResult>("DocsGetResult", "The documentation topics, or one topic's Markdown.")
			.Field("topics", &DocsGetResult::Topics, "Every topic, when no topic was asked for.")
			.Field("topic", &DocsGetResult::Topic, "The topic asked for.")
			.Field("content", &DocsGetResult::Content, "The topic's Markdown.")
			.Field("path", &DocsGetResult::Path, "The topic's file, relative to the repository root.");
	}

	void RegisterEditorObserveMethods(MethodRegistry& methods)
	{
		Json docsExample = Json::object();
		docsExample["topic"] = "skills/add-automation-method";
		methods.Add(
			{
				.Name = "docs.get",
				.Description = "Serves the agent documentation: without a topic the list of skills and reference documents, with one its "
							   "Markdown.",
				.ExposeAsTool = true,
				.AvailableInLauncher = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the add-automation-method skill.", .Params = docsExample } },
			},
			&Automation::DocsGet);
	}

}
