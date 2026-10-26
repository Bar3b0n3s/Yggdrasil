#include "EnginePCH.h"
#include "Engine/Scripting/ScriptAssetLoads.h"

#include "Engine/Core/Utf8.h"

#include <Luau/Ast.h>
#include <Luau/Parser.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// The scan walks Luau's syntax tree (Luau/Parser.h), which binds every name: a use of the global Assets is an
// AstExprGlobal, while a local or parameter named Assets is an AstExprLocal and a field named Assets an AstExprIndexName's
// index, so neither counts. Type annotations are AstType nodes, which Luau's AstVisitor does not enter by default, so
// typeof(Assets.Load) in an annotation is not a use either. The sandbox keeps the globals table _G (Sandbox.cpp removes
// getfenv and setfenv, never _G), and the base library's _G._G is _G again, so the globals table is _G or its field _G by
// a constant name at any depth (_G._G, _G["_G"]): its field Assets is the global Assets too, and the table reached any
// other way (_G[<expression>], _G._G on its own, passed, stored, rawget(_G, ...), a method call _G:rawget(...)) may reach
// it. The visitor handles both globals where they appear below a call, a field access or an index, and every other place
// they appear is a computed use.

namespace Engine {

	namespace {

		constexpr char AssetsGlobal[] = "Assets";
		constexpr char GlobalsTable[] = "_G";
		constexpr char LoadFunction[] = "Load";

		// `expression` without the parentheses and type assertions around it.
		[[nodiscard]] const Luau::AstExpr* StripWrappers(const Luau::AstExpr* expression)
		{
			while (true)
			{
				if (const auto* group = expression->as<Luau::AstExprGroup>())
					expression = group->expr;
				else if (const auto* assertion = expression->as<Luau::AstExprTypeAssertion>())
					expression = assertion->expr;
				else
					return expression;
			}
		}

		// The text of a string literal: a plain or long string, or an interpolated string without expressions.
		[[nodiscard]] std::optional<std::string> ReadStringLiteral(const Luau::AstExpr* expression)
		{
			expression = StripWrappers(expression);
			if (const auto* constant = expression->as<Luau::AstExprConstantString>())
				return std::string(constant->value.data, constant->value.size);
			if (const auto* interpolated = expression->as<Luau::AstExprInterpString>(); interpolated != nullptr && interpolated->expressions.size == 0
				&& interpolated->strings.size == 1)
			{
				return std::string(interpolated->strings.data[0].data, interpolated->strings.data[0].size);
			}
			return std::nullopt;
		}

		[[nodiscard]] bool IsGlobalsTable(const Luau::AstExpr* expression);

		// The field of the globals table that `expression` reads by a constant name: <globals>.<name> or
		// <globals>["<name>"]; nullopt for anything else.
		[[nodiscard]] std::optional<std::string_view> ReadGlobalsField(const Luau::AstExpr* expression)
		{
			if (const auto* field = expression->as<Luau::AstExprIndexName>(); field != nullptr && IsGlobalsTable(field->expr))
				return std::string_view(field->index.value);
			if (const auto* index = expression->as<Luau::AstExprIndexExpr>(); index != nullptr && IsGlobalsTable(index->expr))
			{
				if (const auto* key = StripWrappers(index->index)->as<Luau::AstExprConstantString>())
					return std::string_view(key->value.data, key->value.size);
			}
			return std::nullopt;
		}

		// Whether `expression`, possibly in parentheses or under a type assertion, is the globals table: the global _G, or
		// its field _G read by a constant name, at any depth (_G._G, _G["_G"]), since the base library's _G._G is _G. The
		// depth is bounded by the parser's recursion limit, which suffix chains count against.
		[[nodiscard]] bool IsGlobalsTable(const Luau::AstExpr* expression)
		{
			expression = StripWrappers(expression);
			if (const auto* global = expression->as<Luau::AstExprGlobal>())
				return global->name == GlobalsTable;
			return ReadGlobalsField(expression) == std::string_view(GlobalsTable);
		}

		// Whether `expression`, possibly in parentheses or under a type assertion, is the global Assets: by its name, or as
		// the globals table's field Assets (_G.Assets, _G["Assets"], _G._G.Assets).
		[[nodiscard]] bool IsAssetsGlobal(const Luau::AstExpr* expression)
		{
			expression = StripWrappers(expression);
			if (const auto* global = expression->as<Luau::AstExprGlobal>())
				return global->name == AssetsGlobal;
			return ReadGlobalsField(expression) == std::string_view(AssetsGlobal);
		}

		// Whether a field of the globals table named `name`, reached by itself (not below a field access or a call), hands
		// on a table that may reach Assets: Assets itself, or the globals table again (_G._G).
		[[nodiscard]] bool IsHandedOnGlobalsField(std::string_view name)
		{
			return name == AssetsGlobal || name == GlobalsTable;
		}

		// Whether `expression` is Assets.Load or Assets:Load on the global Assets.
		[[nodiscard]] bool IsAssetsLoad(const Luau::AstExpr* expression)
		{
			const auto* field = expression->as<Luau::AstExprIndexName>();
			return field != nullptr && IsAssetsGlobal(field->expr) && field->index == LoadFunction;
		}

		class AssetLoadVisitor final : public Luau::AstVisitor
		{
		public:
			explicit AssetLoadVisitor(std::vector<ScriptAssetLoad>& loads)
				: m_Loads(loads)
			{
			}

			bool visit(Luau::AstExprCall* node) override
			{
				if (IsAssetsLoad(node->func))
				{
					// Assets.Load(<literal>): a literal load. With another argument, none, more than one, or called as a method
					// (which passes Assets itself), the reference is computed.
					std::optional<std::string> literal;
					if (!node->self && node->args.size == 1)
						literal = ReadStringLiteral(node->args.data[0]);
					Add(node->location, std::move(literal));
					VisitArguments(*node);
					return false;
				}
				// <globals>:<name>(...) passes the globals table to the function it reads, as _G:rawget("Assets") does.
				if (const auto* method = node->func->as<Luau::AstExprIndexName>(); node->self && method != nullptr && IsGlobalsTable(method->expr))
				{
					Add(node->location, std::nullopt);
					VisitArguments(*node);
					return false;
				}
				return true;
			}

			bool visit(Luau::AstExprIndexName* node) override
			{
				if (IsAssetsGlobal(node->expr))
				{
					// Assets.<field>: a use by a field name, except Assets.Load read as a value (outside a call), whose calls the
					// scan cannot follow.
					if (node->index == LoadFunction)
						Add(node->location, std::nullopt);
					return false;
				}
				// <globals>.<name>: <globals>.Assets or <globals>._G reached here (not below a field access or a call) hands
				// on the global Assets or the globals table; any other field of the globals table is not Assets.
				if (IsGlobalsTable(node->expr))
				{
					if (IsHandedOnGlobalsField(node->index.value))
						Add(node->location, std::nullopt);
					return false;
				}
				return true;
			}

			bool visit(Luau::AstExprIndexExpr* node) override
			{
				if (IsAssetsGlobal(node->expr))
				{
					// Assets["Load"] or Assets[<expression>]: the key is a value, so the scan cannot tell which function it reads.
					Add(node->location, std::nullopt);
					node->index->visit(this);
					return false;
				}
				if (IsGlobalsTable(node->expr))
				{
					// <globals>["<name>"] reads that field, as <globals>.<name> does; <globals>[<expression>] may read Assets.
					const std::optional<std::string_view> name = ReadGlobalsField(node);
					if (!name.has_value() || IsHandedOnGlobalsField(*name))
						Add(node->location, std::nullopt);
					node->index->visit(this);
					return false;
				}
				return true;
			}

			bool visit(Luau::AstExprGlobal* node) override
			{
				// Every place the visitor reaches either global without one of the handlers above: Assets passed, stored or
				// returned, and _G handed on (local g = _G, rawget(_G, "Assets"), setmetatable(_G, ...)), which may reach Assets.
				if (node->name == AssetsGlobal || node->name == GlobalsTable)
					Add(node->location, std::nullopt);
				return false;
			}
		private:
			void VisitArguments(Luau::AstExprCall& call)
			{
				for (Luau::AstExpr* argument : call.args)
					argument->visit(this);
			}

			void Add(const Luau::Location& location, std::optional<std::string> literal)
			{
				ScriptAssetLoad load;
				load.Literal = literal.has_value();
				load.Reference = std::move(literal).value_or(std::string());
				load.Line = location.begin.line + 1;
				load.Column = location.begin.column + 1;
				m_Loads.push_back(std::move(load));
			}
		private:
			std::vector<ScriptAssetLoad>& m_Loads; // the caller's output
		};

	}

	Result<std::vector<ScriptAssetLoad>> FindScriptAssetLoads(std::string_view source, std::string_view chunkName)
	{
		if (!IsValidUtf8(source))
		{
			ErrorLocation location;
			location.File = std::string(chunkName);
			return std::unexpected(Error(ErrorCode::Validation, "the script source is not valid UTF-8").WithLocation(std::move(location)));
		}
		Luau::Allocator allocator;
		Luau::AstNameTable names(allocator);
		const Luau::ParseResult parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
		if (!parsed.errors.empty())
		{
			// The first syntax error, as ScriptCompiler reports it.
			const Luau::ParseError& error = parsed.errors.front();
			ErrorLocation location;
			location.File = std::string(chunkName);
			location.Line = error.getLocation().begin.line + 1;
			location.Column = error.getLocation().begin.column + 1;
			return std::unexpected(Error(ErrorCode::CompileFailed, error.getMessage()).WithLocation(std::move(location)));
		}

		std::vector<ScriptAssetLoad> loads;
		AssetLoadVisitor visitor(loads);
		parsed.root->visit(&visitor);
		// The tree's traversal order is source order for every construct today; the sort makes it the rule.
		std::ranges::stable_sort(loads, [](const ScriptAssetLoad& left, const ScriptAssetLoad& right)
		{
			return std::tie(left.Line, left.Column) < std::tie(right.Line, right.Column);
		});
		return loads;
	}

}
