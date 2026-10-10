#include "EnginePCH.h"
#include "Engine/Scripting/ScriptCompiler.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Scripting/Private/SandboxAccess.h"

#if !defined(ENGINE_DIST)
	#include <Luau/Compiler.h>
	#include <Luau/Parser.h>
#endif

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <string>

namespace Engine {

#if !defined(ENGINE_DIST)
	namespace {

		ErrorLocation CompileLocation(const ScriptSourceMap& map, uint32_t line, uint32_t column)
		{
			ErrorLocation location{};
			location.File = map.Path.empty() ? map.ChunkName.substr(1) : map.Path;
			if (!map.JsonPointer.empty())
				location.JsonPointer = map.JsonPointer;
			if (line > map.GeneratedPrefixLines)
			{
				location.Line = line - map.GeneratedPrefixLines;
				location.Column = column;
				if (location.Line > map.LineOffsets.size())
				{
					location.Line = static_cast<uint32_t>(map.LineOffsets.size());
					location.Column = map.SourceByteCount - map.LineOffsets.back() + 1;
				}
			}
			return location;
		}

		Status ValidateCompileRequest(const ScriptCompileRequest& request)
		{
			if (!request.Path.IsEmpty() && (request.Path.GetScheme() != "project" || !request.Path.GetPath().starts_with("Assets/")))
				return MakeError(ErrorCode::Validation, "script diagnostic origin must be below project://Assets");
			if (request.Source.size() > std::numeric_limits<uint32_t>::max() - 16 || !IsValidUtf8(request.Source))
				return MakeError(ErrorCode::Validation, "script source is too large or is not valid UTF-8");
			if (request.Mode != ScriptCompileMode::Chunk && request.Mode != ScriptCompileMode::Expression && request.Mode != ScriptCompileMode::ExpressionOrChunk)
				return MakeError(ErrorCode::Validation, "unknown script compilation mode");
			const auto hasControl = [](std::string_view text)
			{
				return std::any_of(text.begin(), text.end(), [](unsigned char character)
				{
					return character < 32 || character == 127;
				});
			};
			if (!IsValidUtf8(request.ChunkName) || hasControl(request.ChunkName))
				return MakeError(ErrorCode::Validation, "script chunk name is not a valid diagnostic label");
			if (!request.ChunkName.empty())
			{
				if (request.ChunkName.size() < 2 || (request.ChunkName.front() != '@' && request.ChunkName.front() != '='))
					return MakeError(ErrorCode::Validation, "script chunk name must begin with @ or =");
				if (request.ChunkName.front() == '@')
				{
					ENGINE_TRY_ASSIGN(auto origin, VfsPath::Create("project", request.ChunkName.substr(1)));
					if (!origin.GetPath().starts_with("Assets/"))
						return MakeError(ErrorCode::Validation, "file chunk name must be below Assets");
				}
			}
			if (!IsValidUtf8(request.JsonPointer) || request.JsonPointer.find('\0') != std::string_view::npos || (!request.JsonPointer.empty() && request.JsonPointer.front() != '/'))
				return MakeError(ErrorCode::Validation, "script source pointer must be an RFC 6901 JSON pointer");
			for (size_t index = 0; index < request.JsonPointer.size(); ++index)
				if (request.JsonPointer[index] == '~' && (++index == request.JsonPointer.size() || (request.JsonPointer[index] != '0' && request.JsonPointer[index] != '1')))
					return MakeError(ErrorCode::Validation, "invalid escape in script source JSON pointer");
			return {};
		}

		bool IsCompleteExpression(std::string_view source, const ScriptSourceMap& map)
		{
			Luau::Allocator allocator;
			Luau::AstNameTable names(allocator);
			const auto expression = Luau::Parser::parseExpr(source.data(), source.size(), names, allocator);
			if (!expression.root || !expression.errors.empty())
				return false;
			const auto end = expression.root->location.end;
			if (end.line >= map.LineOffsets.size())
				return false;
			const uint64_t endOffset = static_cast<uint64_t>(map.LineOffsets[end.line]) + end.column;
			if (endOffset > source.size())
				return false;
			// The pinned parseExpr helper advances once before checking EOF. Verify the actual expression suffix too.
			const auto suffix = source.substr(static_cast<size_t>(endOffset));
			Luau::Lexer lexer(suffix.data(), suffix.size(), names);
			lexer.setSkipComments(true);
			return lexer.next().type == Luau::Lexeme::Eof;
		}

	}
#endif

	Result<ScriptCompilation> ScriptCompiler::Compile(const ScriptCompileRequest& request)
	{
#if defined(ENGINE_DIST)
		static_cast<void>(request);
		return MakeError(ErrorCode::Unsupported, "source compilation is unavailable in Dist");
#else
		if (!Detail::IsScriptingRuntimeInitialized())
			return MakeError(ErrorCode::InvalidState, "scripting runtime must be initialized before compilation");
		ENGINE_TRY(ValidateCompileRequest(request));
		ScriptCompilation result{};
		auto& map = result.SourceMap;
		map.Path = request.Path.GetPath();
		map.ChunkName = request.ChunkName.empty() ? (map.Path.empty() ? "=eval" : "@" + map.Path) : std::string(request.ChunkName);
		map.JsonPointer = request.JsonPointer;
		map.SourceHash = XXH64(request.Source);
		map.SourceByteCount = static_cast<uint32_t>(request.Source.size());
		map.LineOffsets.push_back(0);
		for (size_t index = 0; index < request.Source.size(); ++index)
			if (request.Source[index] == '\n')
				map.LineOffsets.push_back(static_cast<uint32_t>(index + 1));
		bool expression = false;
		if (request.Mode != ScriptCompileMode::Chunk)
			expression = IsCompleteExpression(request.Source, map);
		if (!expression && request.Mode == ScriptCompileMode::Expression)
		{
			// Parsing the wrapped form supplies an authored location even when parseExpr only consumed a prefix.
			map.GeneratedPrefixLines = 1;
		}
		else
			map.GeneratedPrefixLines = expression ? 1u : 0u;
		const std::string source = map.GeneratedPrefixLines ? "return (\n" + std::string(request.Source) + "\n)" : std::string(request.Source);
		Luau::Allocator allocator;
		Luau::AstNameTable names(allocator);
		const auto parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
		if (!parsed.errors.empty())
		{
			const auto& error = parsed.errors.front();
			const auto position = error.getLocation().begin;
			const bool generatedSuffix = position.line + 1 > map.GeneratedPrefixLines + map.LineOffsets.size();
			return std::unexpected(Error(ErrorCode::CompileFailed, error.getMessage())
					.WithLocation(CompileLocation(map, position.line + 1, position.column + 1))
					.WithContext(generatedSuffix ? "at authored EOF in the generated expression wrapper" : "while compiling script source"));
		}
		if (!expression && request.Mode == ScriptCompileMode::Expression)
			return std::unexpected(Error(ErrorCode::CompileFailed, "expected exactly one complete expression")
					.WithLocation(CompileLocation(map, map.GeneratedPrefixLines + 1, 1)));
		const char* const mutableGlobals[] = { "math", nullptr };
		Luau::CompileOptions options{};
		options.optimizationLevel = 1;
		options.debugLevel = 1;
		options.typeInfoLevel = 0;
		options.coverageLevel = 0;
		if (map.GeneratedPrefixLines != 0)
		{
			// The wrapper moves authored header comments below a token. Preserve their option effects using the
			// public parser's original header classification. Native code generation remains disabled by the engine.
			Luau::Allocator authoredAllocator;
			Luau::AstNameTable authoredNames(authoredAllocator);
			const auto authored = Luau::Parser::parseExpr(request.Source.data(), request.Source.size(), authoredNames, authoredAllocator);
			for (const auto& comment : authored.hotcomments)
			{
				if (comment.header && comment.content.starts_with("optimize "))
					options.optimizationLevel = std::clamp(std::atoi(comment.content.c_str() + 9), 0, 2);
				if (comment.header && comment.content == "native")
				{
					options.optimizationLevel = 2;
					options.typeInfoLevel = 1;
				}
			}
		}
		options.mutableGlobals = mutableGlobals;
		const std::string bytecode = Luau::compile(source, options);
		if (bytecode.empty() || bytecode.front() == '\0')
		{
			// Compiler-only resource/semantic diagnostics use the documented error bytecode envelope.
			std::string_view message = bytecode.empty() ? "compiler returned no bytecode" : std::string_view(bytecode).substr(1);
			uint32_t line = 0;
			if (message.starts_with(":"))
			{
				const auto parsedLine = std::from_chars(message.data() + 1, message.data() + message.size(), line);
				if (parsedLine.ec == std::errc{} && parsedLine.ptr != message.data() + message.size() && *parsedLine.ptr == ':')
					message.remove_prefix(static_cast<size_t>(parsedLine.ptr - message.data()) + 1);
			}
			return std::unexpected(Error(ErrorCode::CompileFailed, std::string(message)).WithLocation(CompileLocation(map, line, 0)));
		}
		const auto bytes = AsBytes(bytecode);
		result.Bytecode.assign(bytes.begin(), bytes.end());
		return result;
#endif
	}

}
