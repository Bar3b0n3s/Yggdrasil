#include "EnginePCH.h"
#include "Engine/Scripting/RequireResolver.h"

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Core/Assert.h"

#include <algorithm>
#include <tuple>
#include <vector>

namespace Engine {

	namespace {

		ErrorLocation ModuleLocation(const VfsPath& path)
		{
			ErrorLocation location{};
			location.File = path.GetPath();
			return location;
		}

		Status ValidateModulePath(const VfsPath& path)
		{
			if (path.GetScheme() != "project" || !path.GetPath().starts_with("Assets/") || path.GetExtension() != ".luau")
				return std::unexpected(Error(ErrorCode::Validation, "require origin must be a .luau file below project://Assets")
						.WithLocation(ModuleLocation(path)));
			return {};
		}

	}

	struct RequireResolver::State
	{
		std::vector<ScriptRequire> Requires{};
		std::vector<VfsPath> Active{};
	};

	RequireResolver::RequireResolver()
		: m_State(CreateScope<State>())
	{
	}

	RequireResolver::~RequireResolver() = default;

	Result<VfsPath> RequireResolver::Resolve(const VfsPath& importer, std::string_view request)
	{
		ENGINE_TRY(ValidateModulePath(importer));
		const auto invalid = [&importer, request](std::string_view reason)
		{
			return std::unexpected(Error(ErrorCode::Validation, std::format("invalid require '{}': {}", request, reason))
					.WithLocation(ModuleLocation(importer)));
		};
		if ((!request.starts_with("./") && !request.starts_with("../")) || request.find_first_of("\\:") != std::string_view::npos)
			return invalid("only relative forward-slash paths are accepted");
		std::vector<std::string> segments;
		const std::string parent(importer.GetParent().GetPath());
		for (size_t start = 0; start < parent.size();)
		{
			const size_t end = parent.find('/', start);
			segments.emplace_back(parent.substr(start, end == std::string::npos ? end : end - start));
			start = end == std::string::npos ? parent.size() : end + 1;
		}
		for (size_t start = 0; start <= request.size();)
		{
			const size_t end = request.find('/', start);
			const auto segment = request.substr(start, end == std::string_view::npos ? end : end - start);
			if (segment.empty())
				return invalid("empty path segment");
			if (segment == "..")
			{
				if (segments.size() <= 1)
					return invalid("path escapes Assets");
				segments.pop_back();
			}
			else if (segment != ".")
			{
				if (const auto valid = VfsPath::ValidateRelativePath(segment); !valid)
					return invalid(valid.error().GetMessageText());
				segments.emplace_back(segment);
			}
			if (end == std::string_view::npos)
				break;
			start = end + 1;
		}
		if (segments.size() <= 1 || request.ends_with("/.") || request.ends_with("/.."))
			return invalid("request must name a module");
		std::string path;
		for (const auto& segment : segments)
		{
			if (!path.empty())
				path += '/';
			path += segment;
		}
		ENGINE_TRY_ASSIGN(auto resolved, VfsPath::Create("project", path));
		if (resolved.GetExtension().empty())
		{
			path += ".luau";
			ENGINE_TRY_ASSIGN(auto withExtension, VfsPath::Create("project", path));
			resolved = std::move(withExtension);
		}
		if (const auto valid = ValidateModulePath(resolved); !valid)
			return invalid(valid.error().GetMessageText());
		ScriptRequire edge{ .From = std::string(importer.GetPath()), .Request = std::string(request), .Path = std::string(resolved.GetPath()) };
		const auto less = [](const ScriptRequire& left, const ScriptRequire& right)
		{
			return std::tie(left.From, left.Request, left.Path, left.Handle) < std::tie(right.From, right.Request, right.Path, right.Handle);
		};
		auto position = std::lower_bound(m_State->Requires.begin(), m_State->Requires.end(), edge, less);
		if (position == m_State->Requires.end() || *position != edge)
			m_State->Requires.insert(position, std::move(edge));
		return resolved;
	}

	Result<std::string> RequireResolver::ReadSource(IScriptModuleReader& reader, const VfsPath& path) const
	{
#if defined(ENGINE_DIST)
		static_cast<void>(reader);
		static_cast<void>(path);
		return MakeError(ErrorCode::Unsupported, "source modules are unavailable in Dist");
#else
		ENGINE_TRY(ValidateModulePath(path));
		auto source = reader.ReadModule(path);
		if (!source)
			return std::unexpected(std::move(source).error().WithLocation(ModuleLocation(path)).WithContext("while reading a required script module"));
		return source;
#endif
	}

	Status RequireResolver::EnterModule(const VfsPath& path)
	{
		ENGINE_TRY(ValidateModulePath(path));
		if (std::find(m_State->Active.begin(), m_State->Active.end(), path) != m_State->Active.end())
		{
			std::string chain;
			for (const auto& active : m_State->Active)
				chain += std::string(active.GetPath()) + " -> ";
			chain += path.GetPath();
			return std::unexpected(Error(ErrorCode::Script, "require cycle: " + chain).WithLocation(ModuleLocation(path)));
		}
		m_State->Active.push_back(path);
		return {};
	}

	void RequireResolver::LeaveModule(const VfsPath& path)
	{
		ENGINE_CORE_VERIFY(!m_State->Active.empty() && m_State->Active.back() == path, "unmatched module exit");
		m_State->Active.pop_back();
	}

	std::span<const ScriptRequire> RequireResolver::GetRequires() const
	{
		return m_State->Requires;
	}

	bool RequireResolver::HasActiveModules() const noexcept
	{
		return !m_State->Active.empty();
	}

}
