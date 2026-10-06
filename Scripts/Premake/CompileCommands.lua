-- premake5 compile-commands: writes compile_commands.json, the compilation database used by clang-tidy, clangd and
-- Scripts/Lint.py (Docs/Architecture.md §2.2).
--
--   premake5 compile-commands [--cc-config=NAME] [--cc-platform=NAME] [--cc-output=FILE] [--cc=clang|gcc]
--                             [--os=windows|linux|macosx] [--to=DIR]
--
-- The root premake5.lua includes this file, so the action is available like any generator. It can also be loaded
-- into an unmodified workspace through premake's system-script hook, which runs before the workspace script:
--
--   premake5 --systemscript=Scripts/Premake/CompileCommands.lua compile-commands
--
-- Contents: one entry per compiled C or C++ file of every project, for one build configuration (--cc-config, default
-- the workspace's first configuration, Debug). The --cc-config, --cc-platform and --cc-output options are premake's
-- own compilation-database options; this action gives them the same meaning.
--
-- Flags: the compiler flags are produced by premake's toolset adapter (clang unless --cc says otherwise) from the
-- same project settings the IDE and make projects use, in the order the gmake generator uses: preprocessor flags,
-- defines, include directories, compiler flags, buildoptions, then per-file settings and forced includes. Filters on
-- "toolset:gcc or clang" therefore apply and "toolset:msc*" ones do not. Dependency-file flags (-MD, -MP) are left
-- out because no build runs from this file, and so are precompiled-header forced includes: every first-party source
-- includes its precompiled header explicitly as its first line, so it parses the same without one.
--
-- Paths are absolute, so entries stay valid wherever the file is moved. Implicit system include directories are not
-- embedded either: the consumers are clang tools, which locate the standard library themselves, and embedding them
-- would pin machine-specific paths. Utility and Makefile projects compile nothing and contribute no entries.
--
-- Output: <workspace location>/compile_commands.json (the repository root, or the --to directory), or --cc-output.
-- The file is rewritten only when its content changes. It is gitignored.

local p = premake

-- The root premake5.lua includes this file, and a --systemscript run loads it before that: register once.
if p.action.get("compile-commands") ~= nil then
	return
end

local CompileCommands = {}

-- Project kinds that compile sources.
CompileCommands.CompiledKinds = { ConsoleApp = true, WindowedApp = true, StaticLib = true, SharedLib = true }

-- Source extensions and the toolset tool that compiles them.
CompileCommands.SourceTools = { [".c"] = "cc", [".cpp"] = "cxx", [".cc"] = "cxx", [".cxx"] = "cxx", [".c++"] = "cxx" }

local JsonEscapes =
{
	["\""] = "\\\"",
	["\\"] = "\\\\",
	["\b"] = "\\b",
	["\f"] = "\\f",
	["\n"] = "\\n",
	["\r"] = "\\r",
	["\t"] = "\\t"
}

-- A JSON string literal: quotes, backslashes and control characters escaped.
function CompileCommands.JsonString(value)
	local escaped = value:gsub("[%c\"\\]", function(character)
		return JsonEscapes[character] or string.format("\\u%04x", character:byte())
	end)
	return "\"" .. escaped .. "\""
end

function CompileCommands.AbsolutePath(value)
	return path.normalize(path.getabsolute(value))
end

-- The tool ("cc" or "cxx") that compiles a file in this configuration, or nil when the file is not compiled.
function CompileCommands.GetTool(file, fcfg)
	if fcfg == nil or fcfg.buildaction == "None" or fcfg.excludefrombuild then
		return nil
	end

	if p.fileconfig.hasCustomBuildRule(fcfg) then
		return nil
	end

	if fcfg.compileas ~= nil and fcfg.compileas ~= "Default" then
		if p.languages.isc(fcfg.compileas) then
			return "cc"
		elseif p.languages.iscpp(fcfg.compileas) then
			return "cxx"
		end
		return nil
	end

	return CompileCommands.SourceTools[path.getextension(file):lower()]
end

function CompileCommands.AppendAll(arguments, values)
	for _, value in ipairs(values or {}) do
		table.insert(arguments, value)
	end
end

-- Toolset flags and buildoptions as separate arguments. premake returns some multi-word flags as one string (the clang
-- toolset's "-arch arm64" on macOS), which the gmake generator joins into a shell command line where the shell splits
-- it; an "arguments" array must split it the same way. Values containing quotes are kept whole.
function CompileCommands.AppendFlags(arguments, flags)
	for _, flag in ipairs(flags or {}) do
		if flag:find("%s") and not flag:find("[\"']") then
			for word in flag:gmatch("%S+") do
				table.insert(arguments, word)
			end
		else
			table.insert(arguments, flag)
		end
	end
end

function CompileCommands.AppendDirectories(arguments, flag, directories)
	for _, directory in ipairs(directories or {}) do
		table.insert(arguments, flag)
		table.insert(arguments, CompileCommands.AbsolutePath(directory))
	end
end

-- Defines, undefines and include directories of a configuration or of a file's own settings.
function CompileCommands.AppendPreprocessor(arguments, toolset, cfg, settings)
	CompileCommands.AppendAll(arguments, toolset.getdefines(settings.defines or {}, cfg))
	CompileCommands.AppendAll(arguments, toolset.getundefines(settings.undefines or {}))
	CompileCommands.AppendDirectories(arguments, "-I", settings.includedirs)
	CompileCommands.AppendDirectories(arguments, "-isystem", settings.externalincludedirs)
	CompileCommands.AppendDirectories(arguments, "-F", settings.frameworkdirs)
	CompileCommands.AppendDirectories(arguments, "-idirafter", settings.includedirsafter)
end

function CompileCommands.GetArguments(cfg, toolset, file, fcfg, tool, object)
	local arguments = { toolset.gettoolname(cfg, tool) }

	local cppflags = {}
	for _, flag in ipairs(toolset.getcppflags(cfg)) do
		if not flag:startswith("-M") then
			table.insert(cppflags, flag)
		end
	end
	CompileCommands.AppendFlags(arguments, cppflags)

	CompileCommands.AppendPreprocessor(arguments, toolset, cfg, cfg)

	local getflags = iif(tool == "cc", toolset.getcflags, toolset.getcxxflags)
	CompileCommands.AppendFlags(arguments, getflags(cfg))
	CompileCommands.AppendFlags(arguments, cfg.buildoptions)

	-- Per-file settings (premake filters on "files:..."), appended after the configuration's like the gmake generator.
	if p.fileconfig.hasFileSettings(fcfg) then
		CompileCommands.AppendFlags(arguments, getflags(fcfg))
		CompileCommands.AppendFlags(arguments, fcfg.buildoptions)
		CompileCommands.AppendPreprocessor(arguments, toolset, cfg, fcfg)
	end

	for _, header in ipairs(table.join(cfg.forceincludes or {}, fcfg.forceincludes or {})) do
		table.insert(arguments, "-include")
		table.insert(arguments, CompileCommands.AbsolutePath(header))
	end

	CompileCommands.AppendAll(arguments, { "-c", file, "-o", object })
	return arguments
end

function CompileCommands.GetConfiguration(wks, prj)
	local buildcfg = _OPTIONS["cc-config"] or wks.configurations[1]
	local platform = _OPTIONS["cc-platform"] or wks.defaultplatform

	if not table.contains(wks.configurations, buildcfg) then
		p.error("compile-commands: workspace '%s' has no configuration '%s' (--cc-config)", wks.name, buildcfg)
	end

	-- nil when the project removed the configuration (EditorCore, Editor and Tests have no Dist).
	return p.project.getconfig(prj, buildcfg, platform)
end

function CompileCommands.CollectProject(wks, prj, entries)
	local cfg = CompileCommands.GetConfiguration(wks, prj)
	if cfg == nil or not CompileCommands.CompiledKinds[cfg.kind] then
		return
	end

	local toolset = p.tools.canonical(cfg.toolset)
	if toolset == nil then
		p.error("compile-commands: project '%s' uses toolset '%s', which premake does not know", prj.name, tostring(cfg.toolset))
	end

	p.oven.assignObjectSequences(prj)

	local directory = CompileCommands.AbsolutePath(prj.location)
	local tree = p.project.getsourcetree(prj)
	p.tree.traverse(tree, {
		onleaf = function(node)
			local fcfg = p.fileconfig.getconfig(node, cfg)
			local tool = CompileCommands.GetTool(node.abspath, fcfg)
			if tool == nil then
				return
			end

			local file = CompileCommands.AbsolutePath(node.abspath)
			local object = CompileCommands.AbsolutePath(path.join(cfg.objdir, (fcfg.objname or path.getbasename(file)) .. ".o"))
			table.insert(entries, {
				directory = directory,
				file = file,
				arguments = CompileCommands.GetArguments(cfg, toolset, file, fcfg, tool, object),
				output = object,
			})
		end,
	})
end

function CompileCommands.Encode(entries)
	local lines = { "[" }
	for index, entry in ipairs(entries) do
		local arguments = {}
		for _, argument in ipairs(entry.arguments) do
			table.insert(arguments, "\t\t\t" .. CompileCommands.JsonString(argument))
		end

		table.insert(lines, "\t{")
		table.insert(lines, "\t\t\"directory\": " .. CompileCommands.JsonString(entry.directory) .. ",")
		table.insert(lines, "\t\t\"file\": " .. CompileCommands.JsonString(entry.file) .. ",")
		table.insert(lines, "\t\t\"arguments\": [")
		table.insert(lines, table.concat(arguments, ",\n"))
		table.insert(lines, "\t\t],")
		table.insert(lines, "\t\t\"output\": " .. CompileCommands.JsonString(entry.output))
		table.insert(lines, iif(index < #entries, "\t},", "\t}"))
	end
	table.insert(lines, "]")
	return table.concat(lines, "\n") .. "\n"
end

function CompileCommands.Execute()
	-- Output file -> entries, so that several workspaces at one location share one database.
	local outputs = {}
	local order = {}

	for wks in p.global.eachWorkspace() do
		local output = CompileCommands.AbsolutePath(_OPTIONS["cc-output"] or path.join(wks.location, "compile_commands.json"))
		if outputs[output] == nil then
			outputs[output] = {}
			table.insert(order, output)
		end

		for prj in p.workspace.eachproject(wks) do
			CompileCommands.CollectProject(wks, prj, outputs[output])
		end
	end

	for _, output in ipairs(order) do
		local entries = outputs[output]
		table.sort(entries, function(a, b)
			if a.file ~= b.file then
				return a.file < b.file
			end
			return a.output < b.output
		end)

		local ok, message = os.mkdir(path.getdirectory(output))
		if not ok then
			p.error("compile-commands: cannot create the directory of '%s': %s", output, tostring(message))
		end

		local result, writeError = os.writefile_ifnotequal(CompileCommands.Encode(entries), output)
		if writeError then
			p.error("compile-commands: cannot write '%s': %s", output, writeError)
		elseif result ~= 0 then
			printf("Generated %s (%d entries)...", output, #entries)
		else
			printf("%s is up to date (%d entries)", output, #entries)
		end
	end
end

newaction
{
	trigger = "compile-commands",
	shortname = "compile_commands.json",
	description = "Generate compile_commands.json (clang-style flags) for clang-tidy, clangd and Scripts/Lint.py",

	-- Utility projects (Shaders) are valid in the workspace; they simply compile nothing.
	valid_kinds = { "ConsoleApp", "WindowedApp", "StaticLib", "SharedLib", "Utility", "Makefile", "None" },
	valid_languages = { "C", "C++" },
	valid_tools = { cc = { "clang", "gcc" } },
	toolset = "clang",

	execute = CompileCommands.Execute,
}
