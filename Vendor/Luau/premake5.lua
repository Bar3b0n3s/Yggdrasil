-- Luau 0.741 (https://github.com/luau-lang/luau) - see VENDOR.md
-- Two static libraries, mirroring the upstream CMakeLists.txt / Sources.cmake / Makefile:
--   Luau         - runtime: Common, Ast, Bytecode, Compiler, Config, VM, Require
--                  (Bytecode is a public dependency of Compiler; Config is a public dependency of Require)
--   LuauAnalysis - static type checking / linting / autocomplete: Analysis (depends on Luau)
-- Not built: CodeGen (native JIT), Inliner (JIT-only), CLI, tests, fuzz, bench.
--
-- Error handling: LUA_USE_LONGJMP is left at its upstream default (0), so Luau errors are C++ exceptions
-- (lua_exception, derived from std::exception). C++ exceptions must stay enabled for these projects
-- and for every consumer. Consumers must not define LUA_USE_LONGJMP, LUA_API, LUACODE_API or
-- LUA_VECTOR_SIZE differently from this file (no overrides are defined here).

local LuauIncludeDirs =
{
	"Common/include",
	"Ast/include",
	"Bytecode/include",
	"Compiler/include",
	"Config/include",
	"VM/include",
	"Require/include"
}

project "Luau"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"
	exceptionhandling "On"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		-- Luau.Common
		"Common/include/Luau/*.h",
		"Common/src/BytecodeWire.cpp",
		"Common/src/StringUtils.cpp",
		"Common/src/TimeTrace.cpp",

		-- Luau.Ast
		"Ast/include/Luau/*.h",
		"Ast/src/Allocator.cpp",
		"Ast/src/Ast.cpp",
		"Ast/src/Confusables.cpp",
		"Ast/src/Cst.cpp",
		"Ast/src/Lexer.cpp",
		"Ast/src/Location.cpp",
		"Ast/src/Parser.cpp",
		"Ast/src/PrettyPrinter.cpp",

		-- Luau.Bytecode
		"Bytecode/include/Luau/*.h",
		"Bytecode/src/*.h",
		"Bytecode/src/BytecodeBuilder.cpp",
		"Bytecode/src/BytecodeDump.cpp",
		"Bytecode/src/BytecodeGraph.cpp",
		"Bytecode/src/Sccp.cpp",

		-- Luau.Compiler
		"Compiler/include/*.h",
		"Compiler/include/Luau/*.h",
		"Compiler/src/*.h",
		"Compiler/src/BuiltinFolding.cpp",
		"Compiler/src/Builtins.cpp",
		"Compiler/src/Compiler.cpp",
		"Compiler/src/ConstantFolding.cpp",
		"Compiler/src/CostModel.cpp",
		"Compiler/src/TableShape.cpp",
		"Compiler/src/Types.cpp",
		"Compiler/src/ValueTracking.cpp",
		"Compiler/src/lcode.cpp",

		-- Luau.Config
		"Config/include/Luau/*.h",
		"Config/src/Config.cpp",
		"Config/src/LinterConfig.cpp",
		"Config/src/LuauConfig.cpp",

		-- Luau.VM
		"VM/include/*.h",
		"VM/src/*.h",
		"VM/src/lapi.cpp",
		"VM/src/laux.cpp",
		"VM/src/lbaselib.cpp",
		"VM/src/lbitlib.cpp",
		"VM/src/lbuffer.cpp",
		"VM/src/lbuflib.cpp",
		"VM/src/lbuiltins.cpp",
		"VM/src/lclass.cpp",
		"VM/src/lclasslib.cpp",
		"VM/src/lcorolib.cpp",
		"VM/src/ldblib.cpp",
		"VM/src/ldebug.cpp",
		"VM/src/ldo.cpp",
		"VM/src/lfunc.cpp",
		"VM/src/lgc.cpp",
		"VM/src/lgcdebug.cpp",
		"VM/src/linit.cpp",
		"VM/src/lintlib.cpp",
		"VM/src/lmathlib.cpp",
		"VM/src/lmem.cpp",
		"VM/src/lnumprint.cpp",
		"VM/src/lobject.cpp",
		"VM/src/loslib.cpp",
		"VM/src/lperf.cpp",
		"VM/src/lstate.cpp",
		"VM/src/lstring.cpp",
		"VM/src/lstrlib.cpp",
		"VM/src/ltable.cpp",
		"VM/src/ltablib.cpp",
		"VM/src/ltm.cpp",
		"VM/src/ludata.cpp",
		"VM/src/lutf8lib.cpp",
		"VM/src/lveclib.cpp",
		"VM/src/lvector.cpp",
		"VM/src/lvmexecute.cpp",
		"VM/src/lvmload.cpp",
		"VM/src/lvmutils.cpp",

		-- Luau.Require
		"Require/include/Luau/*.h",
		"Require/src/*.h",
		"Require/src/AliasCycleTracker.cpp",
		"Require/src/Navigation.cpp",
		"Require/src/PathUtilities.cpp",
		"Require/src/Require.cpp",
		"Require/src/RequireImpl.cpp",
		"Require/src/RequireNavigator.cpp"
	}

	includedirs
	{
		LuauIncludeDirs
	}

	filter "system:windows"
		systemversion "latest"
		multiprocessorcompile "On"
		defines { "_CRT_SECURE_NO_WARNINGS" }

		-- Debugger visualizers (upstream adds them to the MSVC solution; consumers may pass /NATVIS, see VENDOR.md)
		files
		{
			"tools/natvis/Common.natvis",
			"tools/natvis/Ast.natvis",
			"tools/natvis/VM.natvis"
		}

	-- Upstream disables MSVC partial redundancy elimination for the interpreter loop (MSVC_VERSION >= 1924),
	-- since it substantially regresses interpreter codegen.
	filter { "toolset:msc*", "files:VM/src/lvmexecute.cpp" }
		buildoptions { "/d2ssa-pre-" }

	filter "system:linux"
		pic "On"
		buildoptions { "-fno-math-errno" }

	filter "system:macosx"
		buildoptions { "-fno-math-errno" }

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	-- Upstream optimized builds (CMake Release/RelWithDebInfo, Makefile config=release) define NDEBUG,
	-- which compiles out LUAU_ASSERT.
	filter "configurations:Release"
		runtime "Release"
		optimize "on"
		symbols "on"
		defines { "NDEBUG" }

	filter "configurations:Dist"
		runtime "Release"
		optimize "full"
		symbols "off"
		defines { "NDEBUG" }

	filter {}

project "LuauAnalysis"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"
	exceptionhandling "On"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		-- Luau.Analysis
		"Analysis/include/Luau/*.h",
		"Analysis/src/*.h",
		"Analysis/src/Anyification.cpp",
		"Analysis/src/ApplyTypeFunction.cpp",
		"Analysis/src/AstJsonEncoder.cpp",
		"Analysis/src/AstQuery.cpp",
		"Analysis/src/AstUtils.cpp",
		"Analysis/src/Autocomplete.cpp",
		"Analysis/src/AutocompleteCore.cpp",
		"Analysis/src/BuiltinDefinitions.cpp",
		"Analysis/src/BuiltinTypeFunctions.cpp",
		"Analysis/src/Clone.cpp",
		"Analysis/src/Constraint.cpp",
		"Analysis/src/ConstraintGenerator.cpp",
		"Analysis/src/ConstraintGraph.cpp",
		"Analysis/src/ConstraintSolver.cpp",
		"Analysis/src/ControlFlowGraph.cpp",
		"Analysis/src/DataFlowGraph.cpp",
		"Analysis/src/DcrLogger.cpp",
		"Analysis/src/Def.cpp",
		"Analysis/src/DumpCFG.cpp",
		"Analysis/src/EmbeddedBuiltinDefinitions.cpp",
		"Analysis/src/Error.cpp",
		"Analysis/src/ExpectedTypeVisitor.cpp",
		"Analysis/src/FileResolver.cpp",
		"Analysis/src/FragmentAutocomplete.cpp",
		"Analysis/src/Frontend.cpp",
		"Analysis/src/Generalization.cpp",
		"Analysis/src/GlobalTypes.cpp",
		"Analysis/src/Instantiation.cpp",
		"Analysis/src/Instantiation2.cpp",
		"Analysis/src/IostreamHelpers.cpp",
		"Analysis/src/IterativeTypeFunctionTypeVisitor.cpp",
		"Analysis/src/IterativeTypeVisitor.cpp",
		"Analysis/src/JsonEmitter.cpp",
		"Analysis/src/LValue.cpp",
		"Analysis/src/Linter.cpp",
		"Analysis/src/Module.cpp",
		"Analysis/src/NativeStackGuard.cpp",
		"Analysis/src/NonStrictTypeChecker.cpp",
		"Analysis/src/Normalize.cpp",
		"Analysis/src/OverloadResolver.cpp",
		"Analysis/src/Quantify.cpp",
		"Analysis/src/RecursionCounter.cpp",
		"Analysis/src/Refinement.cpp",
		"Analysis/src/RequireTracer.cpp",
		"Analysis/src/Scope.cpp",
		"Analysis/src/Simplify.cpp",
		"Analysis/src/StructuralTypeEquality.cpp",
		"Analysis/src/Substitution.cpp",
		"Analysis/src/Subtyping.cpp",
		"Analysis/src/Symbol.cpp",
		"Analysis/src/TableLiteralInference.cpp",
		"Analysis/src/ToDot.cpp",
		"Analysis/src/ToString.cpp",
		"Analysis/src/TopoSortStatements.cpp",
		"Analysis/src/TxnLog.cpp",
		"Analysis/src/Type.cpp",
		"Analysis/src/TypeArena.cpp",
		"Analysis/src/TypeAttach.cpp",
		"Analysis/src/TypeChecker2.cpp",
		"Analysis/src/TypeFunction.cpp",
		"Analysis/src/TypeFunctionError.cpp",
		"Analysis/src/TypeFunctionReductionGuesser.cpp",
		"Analysis/src/TypeFunctionRuntime.cpp",
		"Analysis/src/TypeFunctionRuntimeBuilder.cpp",
		"Analysis/src/TypeIds.cpp",
		"Analysis/src/TypeInfer.cpp",
		"Analysis/src/TypeOrPack.cpp",
		"Analysis/src/TypePack.cpp",
		"Analysis/src/TypePath.cpp",
		"Analysis/src/TypeStateMap.cpp",
		"Analysis/src/TypeUtils.cpp",
		"Analysis/src/TypedAllocator.cpp",
		"Analysis/src/Unifiable.cpp",
		"Analysis/src/Unifier.cpp",
		"Analysis/src/Unifier2.cpp",
		"Analysis/src/UserDefinedTypeFunction.cpp"
	}

	includedirs
	{
		LuauIncludeDirs,
		"Analysis/include"
	}

	links
	{
		"Luau"
	}

	filter "system:windows"
		systemversion "latest"
		multiprocessorcompile "On"
		defines { "_CRT_SECURE_NO_WARNINGS" }

		files
		{
			"tools/natvis/Analysis.natvis"
		}

	filter "system:linux"
		pic "On"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
		symbols "on"
		defines { "NDEBUG" }

	filter "configurations:Dist"
		runtime "Release"
		optimize "full"
		symbols "off"
		defines { "NDEBUG" }

	filter {}
