#include "EditorPCH.h"
#include "EditorCore/Scripting/ScriptTypeChecker.h"

#include "EditorCore/Scripting/Private/ScriptValidation.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scripting/RequireResolver.h"
#include "Engine/Scripting/Sandbox.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <Luau/Allocator.h>
#include <Luau/Ast.h>
#include <Luau/BuiltinDefinitions.h>
#include <Luau/Config.h>
#include <Luau/ConfigResolver.h>
#include <Luau/Error.h>
#include <Luau/FileResolver.h>
#include <Luau/Frontend.h>
#include <Luau/Parser.h>
#include <Luau/TypeArena.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace Engine {

	namespace Utils {

		static std::string CheckerDisplayPath(std::string_view name)
		{
			const auto path = VfsPath::Parse(name);
			return path ? std::string(path->GetPath()) : std::string(name);
		}

		static ScriptDiagnostic CheckerError(std::string_view file, std::string message)
		{
			return { .Severity = DiagnosticSeverity::Error, .Code = "SCRIPT_TYPE_ERROR", .File = CheckerDisplayPath(file), .Line = 1, .Column = 1, .Message = std::move(message) };
		}

		static void SetCheckerRange(ScriptDiagnostic& diagnostic, const Luau::Location& location)
		{
			const auto position = [](unsigned int value)
			{
				return value == std::numeric_limits<unsigned int>::max() ? 0u : static_cast<uint32_t>(value) + 1;
			};
			diagnostic.Line = position(location.begin.line);
			diagnostic.Column = position(location.begin.column);
			diagnostic.EndLine = position(location.end.line);
			diagnostic.EndColumn = position(location.end.column);
		}

		static void CanonicalizeCheckerFindings(std::vector<ScriptDiagnostic>& findings)
		{
			std::sort(findings.begin(), findings.end(), [](const ScriptDiagnostic& left, const ScriptDiagnostic& right)
			{
				return std::tie(left.File, left.Line, left.Column, left.Code, left.Message, left.EndLine, left.EndColumn, left.Severity) < std::tie(right.File, right.Line, right.Column, right.Code, right.Message, right.EndLine, right.EndColumn, right.Severity);
			});
			findings.erase(std::unique(findings.begin(), findings.end()), findings.end());
		}

		class InputActionVisitor final : public Luau::AstVisitor
		{
		public:
			InputActionVisitor(std::span<const std::string> actions, std::string_view file, std::vector<ScriptDiagnostic>& diagnostics)
				: m_Actions(actions), m_File(file), m_Diagnostics(diagnostics)
			{
			}

			bool visit(Luau::AstExprCall* call) override
			{
				if (call->self || call->args.size == 0)
					return true;
				Luau::AstExpr* receiver = nullptr;
				std::string_view method;
				if (const auto* named = Unwrap(call->func)->as<Luau::AstExprIndexName>())
				{
					receiver = named->expr;
					method = named->index.value;
				}
				else if (const auto* indexed = Unwrap(call->func)->as<Luau::AstExprIndexExpr>())
				{
					const auto* name = Unwrap(indexed->index)->as<Luau::AstExprConstantString>();
					if (name != nullptr)
					{
						receiver = indexed->expr;
						method = std::string_view(name->value.data, name->value.size);
					}
				}
				const auto* global = receiver == nullptr ? nullptr : Unwrap(receiver)->as<Luau::AstExprGlobal>();
				const auto* name = Unwrap(call->args.data[0])->as<Luau::AstExprConstantString>();
				if (global == nullptr || std::string_view(global->name.value) != "Input" || name == nullptr
					|| (!method.contains("Action") && method != "GetAxis"))
					return true;
				const std::string action(name->value.data, name->value.size);
				if (std::ranges::find(m_Actions, action) != m_Actions.end())
					return true;
				ScriptDiagnostic finding{ .Severity = DiagnosticSeverity::Error, .Code = "INPUT_UNKNOWN_ACTION", .File = std::string(m_File), .Message = std::format("Input.{} names unknown action '{}'", method, action) };
				SetCheckerRange(finding, name->location);
				m_Diagnostics.push_back(std::move(finding));
				return true;
			}
		private:
			static Luau::AstExpr* Unwrap(Luau::AstExpr* expression)
			{
				while (true)
				{
					if (auto* group = expression->as<Luau::AstExprGroup>())
						expression = group->expr;
					else if (auto* assertion = expression->as<Luau::AstExprTypeAssertion>())
						expression = assertion->expr;
					else
						return expression;
				}
			}
			std::span<const std::string> m_Actions; // Borrowed only during this AST visit.
			std::string_view m_File;
			std::vector<ScriptDiagnostic>& m_Diagnostics; // Borrowed output of this synchronous visit.
		};

		class CheckerFileResolver final : public Luau::FileResolver
		{
		public:
			CheckerFileResolver(const ScriptCheckRequest& request, std::vector<ScriptDiagnostic>& findings)
				: m_Request(request), m_Findings(findings)
			{
			}

			std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override
			{
				if (const auto cached = m_Sources.find(name); cached != m_Sources.end())
					return cached->second;
				if (name == m_Request.Path.ToString())
				{
					Luau::SourceCode source{ std::string(m_Request.Source), Luau::SourceCode::Module };
					m_Sources.emplace(name, source);
					return source;
				}
				const auto path = VfsPath::Parse(name);
				if (!path)
				{
					m_Findings.push_back(CheckerError(name, path.error().GetMessageText()));
					m_Sources.emplace(name, std::nullopt);
					return std::nullopt;
				}
				auto source = m_Resolver.ReadSource(*m_Request.Modules, *path);
				if (!source || !IsValidUtf8(*source))
				{
					m_Findings.push_back(CheckerError(name, source ? "required source is not UTF-8" : source.error().GetMessageText()));
					m_Sources.emplace(name, std::nullopt);
					return std::nullopt;
				}
				Luau::SourceCode code{ std::move(*source), Luau::SourceCode::Module };
				m_Sources.emplace(name, code);
				return code;
			}

			std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* expression,
				const Luau::TypeCheckLimits& /*limits*/) override
			{
				if (context == nullptr || expression == nullptr)
					return std::nullopt;
				const auto* literal = expression->as<Luau::AstExprConstantString>();
				if (literal == nullptr)
					return std::nullopt;
				const auto importer = VfsPath::Parse(context->name);
				if (!importer)
					return std::nullopt;
				const auto resolved = m_Resolver.Resolve(*importer, std::string_view(literal->value.data, literal->value.size));
				if (!resolved)
				{
					auto diagnostic = CheckerError(context->name, resolved.error().GetMessageText());
					SetCheckerRange(diagnostic, expression->location);
					m_Findings.push_back(std::move(diagnostic));
					return std::nullopt;
				}
				return Luau::ModuleInfo{ resolved->ToString(), false };
			}

			std::string getHumanReadableModuleName(const Luau::ModuleName& name) const override
			{
				return CheckerDisplayPath(name);
			}

			void ReportCycles()
			{
				// Kahn's algorithm bounds traversal without recursive native stack growth on an untrusted graph.
				std::map<std::string, std::set<std::string>> graph;
				std::map<std::string, size_t> incoming;
				for (const auto& edge : m_Resolver.GetRequires())
				{
					incoming.try_emplace(edge.From, 0);
					incoming.try_emplace(edge.Path, 0);
					if (graph[edge.From].insert(edge.Path).second)
						++incoming[edge.Path];
				}
				std::set<std::string> ready;
				for (const auto& [name, count] : incoming)
				{
					if (count == 0)
						ready.insert(name);
				}
				while (!ready.empty())
				{
					const auto name = *ready.begin();
					ready.erase(ready.begin());
					for (const auto& dependency : graph[name])
					{
						if (--incoming[dependency] == 0)
							ready.insert(dependency);
					}
					incoming.erase(name);
				}
				if (!incoming.empty())
					m_Findings.push_back(CheckerError(incoming.begin()->first, "cyclic script require graph"));
			}
		private:
			const ScriptCheckRequest& m_Request; // borrowed until this synchronous check returns
			std::vector<ScriptDiagnostic>& m_Findings;
			RequireResolver m_Resolver{};
			std::map<std::string, std::optional<Luau::SourceCode>, std::less<>> m_Sources{};
		};

		class CheckerConfigResolver final : public Luau::ConfigResolver
		{
		public:
			CheckerConfigResolver(const ScriptTypeCheckerConfiguration& configuration, std::vector<ScriptDiagnostic>& findings)
				: m_Configuration(configuration), m_Findings(findings)
			{
				m_Default.mode = Luau::Mode::Strict;
			}

			const Luau::Config& getConfig(const Luau::ModuleName& name, const Luau::TypeCheckLimits& /*limits*/) const override
			{
				if (const auto found = m_Configs.find(name); found != m_Configs.end())
					return found->second;
				const auto path = VfsPath::Parse(name);
				if (!path)
					return m_Default;
				Luau::Config config = m_Default;
				for (const auto& file : m_Configuration.Files)
				{
					if (!file.Source || !path->IsUnder(file.Path.GetParent()))
						continue;
					Luau::Config next = config;
					if (const auto error = Luau::parseConfig(*file.Source, next))
						m_Findings.push_back(CheckerError(file.Path.ToString(), "invalid .luaurc: " + *error));
					else
						config = std::move(next);
				}
				// Resolver confinement is independent of .luaurc aliases; none grant access beyond ./ and ../ under Assets.
				return m_Configs.emplace(name, std::move(config)).first->second;
			}
		private:
			const ScriptTypeCheckerConfiguration& m_Configuration; // owned by the checker, borrowed for this call
			std::vector<ScriptDiagnostic>& m_Findings;
			Luau::Config m_Default{};
			mutable std::map<std::string, Luau::Config, std::less<>> m_Configs{};
		};

		static Status LoadCheckerDefinitions(Luau::Frontend& frontend, const std::string& definitions)
		{
			Luau::registerBuiltinGlobals(frontend, frontend.globals);
			const auto loaded = frontend.loadDefinitionFile(frontend.globals, frontend.globals.globalScope, definitions, "Engine.d.luau", false);
			if (!loaded.success)
			{
				std::string message = "generated Engine.d.luau definitions are invalid";
				if (!loaded.parseResult.errors.empty())
				{
					const auto& error = loaded.parseResult.errors.front();
					message += std::format(" at {}:{}: {}", error.getLocation().begin.line + 1, error.getLocation().begin.column + 1, error.getMessage());
				}
				else if (loaded.module && !loaded.module->errors.empty())
				{
					for (size_t index = 0; index < std::min(static_cast<size_t>(8), loaded.module->errors.size()); ++index)
					{
						const auto& error = loaded.module->errors[index];
						message += std::format(" at {}:{}: {}", error.location.begin.line + 1, error.location.begin.column + 1, Luau::toString(error));
					}
				}
				return MakeError(ErrorCode::Validation, "{}", message);
			}
			Luau::freeze(frontend.globals.globalTypes);
			return {};
		}

		static Status ValidateCheckerConfiguration(const ScriptTypeCheckerConfiguration& configuration)
		{
			VfsPath previous;
			for (const auto& file : configuration.Files)
			{
				if (file.Path.GetScheme() != "project" || file.Path.GetFileName() != ".luaurc" || (!file.Path.GetParent().IsRoot() && file.Path.GetParent().GetPath() != "Assets" && !file.Path.GetParent().GetPath().starts_with("Assets/")) || (!previous.IsEmpty() && previous >= file.Path) || (file.Source && (!IsValidUtf8(*file.Source) || file.Source->size() > std::numeric_limits<uint32_t>::max())))
					return MakeError(ErrorCode::Validation, "type checker configuration must contain unique sorted project .luaurc snapshots");
				previous = file.Path;
			}
			return {};
		}

	}

	struct ScriptTypeChecker::State
	{
		std::string Definitions{};
		ScriptTypeCheckerConfiguration Configuration{};
		uint64_t EnvironmentHash = 0;
	};

	ScriptTypeChecker::ScriptTypeChecker(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	ScriptTypeChecker::~ScriptTypeChecker() = default;

	Result<ScriptTypeCheckerConfiguration> ScriptTypeChecker::CaptureConfiguration(const VirtualFileSystem& vfs)
	{
		ENGINE_TRY_ASSIGN(const auto root, VfsPath::Parse("project://"));
		ENGINE_TRY_ASSIGN(const auto assets, root.Join("Assets"));
		std::set<VfsPath> directories{ root, assets };
		auto entries = vfs.List(assets, true);
		if (!entries && entries.error().GetCode() != ErrorCode::NotFound)
			return std::unexpected(std::move(entries).error());
		if (entries)
		{
			for (const auto& entry : *entries)
			{
				if (entry.Info.IsDirectory)
					directories.insert(entry.Path);
			}
		}
		ScriptTypeCheckerConfiguration configuration;
		for (const auto& directory : directories)
		{
			ENGINE_TRY_ASSIGN(auto path, directory.Join(".luaurc"));
			auto text = vfs.ReadText(path);
			if (!text && text.error().GetCode() != ErrorCode::NotFound)
				return std::unexpected(std::move(text).error());
			ScriptTypeCheckerConfigFile file{ .Path = std::move(path) };
			if (text)
				file.Source = std::move(*text);
			configuration.Files.push_back(std::move(file));
		}
		std::sort(configuration.Files.begin(), configuration.Files.end(), [](const auto& left, const auto& right)
		{
			return left.Path < right.Path;
		});
		ENGINE_TRY(Utils::ValidateCheckerConfiguration(configuration));
		return configuration;
	}

	Result<Scope<ScriptTypeChecker>> ScriptTypeChecker::Create(const ScriptApiRegistry& api, ScriptTypeCheckerConfiguration configuration)
	{
		if (!IsScriptingRuntimeInitialized() || !api.IsFrozen())
			return MakeError(ErrorCode::InvalidState, "type checker requires initialized scripting and a frozen API registry");
		ENGINE_TRY(Utils::ValidateCheckerConfiguration(configuration));
		ENGINE_TRY_ASSIGN(std::string definitions, api.GenerateDefinitions());
		if (!IsValidUtf8(definitions) || definitions.size() > std::numeric_limits<uint32_t>::max())
			return MakeError(ErrorCode::Validation, "generated definitions must be bounded UTF-8");
		try
		{
			Luau::NullFileResolver files;
			Luau::NullConfigResolver configs;
			configs.defaultConfig.mode = Luau::Mode::Strict;
			Luau::Frontend frontend(Luau::SolverMode::New, &files, &configs);
			ENGINE_TRY(Utils::LoadCheckerDefinitions(frontend, definitions));
		}
		catch (const Luau::InternalCompilerError& error)
		{
			return MakeError(ErrorCode::Validation, "definition analysis failed: {}", error.what());
		}
		BinaryWriter fingerprint;
		fingerprint.WriteString("Luau-0.741/New/Strict/RelativeAssets/v1");
		fingerprint.WriteString(definitions);
		for (const auto& file : configuration.Files)
		{
			fingerprint.WriteString(file.Path.ToString());
			fingerprint.WriteBool(file.Source.has_value());
			if (file.Source)
				fingerprint.WriteString(*file.Source);
		}
		auto checker = CreateScope<ScriptTypeChecker>(ConstructionKey{});
		checker->m_State->Definitions = std::move(definitions);
		checker->m_State->Configuration = std::move(configuration);
		checker->m_State->EnvironmentHash = XXH64(fingerprint.GetData());
		return checker;
	}

	uint64_t ScriptTypeChecker::GetEnvironmentHash() const
	{
		return m_State->EnvironmentHash;
	}

	std::vector<ScriptDiagnostic> ScriptTypeChecker::CheckScript(const ScriptCheckRequest& request)
	{
		std::vector<ScriptDiagnostic> findings;
		const std::string root = request.Path.ToString();
		if (!IsScriptingRuntimeInitialized() || request.Modules == nullptr || request.Path.GetScheme() != "project" || !request.Path.GetPath().starts_with("Assets/") || request.Path.GetExtension() != ".luau" || !IsValidUtf8(request.Source) || request.Source.size() > std::numeric_limits<uint32_t>::max())
			return { Utils::CheckerError(root, "invalid script check request or scripting runtime is not initialized") };
		Utils::CheckerFileResolver files(request, findings);
		Utils::CheckerConfigResolver configs(m_State->Configuration, findings);
		try
		{
			Luau::FrontendOptions options;
			options.moduleTimeLimitSec = 1.0;
			options.runLintChecks = true;
			Luau::Frontend frontend(Luau::SolverMode::New, &files, &configs, options);
			const auto definitions = Utils::LoadCheckerDefinitions(frontend, m_State->Definitions);
			if (!definitions)
				return { Utils::CheckerError(root, definitions.error().GetMessageText()) };
			const auto result = frontend.check(root);
			for (const auto& error : result.errors)
			{
				auto diagnostic = Utils::CheckerError(error.moduleName.empty() ? root : error.moduleName, Luau::toString(error, { &files }));
				Utils::SetCheckerRange(diagnostic, error.location);
				findings.push_back(std::move(diagnostic));
			}
			for (const auto& name : result.timeoutHits)
				findings.push_back(Utils::CheckerError(name, "script analysis exceeded its safety limit"));
			for (const auto& warning : result.lintResult.errors)
			{
				auto diagnostic = Utils::CheckerError(root, warning.text);
				Utils::SetCheckerRange(diagnostic, warning.location);
				findings.push_back(std::move(diagnostic));
			}
			for (const auto& warning : result.lintResult.warnings)
			{
				auto diagnostic = Utils::CheckerError(root, warning.text);
				diagnostic.Severity = DiagnosticSeverity::Warning;
				Utils::SetCheckerRange(diagnostic, warning.location);
				findings.push_back(std::move(diagnostic));
			}
			files.ReportCycles();
		}
		catch (const Luau::InternalCompilerError& error)
		{
			findings.push_back(Utils::CheckerError(error.moduleName.value_or(root), std::string("script analysis failed: ") + error.what()));
		}
		Utils::CanonicalizeCheckerFindings(findings);
		return findings;
	}

	namespace Detail {

		std::vector<ScriptDiagnostic> ValidateScriptSource(std::string_view file, std::string_view source, std::span<const std::string> inputActions)
		{
			Luau::Allocator allocator;
			Luau::AstNameTable names(allocator);
			const Luau::ParseResult parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
			std::vector<ScriptDiagnostic> diagnostics;
			for (const auto& error : parsed.errors)
			{
				ScriptDiagnostic finding{ .Severity = DiagnosticSeverity::Error, .Code = "SCRIPT_COMPILE_ERROR", .File = std::string(file), .Message = error.getMessage() };
				Utils::SetCheckerRange(finding, error.getLocation());
				diagnostics.push_back(std::move(finding));
			}
			if (parsed.root != nullptr)
			{
				Utils::InputActionVisitor visitor(inputActions, file, diagnostics);
				parsed.root->visit(&visitor);
			}
			Utils::CanonicalizeCheckerFindings(diagnostics);
			return diagnostics;
		}

	}

}
