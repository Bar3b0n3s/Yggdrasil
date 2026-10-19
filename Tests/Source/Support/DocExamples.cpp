#include "TestsPCH.h"
#include "Support/DocExamples.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <format>
#include <map>
#include <system_error>
#include <utility>

namespace Engine {

	namespace Test {

		namespace {

			[[nodiscard]] Error MakeLineError(uint32_t line, std::string message)
			{
				ErrorLocation location;
				location.Line = line;
				return Error(ErrorCode::Validation, std::move(message)).WithLocation(std::move(location));
			}

			// The count of leading spaces and tabs of `line`.
			size_t CountIndentation(std::string_view line)
			{
				size_t count = 0;
				while (count < line.size() && (line[count] == ' ' || line[count] == '\t'))
					++count;
				return count;
			}

			std::string_view TrimSpaces(std::string_view text)
			{
				const size_t first = text.find_first_not_of(" \t");
				if (first == std::string_view::npos)
					return {};
				const size_t last = text.find_last_not_of(" \t");
				return text.substr(first, last - first + 1);
			}

			// Parses the info string `info` of the fence on `line` into `block`; false (no error) for another language.
			Result<bool> ParseInfo(std::string_view info, uint32_t line, DocExampleBlock& block)
			{
				size_t position = 0;
				const auto skipSpaces = [&info, &position]()
				{
					while (position < info.size() && (info[position] == ' ' || info[position] == '\t'))
						++position;
				};
				skipSpaces();
				const size_t languageEnd = std::min(info.find_first_of(" \t", position), info.size());
				const std::string_view language = info.substr(position, languageEnd - position);
				if (language != "lua" && language != "luau")
					return false;
				position = languageEnd;
				while (true)
				{
					skipSpaces();
					if (position >= info.size())
						break;
					const size_t keyEnd = std::min(info.find_first_of(" \t=", position), info.size());
					const std::string key(info.substr(position, keyEnd - position));
					if (keyEnd >= info.size() || info[keyEnd] != '=')
						return std::unexpected(MakeLineError(line, std::format("the Luau fence attribute '{}' needs a value (key=value)", key)));
					position = keyEnd + 1;
					std::string value;
					if (position < info.size() && info[position] == '"')
					{
						const size_t close = info.find('"', position + 1);
						if (close == std::string_view::npos)
							return std::unexpected(MakeLineError(line, std::format("the quoted value of '{}' is never closed", key)));
						value = std::string(info.substr(position + 1, close - position - 1));
						position = close + 1;
					}
					else
					{
						const size_t valueEnd = std::min(info.find_first_of(" \t", position), info.size());
						value = std::string(info.substr(position, valueEnd - position));
						position = valueEnd;
					}
					if (key == "path")
					{
						if (!value.starts_with("Assets/") || !value.ends_with(".luau") || !VfsPath::Create("project", value).has_value())
							return std::unexpected(MakeLineError(line, std::format("path='{}' must name a .luau file below Assets/", value)));
						block.Path = std::move(value);
					}
					else if (key == "prelude")
					{
						if (value.empty())
							return std::unexpected(MakeLineError(line, "prelude= needs a prelude name"));
						block.Prelude = std::move(value);
					}
					else if (key == "skip")
					{
						if (TrimSpaces(value).empty())
							return std::unexpected(MakeLineError(line, "skip= needs a reason, such as skip=\"an excerpt\""));
						block.Skip = std::move(value);
					}
					else
					{
						return std::unexpected(MakeLineError(line, std::format("unknown Luau fence attribute '{}' (path, prelude or skip)", key)));
					}
				}
				return true;
			}

			// Reads the stub modules a check's requires resolve to.
			class StubModuleReader final : public IScriptModuleReader
			{
			public:
				explicit StubModuleReader(const std::map<VfsPath, std::string>& modules)
					: m_Modules(modules)
				{
				}

				Result<std::string> ReadModule(const VfsPath& path) override
				{
					if (const auto found = m_Modules.find(path); found != m_Modules.end())
						return found->second;
					return MakeError(ErrorCode::NotFound, "no doc-example stub module '{}' (Tests/Data/TypeChecker/DocExamples/Assets)", path);
				}
			private:
				const std::map<VfsPath, std::string>& m_Modules; // borrowed for one check
			};

			// The text of a prelude around its marker line.
			struct Prelude
			{
				std::string Before{};
				std::string After{};
				std::string File{}; // for messages
				bool Valid = false;
			};

		}

		Result<std::vector<DocExampleBlock>> ExtractDocExamples(std::string_view text)
		{
			std::vector<DocExampleBlock> blocks;
			bool inside = false;
			char fence = '`';
			size_t fenceLength = 0;
			size_t fenceIndentation = 0;
			bool isLuau = false;
			uint32_t openedAt = 0;
			DocExampleBlock block;

			uint32_t lineNumber = 0;
			size_t start = 0;
			while (start < text.size())
			{
				const size_t end = text.find('\n', start);
				std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
				start = end == std::string_view::npos ? text.size() : end + 1;
				++lineNumber;
				if (line.ends_with('\r'))
					line.remove_suffix(1);

				const size_t indentation = CountIndentation(line);
				const std::string_view rest = line.substr(indentation);
				if (inside)
				{
					const size_t run = rest.find_first_not_of(fence);
					const size_t length = run == std::string_view::npos ? rest.size() : run;
					if (length >= fenceLength && TrimSpaces(rest.substr(length)).empty())
					{
						inside = false;
						if (isLuau)
							blocks.push_back(std::move(block));
						continue;
					}
					if (isLuau)
					{
						block.Source += std::string(line.substr(std::min(indentation, fenceIndentation)));
						block.Source += '\n';
					}
					continue;
				}

				if (rest.starts_with("```") || rest.starts_with("~~~"))
				{
					fence = rest.front();
					const size_t run = rest.find_first_not_of(fence);
					fenceLength = run == std::string_view::npos ? rest.size() : run;
					fenceIndentation = indentation;
					openedAt = lineNumber;
					inside = true;
					block = DocExampleBlock{};
					block.FenceLine = lineNumber;
					ENGINE_TRY_ASSIGN(isLuau, ParseInfo(rest.substr(fenceLength), lineNumber, block));
				}
			}
			if (inside)
				return std::unexpected(MakeLineError(openedAt, std::format("the fence opened at line {} is never closed", openedAt)));
			return blocks;
		}

		struct DocExampleChecker::State
		{
			Scope<TypeRegistry> Types{};
			Scope<ScriptApiRegistry> Api{};
			Scope<ScriptTypeChecker> Checker{};
			std::map<VfsPath, std::string> Modules{};
			std::map<std::string, Prelude> Preludes{};
		};

		DocExampleChecker::DocExampleChecker(const std::filesystem::path& fixtureRoot)
			: m_State(CreateScope<State>())
		{
			State& state = *m_State;
			state.Types = CreateBuiltinRegistry();
			state.Api = CreateScope<ScriptApiRegistry>();
			const Status bound = RegisterBindings(*state.Api, *state.Types);
			REQUIRE_MESSAGE(bound.has_value(), (bound ? std::string() : bound.error().ToString()));
			const Status frozen = state.Api->Freeze(*state.Types);
			REQUIRE_MESSAGE(frozen.has_value(), (frozen ? std::string() : frozen.error().ToString()));
			Result<Scope<ScriptTypeChecker>> checker = ScriptTypeChecker::Create(*state.Api);
			REQUIRE_MESSAGE(checker.has_value(), (checker ? std::string() : checker.error().ToString()));
			state.Checker = std::move(*checker);

			const std::filesystem::path assets = fixtureRoot / "Assets";
			const Result<std::vector<std::filesystem::path>> modules = FileSystem::ListDirectory(assets, true);
			REQUIRE_MESSAGE(modules.has_value(), (modules ? std::string() : modules.error().ToString()));
			for (const std::filesystem::path& file : *modules)
			{
				if (file.extension() != std::filesystem::path(".luau"))
					continue;
				std::string relative = FileSystem::PathToUtf8(file.lexically_relative(assets));
				std::ranges::replace(relative, '\\', '/');
				const Result<std::string> source = FileSystem::ReadText(file);
				REQUIRE_MESSAGE(source.has_value(), (source ? std::string() : source.error().ToString()));
				const Result<VfsPath> path = VfsPath::Create("project", "Assets/" + relative);
				REQUIRE_MESSAGE(path.has_value(), relative);
				state.Modules.emplace(*path, *source);
			}

			const Result<std::vector<std::filesystem::path>> preludes = FileSystem::ListDirectory(fixtureRoot / "Preludes", false);
			REQUIRE_MESSAGE(preludes.has_value(), (preludes ? std::string() : preludes.error().ToString()));
			for (const std::filesystem::path& file : *preludes)
			{
				if (file.extension() != std::filesystem::path(".luau"))
					continue;
				const Result<std::string> source = FileSystem::ReadText(file);
				REQUIRE_MESSAGE(source.has_value(), (source ? std::string() : source.error().ToString()));
				Prelude prelude;
				prelude.File = std::format("Tests/Data/TypeChecker/DocExamples/Preludes/{}", FileSystem::PathToUtf8(file.filename()));
				const std::string marker = std::format("{}\n", DocExamplePreludeMarker);
				const size_t found = source->find(marker);
				const bool atLineStart = found != std::string::npos && (found == 0 || (*source)[found - 1] == '\n');
				if (atLineStart && source->find(marker, found + marker.size()) == std::string::npos)
				{
					prelude.Before = source->substr(0, found);
					prelude.After = source->substr(found + marker.size());
					prelude.Valid = true;
				}
				state.Preludes.emplace(FileSystem::PathToUtf8(file.stem()), std::move(prelude));
			}
		}

		DocExampleChecker::~DocExampleChecker() = default;

		Result<std::vector<DocExampleFinding>> DocExampleChecker::Check(const DocExampleBlock& block) const
		{
			const State& state = *m_State;
			std::vector<DocExampleFinding> findings;
			if (block.Skip.has_value())
				return findings;

			const Prelude* prelude = nullptr;
			if (!block.Prelude.empty())
			{
				const auto found = state.Preludes.find(block.Prelude);
				if (found == state.Preludes.end())
					return MakeError(ErrorCode::NotFound, "no prelude '{}' in Tests/Data/TypeChecker/DocExamples/Preludes", block.Prelude);
				if (!found->second.Valid)
					return MakeError(ErrorCode::Validation, "the prelude '{}' needs exactly one '{}' line", block.Prelude, DocExamplePreludeMarker);
				prelude = &found->second;
			}
			const std::string before = prelude != nullptr ? prelude->Before : std::string();
			const std::string after = prelude != nullptr ? prelude->After : std::string();
			const std::string combined = before + block.Source + after;
			const uint32_t preludeLines = static_cast<uint32_t>(std::ranges::count(before, '\n'));
			const uint32_t blockLines = static_cast<uint32_t>(std::ranges::count(block.Source, '\n'));

			ENGINE_TRY_ASSIGN(const VfsPath root, VfsPath::Create("project", block.Path));
			StubModuleReader reader(state.Modules);
			for (ScriptDiagnostic& diagnostic : state.Checker->CheckScript({ .Path = root, .Source = combined, .Modules = &reader }))
			{
				DocExampleFinding finding;
				if (diagnostic.File != block.Path)
				{
					finding.Elsewhere = std::format("{}:{}:{}", diagnostic.File, diagnostic.Line, diagnostic.Column);
				}
				else if (diagnostic.Line > preludeLines && diagnostic.Line <= preludeLines + blockLines)
				{
					finding.DocumentLine = block.FenceLine + (diagnostic.Line - preludeLines);
				}
				else
				{
					// The prelude's own line: the example replaces its marker line.
					const uint32_t line = diagnostic.Line <= preludeLines ? diagnostic.Line : diagnostic.Line - blockLines + 1;
					finding.Elsewhere = std::format("{}:{}:{}", prelude != nullptr ? prelude->File : block.Path, line, diagnostic.Column);
				}
				finding.Diagnostic = std::move(diagnostic);
				findings.push_back(std::move(finding));
			}
			return findings;
		}

	}

}
