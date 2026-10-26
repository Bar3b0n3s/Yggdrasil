#include "EditorPCH.h"
#include "EditorCore/Automation/ObserveMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/ResultOffload.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace Engine {

	namespace Utils {

		constexpr std::string_view SkillsTopicPrefix = "skills/";
		constexpr std::string_view SchemasTopicPrefix = "reference/Schemas/";
		constexpr std::string_view ReferenceTopicPrefix = "reference/";
		constexpr std::string_view ScriptingTopicPrefix = "scripting/";
		constexpr std::string_view TemplatesTopicPrefix = "templates/Scripts/";
		constexpr std::string_view SkillsDirectory = ".claude/skills";
		constexpr std::string_view SkillFileName = "SKILL.md";
		constexpr std::string_view ReferenceDirectory = "Docs/Reference";
		constexpr std::string_view SchemasDirectory = "Docs/Reference/Schemas";
		constexpr std::string_view SchemaExtension = ".schema.json";
		constexpr std::string_view ScriptingDirectory = "Resources/Scripting";
		constexpr std::string_view DeclarationsExtension = ".d.luau";
		constexpr std::string_view TemplatesDirectory = "Resources/Templates/Scripts";
		constexpr std::string_view TemplateExtension = ".luau";
		// The native module's interface (ADR 0030 decision 26): one fixed topic, since no other engine header is a module's.
		constexpr std::string_view NativeApiTopic = "native/NativeApi.h";
		constexpr std::string_view NativeApiPath = "Engine/Source/Engine/Native/NativeApi.h";

		// How a topic's text divides into sections.
		enum class DocsTextKind : uint8_t
		{
			Markdown,     // headings
			Declarations, // top-level Luau declarations
			Plain         // none
		};

		struct DocsTopicFile
		{
			std::string Path{}; // repository-relative
			DocsTextKind Kind = DocsTextKind::Plain;
		};

		// One section of a text: [Begin, End) in bytes.
		struct TextSection
		{
			std::string Title{};
			std::string Anchor{};
			uint32_t Level = 1;
			size_t Begin = 0;
			size_t End = 0;
		};

		[[nodiscard]] static uint32_t ToByteCount(size_t value)
		{
			return static_cast<uint32_t>(std::min<size_t>(value, std::numeric_limits<uint32_t>::max()));
		}

		// A topic name segment: letters, digits, '-' and '_' only, so a topic can never name a path outside its directory.
		static bool IsTopicSegment(std::string_view name)
		{
			return !name.empty() && std::all_of(name.begin(), name.end(), [](char character)
			{
				return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
					|| character == '-' || character == '_';
			});
		}

		// The lines of `text` with their byte offsets, without their line breaks ("\n" or "\r\n").
		template<typename Visitor>
		static void ForEachLine(std::string_view text, Visitor&& visit)
		{
			size_t start = 0;
			while (start < text.size())
			{
				const size_t end = text.find('\n', start);
				const size_t stop = end == std::string_view::npos ? text.size() : end;
				std::string_view line = text.substr(start, stop - start);
				if (line.ends_with('\r'))
					line.remove_suffix(1);
				visit(line, start);
				if (end == std::string_view::npos)
					break;
				start = end + 1;
			}
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
			std::string heading;
			ForEachLine(text, [&heading](std::string_view line, size_t /*start*/)
			{
				if (heading.empty() && line.starts_with("# "))
					heading = std::string(line.substr(2));
			});
			return heading;
		}

		// A JSON schema's "description", else its "title"; empty when it has neither or is not JSON.
		static std::string ReadSchemaTitle(std::string_view text)
		{
			const Result<Json> document = JsonReader::Parse(text);
			if (!document)
				return {};
			const JsonReader reader(*document);
			return reader.ReadMember<std::string>("description").value_or(reader.ReadMember<std::string>("title").value_or(std::string()));
		}

		// The file of a topic; nullopt for a name that is not a topic.
		static std::optional<DocsTopicFile> GetTopicFile(std::string_view topic)
		{
			if (topic.starts_with(SkillsTopicPrefix))
			{
				const std::string_view name = topic.substr(SkillsTopicPrefix.size());
				if (IsTopicSegment(name))
					return DocsTopicFile{ std::format("{}/{}/{}", SkillsDirectory, name, SkillFileName), DocsTextKind::Markdown };
				return std::nullopt;
			}
			if (topic.starts_with(SchemasTopicPrefix))
			{
				const std::string_view name = topic.substr(SchemasTopicPrefix.size());
				if (IsTopicSegment(name))
					return DocsTopicFile{ std::format("{}/{}{}", SchemasDirectory, name, SchemaExtension), DocsTextKind::Plain };
				return std::nullopt;
			}
			if (topic.starts_with(ReferenceTopicPrefix))
			{
				const std::string_view name = topic.substr(ReferenceTopicPrefix.size());
				if (IsTopicSegment(name))
					return DocsTopicFile{ std::format("{}/{}.md", ReferenceDirectory, name), DocsTextKind::Markdown };
				return std::nullopt;
			}
			if (topic.starts_with(ScriptingTopicPrefix))
			{
				const std::string_view name = topic.substr(ScriptingTopicPrefix.size());
				if (name.ends_with(DeclarationsExtension) && IsTopicSegment(name.substr(0, name.size() - DeclarationsExtension.size())))
					return DocsTopicFile{ std::format("{}/{}", ScriptingDirectory, name), DocsTextKind::Declarations };
				return std::nullopt;
			}
			if (topic.starts_with(TemplatesTopicPrefix))
			{
				const std::string_view name = topic.substr(TemplatesTopicPrefix.size());
				if (IsTopicSegment(name))
					return DocsTopicFile{ std::format("{}/{}{}", TemplatesDirectory, name, TemplateExtension), DocsTextKind::Plain };
				return std::nullopt;
			}
			if (topic == NativeApiTopic)
				return DocsTopicFile{ std::string(NativeApiPath), DocsTextKind::Plain };
			return std::nullopt;
		}

		// The files directly in `directory` (under `docsRoot`) whose names end with `extension` and whose stem is a topic
		// segment, as (stem, path); none when the directory does not exist.
		static Result<std::vector<std::pair<std::string, std::filesystem::path>>> ListTopicFiles(const std::filesystem::path& docsRoot,
			std::string_view directory, std::string_view extension)
		{
			std::vector<std::pair<std::string, std::filesystem::path>> files;
			const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(docsRoot / FileSystem::PathFromUtf8(directory));
			if (!entries)
			{
				if (entries.error().GetCode() == ErrorCode::NotFound)
					return files;
				return std::unexpected(ToEditorFileError(entries.error()));
			}
			for (const std::filesystem::path& file : *entries)
			{
				const std::string name = FileSystem::PathToUtf8(file.filename());
				if (!name.ends_with(extension))
					continue;
				std::string stem = name.substr(0, name.size() - extension.size());
				if (IsTopicSegment(stem))
					files.emplace_back(std::move(stem), file);
			}
			return files;
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
					topics.push_back(DocsTopic{ std::format("{}{}", SkillsTopicPrefix, name), ReadSkillDescription(*text), ToByteCount(text->size()) });
				}
			}
			else if (skills.error().GetCode() != ErrorCode::NotFound)
			{
				return std::unexpected(ToEditorFileError(skills.error()));
			}

			ENGINE_TRY_ASSIGN(const auto references, ListTopicFiles(docsRoot, ReferenceDirectory, ".md"));
			for (const auto& [stem, file] : references)
			{
				if (const Result<std::string> text = FileSystem::ReadText(file))
					topics.push_back(DocsTopic{ std::format("{}{}", ReferenceTopicPrefix, stem), ReadFirstHeading(*text), ToByteCount(text->size()) });
			}
			ENGINE_TRY_ASSIGN(const auto schemas, ListTopicFiles(docsRoot, SchemasDirectory, SchemaExtension));
			for (const auto& [stem, file] : schemas)
			{
				if (const Result<std::string> text = FileSystem::ReadText(file))
					topics.push_back(DocsTopic{ std::format("{}{}", SchemasTopicPrefix, stem), ReadSchemaTitle(*text), ToByteCount(text->size()) });
			}
			ENGINE_TRY_ASSIGN(const auto declarations, ListTopicFiles(docsRoot, ScriptingDirectory, DeclarationsExtension));
			for (const auto& [stem, file] : declarations)
			{
				if (const Result<std::string> text = FileSystem::ReadText(file))
				{
					topics.push_back(DocsTopic{ std::format("{}{}{}", ScriptingTopicPrefix, stem, DeclarationsExtension),
						"The generated Luau type declarations of the script API; its sections are the declarations (\"Entity\", \"Input\").",
						ToByteCount(text->size()) });
				}
			}
			ENGINE_TRY_ASSIGN(const auto templates, ListTopicFiles(docsRoot, TemplatesDirectory, TemplateExtension));
			for (const auto& [stem, file] : templates)
			{
				if (const Result<std::string> text = FileSystem::ReadText(file))
				{
					topics.push_back(DocsTopic{ std::format("{}{}", TemplatesTopicPrefix, stem),
						std::format("The script.create template \"{}\"; {{{{Name}}}} becomes the script's name.", stem), ToByteCount(text->size()) });
				}
			}
			if (const Result<std::string> text = FileSystem::ReadText(docsRoot / FileSystem::PathFromUtf8(NativeApiPath)))
			{
				topics.push_back(DocsTopic{ std::string(NativeApiTopic),
					"The interface of a game's native C++ module: the one engine header the sources below Native/ include, with ENGINE_NATIVE_MODULE, "
					"the component, resource and system builders and the step context.",
					ToByteCount(text->size()) });
			}

			std::sort(topics.begin(), topics.end(), [](const DocsTopic& left, const DocsTopic& right)
			{
				return left.Name < right.Name;
			});
			return topics;
		}

		// A section's anchor (ObserveMethods.h): lowercase ASCII letters, digits, '-', '_' and non-ASCII bytes kept, spaces
		// turned into '-', every other ASCII character removed.
		[[nodiscard]] static std::string MakeAnchor(std::string_view text)
		{
			std::string anchor;
			anchor.reserve(text.size());
			for (const char character : text)
			{
				if (character >= 'A' && character <= 'Z')
					anchor.push_back(static_cast<char>(character - 'A' + 'a'));
				else if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-' || character == '_'
					|| static_cast<unsigned char>(character) >= 0x80)
					anchor.push_back(character);
				else if (character == ' ')
					anchor.push_back('-');
			}
			return anchor;
		}

		// The headings of a Markdown text outside fenced code blocks, each ending where the next heading of the same or a
		// higher level starts.
		[[nodiscard]] static std::vector<TextSection> FindMarkdownSections(std::string_view text)
		{
			std::vector<TextSection> sections;
			char fence = '\0';
			ForEachLine(text, [&sections, &fence](std::string_view line, size_t start)
			{
				std::string_view content = line;
				for (size_t indent = 0; indent < 3 && content.starts_with(' '); ++indent)
					content.remove_prefix(1);
				if (content.starts_with("```") || content.starts_with("~~~"))
				{
					if (fence == '\0')
						fence = content.front();
					else if (content.front() == fence)
						fence = '\0';
					return;
				}
				if (fence != '\0' || !content.starts_with('#'))
					return;
				size_t level = 0;
				while (level < content.size() && content[level] == '#')
					++level;
				if (level > 6 || (level < content.size() && content[level] != ' ' && content[level] != '\t'))
					return;
				std::string_view title = content.substr(level);
				while (!title.empty() && (title.front() == ' ' || title.front() == '\t'))
					title.remove_prefix(1);
				while (!title.empty() && (title.back() == ' ' || title.back() == '\t'))
					title.remove_suffix(1);
				// An optional closing sequence of '#' after a space.
				const size_t closing = title.find_last_not_of('#');
				if (closing != std::string_view::npos && closing + 1 < title.size() && (title[closing] == ' ' || title[closing] == '\t'))
				{
					title = title.substr(0, closing);
					while (!title.empty() && (title.back() == ' ' || title.back() == '\t'))
						title.remove_suffix(1);
				}
				sections.push_back(TextSection{ .Title = std::string(title), .Anchor = {}, .Level = static_cast<uint32_t>(level), .Begin = start, .End = 0 });
			});
			for (size_t index = 0; index < sections.size(); ++index)
			{
				sections[index].End = text.size();
				for (size_t next = index + 1; next < sections.size(); ++next)
				{
					if (sections[next].Level <= sections[index].Level)
					{
						sections[index].End = sections[next].Begin;
						break;
					}
				}
			}
			return sections;
		}

		// The name a top-level Luau declaration line declares ("declare extern type Entity with" and "declare Input: {" give
		// "Entity" and "Input"); empty for another line.
		[[nodiscard]] static std::string_view GetDeclaredName(std::string_view line)
		{
			constexpr std::string_view Declare = "declare ";
			constexpr std::string_view ExternType = "extern type ";
			if (!line.starts_with(Declare))
				return {};
			std::string_view rest = line.substr(Declare.size());
			if (rest.starts_with(ExternType))
				rest.remove_prefix(ExternType.size());
			size_t length = 0;
			while (length < rest.size()
				&& ((rest[length] >= 'a' && rest[length] <= 'z') || (rest[length] >= 'A' && rest[length] <= 'Z') || (rest[length] >= '0' && rest[length] <= '9')
					|| rest[length] == '_'))
				++length;
			return rest.substr(0, length);
		}

		// The top-level declarations of a .d.luau text: each "declare" line with the comment lines directly above it, up to
		// the next declaration; adjacent declarations of one name form one section. The text before the first declaration
		// (the type aliases) belongs to no section. A name declared again further on (the Script component's extern type and
		// the Script global) titles its extern type's section "<name> (type)", as ScriptAPI.md qualifies a repeated heading,
		// and its global's "<name>", so each section is reachable by its title.
		[[nodiscard]] static std::vector<TextSection> FindDeclarationSections(std::string_view text)
		{
			std::vector<TextSection> sections;
			std::vector<bool> externTypes; // whether each section starts with an extern type declaration
			std::optional<size_t> comments;
			ForEachLine(text, [&sections, &externTypes, &comments](std::string_view line, size_t start)
			{
				constexpr std::string_view ExternTypeDeclaration = "declare extern type ";
				if (line.starts_with("--"))
				{
					if (!comments.has_value())
						comments = start;
					return;
				}
				const std::string_view name = GetDeclaredName(line);
				const size_t begin = comments.value_or(start);
				comments.reset();
				if (name.empty() || (!sections.empty() && sections.back().Title == name))
					return;
				sections.push_back(TextSection{ .Title = std::string(name), .Anchor = {}, .Level = 1, .Begin = begin, .End = 0 });
				externTypes.push_back(line.starts_with(ExternTypeDeclaration));
			});
			std::map<std::string, uint32_t, std::less<>> declarations;
			for (const TextSection& section : sections)
				++declarations[section.Title];
			for (size_t index = 0; index < sections.size(); ++index)
			{
				if (externTypes[index] && declarations[sections[index].Title] > 1)
					sections[index].Title += " (type)";
				sections[index].End = index + 1 < sections.size() ? sections[index + 1].Begin : text.size();
			}
			return sections;
		}

		// The sections of `text` with unique anchors: a repeated anchor gets "-1", "-2", ... in document order.
		[[nodiscard]] static std::vector<TextSection> FindSections(std::string_view text, DocsTextKind kind)
		{
			std::vector<TextSection> sections = kind == DocsTextKind::Markdown ? FindMarkdownSections(text)
				: kind == DocsTextKind::Declarations                           ? FindDeclarationSections(text)
																			   : std::vector<TextSection>();
			std::set<std::string, std::less<>> taken;
			std::map<std::string, uint32_t, std::less<>> repeats; // the last suffix given to each repeated anchor
			for (TextSection& section : sections)
			{
				const std::string base = MakeAnchor(section.Title);
				std::string anchor = base;
				if (taken.contains(anchor))
				{
					uint32_t& suffix = repeats[base];
					do
					{
						anchor = std::format("{}-{}", base, ++suffix);
					} while (taken.contains(anchor));
				}
				taken.insert(anchor);
				section.Anchor = std::move(anchor);
			}
			return sections;
		}

		// The bytes a byte takes in a JSON string as the dispatcher writes it (nlohmann, UTF-8 kept).
		[[nodiscard]] static size_t GetEscapedSize(char character)
		{
			switch (character)
			{
				case '"':
				case '\\':
				case '\b':
				case '\f':
				case '\n':
				case '\r':
				case '\t':
					return 2;
				default:
					return static_cast<unsigned char>(character) < 0x20 ? 6 : 1;
			}
		}

		[[nodiscard]] static bool IsContinuationByte(char character)
		{
			return (static_cast<unsigned char>(character) & 0xC0) == 0x80;
		}

		// One page of `text` from `offset` (moved forward to the start of a character): at most `length` bytes whose escaped
		// size stays within DocsPageBytes, ending after the last line break inside it when the text goes on, else at a
		// character boundary. A page that is not at the end holds at least one character.
		[[nodiscard]] static std::pair<size_t, size_t> MakePage(std::string_view text, size_t offset, size_t length)
		{
			size_t begin = std::min(offset, text.size());
			while (begin < text.size() && IsContinuationByte(text[begin]))
				++begin;
			size_t end = begin;
			size_t escaped = 0;
			while (end < text.size() && end - begin < length)
			{
				const size_t cost = GetEscapedSize(text[end]);
				if (escaped + cost > DocsPageBytes)
					break;
				escaped += cost;
				++end;
			}
			if (end < text.size())
			{
				const size_t lineBreak = end > begin ? text.rfind('\n', end - 1) : std::string_view::npos;
				if (lineBreak != std::string_view::npos && lineBreak >= begin)
					end = lineBreak + 1;
				else
				{
					while (end > begin && IsContinuationByte(text[end]))
						--end;
				}
			}
			if (end == begin && begin < text.size())
			{
				++end;
				while (end < text.size() && IsContinuationByte(text[end]))
					++end;
			}
			return { begin, end };
		}

		// A heading's text without its trailing parenthesized qualifier ("TestSuite (type)" gives "TestSuite"); empty for a
		// text without one.
		[[nodiscard]] static std::string_view StripQualifier(std::string_view title)
		{
			if (!title.ends_with(')'))
				return {};
			const size_t open = title.rfind(" (");
			if (open == std::string_view::npos || open == 0 || open + 3 >= title.size())
				return {};
			return title.substr(0, open);
		}

		using SectionIterator = std::vector<TextSection>::const_iterator;

		// The section `name` names in `sections` (ObserveMethods.h): the heading whose text is exactly `name`, else the one
		// whose text is `name` with a parenthesized qualifier, else the section whose anchor is `name` converted. Errors:
		// InvalidArgument at /section for a text several headings share, listing their anchors; NotFound at /section for no
		// match, with suggestions.
		[[nodiscard]] static Result<SectionIterator> FindSection(const std::vector<TextSection>& sections, std::string_view name, std::string_view topic)
		{
			std::vector<SectionIterator> found;
			for (auto entry = sections.begin(); entry != sections.end(); ++entry)
			{
				if (entry->Title == name)
					found.push_back(entry);
			}
			if (found.empty())
			{
				for (auto entry = sections.begin(); entry != sections.end(); ++entry)
				{
					const std::string_view base = StripQualifier(entry->Title);
					if (!base.empty() && base == name)
						found.push_back(entry);
				}
			}
			if (found.size() == 1)
				return found.front();
			if (found.size() > 1)
			{
				std::string anchors;
				for (const SectionIterator& entry : found)
					anchors += std::format("{}'{}'", anchors.empty() ? "" : ", ", entry->Anchor);
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/section", std::format("{} sections of '{}' are named '{}'", found.size(), topic, name),
					std::format("name one by its anchor: {}", anchors)));
			}

			const std::string anchor = MakeAnchor(name);
			const auto byAnchor = std::ranges::find(sections, anchor, &TextSection::Anchor);
			if (byAnchor != sections.end())
				return byAnchor;
			std::vector<std::string> anchors;
			for (const TextSection& entry : sections)
				anchors.push_back(entry.Anchor);
			const std::vector<std::string> suggestions = FuzzySuggest(anchor, std::span<const std::string>(anchors));
			const std::string hint = !suggestions.empty() ? MakeDidYouMeanHint(suggestions)
				: sections.empty()                        ? std::format("'{}' has no sections; page it with offset and length", topic)
														  : std::format("docs.get {{\"topic\": \"{}\", \"toc\": true}} lists its sections", topic);
			return std::unexpected(MakeParamError(ErrorCode::NotFound, "/section", std::format("'{}' has no section '{}'", topic, name), hint));
		}

		// The minified JSON size of one section list entry, as the registry writes it.
		[[nodiscard]] static size_t MeasureSection(const DocsSection& section)
		{
			const Json entry = Json{ { "title", section.Title }, { "anchor", section.Anchor }, { "level", section.Level }, { "offset", section.Offset }, { "bytes", section.Bytes } };
			return entry.dump(-1, ' ', false, Json::error_handler_t::replace).size();
		}

		// A section list: the entries of `candidates` (offsets relative to `base`) down to the deepest heading level whose
		// list stays within `budget` bytes (DocsSectionListBytes for a first page, DocsTocBytes for toc); when even the top
		// level does not fit, its leading entries that do.
		[[nodiscard]] static std::vector<DocsSection> MakeSectionList(std::span<const TextSection> candidates, size_t base, size_t budget)
		{
			std::vector<uint32_t> levels;
			for (const TextSection& section : candidates)
				levels.push_back(section.Level);
			std::ranges::sort(levels);
			levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
			std::vector<DocsSection> chosen;
			for (const uint32_t level : levels)
			{
				std::vector<DocsSection> list;
				size_t bytes = 2; // the brackets
				bool fits = true;
				for (const TextSection& section : candidates)
				{
					if (section.Level > level)
						continue;
					DocsSection entry{ .Title = section.Title, .Anchor = section.Anchor, .Level = section.Level, .Offset = ToByteCount(section.Begin - base), .Bytes = ToByteCount(section.End - section.Begin) };
					const size_t size = MeasureSection(entry) + 1; // and its comma
					if (bytes + size > budget)
					{
						fits = false;
						break;
					}
					bytes += size;
					list.push_back(std::move(entry));
				}
				if (!fits)
				{
					// Only the top level can leave nothing chosen: its leading entries that fit.
					if (chosen.empty())
						chosen = std::move(list);
					break;
				}
				chosen = std::move(list);
			}
			return chosen;
		}

	}

	namespace Automation {

		Result<DocsGetResult> DocsGet(EditorMethodContext& context, const DocsGetParams& params)
		{
			const std::filesystem::path& docsRoot = context.GetServer().GetSpecification().DocsRoot;
			if (docsRoot.empty())
				return MakeError(ErrorCode::Unsupported, "docs.get needs the repository root, which this editor was not given");

			std::string_view topic = params.Topic;
			std::string_view section = params.Section;
			// The topic's "#<section>" when `section` is given too: both must name the same section.
			std::string_view suffix;
			if (const size_t hash = topic.find('#'); hash != std::string_view::npos)
			{
				suffix = topic.substr(hash + 1);
				topic = topic.substr(0, hash);
			}
			while (section.starts_with('#'))
				section.remove_prefix(1);
			while (suffix.starts_with('#'))
				suffix.remove_prefix(1);
			if (section.empty())
				std::swap(section, suffix);

			const bool paging = context.HasParam("offset") || context.HasParam("length");
			DocsGetResult result;
			if (topic.empty())
			{
				if (!section.empty() || paging || params.Toc)
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/topic", "section, offset, length and toc need a topic", "docs.get {} lists the topics"));
				ENGINE_TRY_ASSIGN(result.Topics, Utils::ListTopics(docsRoot));
				return result;
			}
			if (params.Toc && paging)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/toc", "toc returns the section list without text, so offset and length do not apply",
					"drop offset and length to list the sections, or toc to read the text"));
			}

			const std::optional<Utils::DocsTopicFile> file = Utils::GetTopicFile(topic);
			std::optional<std::string> text;
			if (file.has_value())
			{
				Result<std::string> read = FileSystem::ReadText(docsRoot / FileSystem::PathFromUtf8(file->Path));
				// The same spelling on every host: a topic that matches a file only ignoring case is not that topic.
				if (read.has_value() && FileSystem::VerifyCase(docsRoot, file->Path).has_value())
					text = std::move(*read);
				else if (!read && read.error().GetCode() != ErrorCode::NotFound)
					return std::unexpected(Utils::ToEditorFileError(read.error()));
			}
			if (!text.has_value())
			{
				ENGINE_TRY_ASSIGN(const std::vector<DocsTopic> topics, Utils::ListTopics(docsRoot));
				std::vector<std::string> names;
				for (const DocsTopic& entry : topics)
					names.push_back(entry.Name);
				const std::vector<std::string> suggestions = FuzzySuggest(topic, std::span<const std::string>(names));
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/topic", std::format("no topic '{}'", topic),
					suggestions.empty() ? std::string("docs.get {} lists the topics") : MakeDidYouMeanHint(suggestions)));
			}

			const std::vector<Utils::TextSection> sections = Utils::FindSections(*text, file->Kind);
			std::string_view selected = *text;
			size_t base = 0;
			std::span<const Utils::TextSection> candidates(sections);
			if (!section.empty())
			{
				ENGINE_TRY_ASSIGN(const Utils::SectionIterator found, Utils::FindSection(sections, section, topic));
				if (!suffix.empty())
				{
					ENGINE_TRY_ASSIGN(const Utils::SectionIterator named, Utils::FindSection(sections, suffix, topic));
					if (named != found)
					{
						return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/section",
							std::format("section '{}' and the topic's '#{}' name different sections", section, suffix), "give the section once"));
					}
				}
				selected = selected.substr(found->Begin, found->End - found->Begin);
				base = found->Begin;
				result.Section = found->Anchor;
				// Its subsections: the sections after it that start inside it (none for a declaration).
				const auto first = found + 1;
				const auto last = std::find_if(first, sections.end(), [end = found->End](const Utils::TextSection& entry)
				{
					return entry.Begin >= end;
				});
				candidates = std::span<const Utils::TextSection>(first, last);
			}

			result.Topic = std::string(topic);
			result.Path = file->Path;
			result.TotalBytes = Utils::ToByteCount(selected.size());
			if (params.Toc)
			{
				result.Sections = Utils::MakeSectionList(candidates, base, DocsTocBytes);
				return result;
			}
			if (!paging)
			{
				result.Content = std::string(selected);
				ENGINE_TRY_ASSIGN(const Json whole, context.SerializeResult(result));
				if (whole.dump(-1, ' ', false, Json::error_handler_t::replace).size() <= DefaultOffloadThresholdBytes)
					return result;
			}
			if (params.Offset > selected.size())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/offset",
					std::format("offset {} is beyond the end of the text ({} bytes)", params.Offset, selected.size()), "start at 0, or at a page's nextOffset"));
			}
			if (params.Length == 0 || params.Length > DocsPageBytes)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/length",
					std::format("length must be 1 to {} (got {})", DocsPageBytes, params.Length), "omit length for the largest page"));
			}
			const auto [begin, end] = Utils::MakePage(selected, params.Offset, params.Length);
			result.Content = std::string(selected.substr(begin, end - begin));
			result.Offset = Utils::ToByteCount(begin);
			result.NextOffset = end < selected.size() ? Utils::ToByteCount(end) : 0;
			if (begin == 0 && end < selected.size())
				result.Sections = Utils::MakeSectionList(candidates, base, DocsSectionListBytes);
			return result;
		}

	}

	void RegisterEditorObserveMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<DocsGetParams>("DocsGetParams", "The params of docs.get.")
			.Field("topic", &DocsGetParams::Topic, "The topic, such as \"skills/game-building\", \"reference/ScriptAPI\", \"reference/Schemas/Scene\", \"scripting/Engine.d.luau\", \"templates/Scripts/Behaviour\" or \"native/NativeApi.h\"; a \"#<section>\" suffix selects a section like section; empty for the list of topics.")
			.Field("section", &DocsGetParams::Section, "A section of the topic: a Markdown heading, or a top-level declaration of a .d.luau topic (\"Entity\"). Named by the heading's exact text (\"Test.Suite\"), by that text without a trailing parenthesized qualifier (\"TestSuite\" for \"TestSuite (type)\"), or by its anchor (the text lowercased, spaces as '-', other ASCII punctuation removed, \"-1\" added to a repeat: \"testsuite-type\"), tried in that order; a text several headings share is refused with their anchors. The result is the section with its subsections.")
			.Field("offset", &DocsGetParams::Offset, "Where the page starts, in bytes of the topic's (or the section's) text: 0, or the previous page's nextOffset.")
			.Field("length", &DocsGetParams::Length, "The most bytes of text to return (the default is the most). A page ends after a line break when one falls inside it.", { .Min = 1.0, .Max = static_cast<double>(DocsPageBytes) })
			.Field("toc", &DocsGetParams::Toc, std::format("Return no text, only the topic's section list (or the subsections of section) with each section's title, anchor, level, offset and size, down to the deepest heading level whose list fits in {} bytes, and totalBytes. Not with offset or length.", DocsTocBytes));

		registry.Struct<DocsTopic>("DocsTopic", "One documentation topic.")
			.Field("name", &DocsTopic::Name, "The topic's name, such as \"skills/game-building\" or \"reference/Validation\".")
			.Field("title", &DocsTopic::Title, "A skill's description, a reference document's first heading, a schema's description, or a summary of the script declarations, a script template or the native module's interface.")
			.Field("bytes", &DocsTopic::Bytes, std::format("The size of the topic's text; a topic whose result would be over the offload threshold comes back in pages of at most {} bytes.", DocsPageBytes));

		registry.Struct<DocsSection>("DocsSection", "One section of a topic, as a long text's first page or toc lists it.")
			.Field("title", &DocsSection::Title, "The heading's text, or the declaration's name.")
			.Field("anchor", &DocsSection::Anchor, "The section's anchor: one name section accepts (after its exact heading text).")
			.Field("level", &DocsSection::Level, "1 to 6: the heading's number of '#'; 1 for a declaration.")
			.Field("offset", &DocsSection::Offset, "Where the section starts in the text the page belongs to (the topic's, or the section's), for offset.")
			.Field("bytes", &DocsSection::Bytes, "The section's size, subsections included.");

		registry.Struct<DocsGetResult>("DocsGetResult", "The documentation topics, or one topic's text, a section of it or one page.")
			.Field("topics", &DocsGetResult::Topics, "Every topic, when no topic was asked for.")
			.Field("topic", &DocsGetResult::Topic, "The topic asked for, without its \"#<section>\".")
			.Field("section", &DocsGetResult::Section, "The anchor of the section returned; empty for the whole topic.")
			.Field("content", &DocsGetResult::Content, "The text (Markdown for skills and reference documents), or one page of it.")
			.Field("path", &DocsGetResult::Path, "The topic's file, relative to the repository root.")
			.Field("offset", &DocsGetResult::Offset, "Where content starts in the topic's (or the section's) text, in bytes.")
			.Field("totalBytes", &DocsGetResult::TotalBytes, "The size of the topic's (or the section's) text.")
			.Field("nextOffset", &DocsGetResult::NextOffset, "The offset of the next page; 0 when content reaches the end of the text.")
			.Field("sections", &DocsGetResult::Sections,
				std::format("The sections of the text: with the first page of a text that needs several pages, every heading level whose list fits in {} bytes; with toc, every level whose list fits in {} bytes.",
					DocsSectionListBytes, DocsTocBytes));
	}

	void RegisterEditorObserveMethods(MethodRegistry& methods)
	{
		Json docsExample = Json::object();
		docsExample["topic"] = "skills/add-automation-method";
		Json sectionExample = Json::object();
		sectionExample["topic"] = "reference/ScriptAPI";
		sectionExample["section"] = "Input";
		Json pageExample = Json::object();
		pageExample["topic"] = "scripting/Engine.d.luau";
		pageExample["offset"] = 0;
		pageExample["length"] = 8192;
		Json tocExample = Json::object();
		tocExample["topic"] = "reference/ScriptAPI";
		tocExample["toc"] = true;
		methods.Add(
			{
				.Name = "docs.get",
				.Description = "Serves the agent documentation: without a topic the list of topics (skills, the generated reference and JSON "
							   "schemas, the script declarations, the script templates and the native module's interface); with one its text, "
							   "one section of it (section, or \"<topic>#<section>\", by heading text or anchor), one page (offset, length) or "
							   "with toc only its section list with sizes. A text too large to return inline comes back as its first page with "
							   "nextOffset and its list of sections.",
				.ExposeAsTool = true,
				.AvailableInLauncher = true,
				.AllowedInBatch = true,
				.Examples = {
					{ .Description = "Read the add-automation-method skill.", .Params = docsExample },
					{ .Description = "Read the Input section of the script API reference.", .Params = sectionExample },
					{ .Description = "Read the first 8 KB of the script declarations.", .Params = pageExample },
					{ .Description = "List the sections of the script API reference with their sizes.", .Params = tocExample },
				},
			},
			&Automation::DocsGet);
	}

}
