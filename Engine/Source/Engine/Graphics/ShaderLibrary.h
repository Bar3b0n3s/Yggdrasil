#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Graphics/ShaderReflection.h"

#include <nvrhi/nvrhi.h>

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

// The compiled SPIR-V of every program in Resources/Shaders/Shaders.json (Architecture §8.12). Scripts/CompileShaders.py
// writes <Program>/<Entry>[.<KEY>-<VALUE>...].spv and .refl.json per entry point and permutation; development builds
// mount that output directory, bin/<OutputDir>/Shaders (ENGINE_SHADER_DIRECTORY), as shaders://, and exported games read
// the target configuration's SPIR-V from Engine.pak (M7) under whatever root the runtime mounts. The library only ever
// sees a VFS root, so neither case is special here.

namespace Engine {

	class GraphicsDevice;
	class VirtualFileSystem;

	// The scheme development builds mount the shader output directory at (EngineContext; HeadlessGpuFixture), and the
	// default library root "shaders://" (Docs/Decisions/0009-m5-decisions.md: the VFS mounts whole schemes, so §8.12's
	// engine://Shaders cannot point at bin/ while engine:// points at Resources/).
	inline constexpr std::string_view ShaderScheme = "shaders";

	// One permutation value: a key of the program's "Permutations" and one of its values (both as in Shaders.json).
	struct ShaderDefine
	{
		std::string Key{};
		std::string Value{};
	};

	// The file stem CompileShaders.py gives a variant: "<Program>/<Entry>" followed by ".<KEY>-<VALUE>" per define in
	// ascending key order, whatever order `permutation` lists them in ("Forward/PSMain.ALPHA_MASK-1"). Pure; the caller
	// passes names that Shaders.json accepts (PascalCase identifiers, upper-case keys).
	[[nodiscard]] std::string MakeShaderVariantStem(std::string_view program, std::string_view entry, std::span<const ShaderDefine> permutation);

	// Loads and caches shader variants. Not copyable or movable; main thread only (§4.11; the VFS reads themselves are
	// thread-safe).
	class ShaderLibrary
	{
	public:
		// `device` may be null: such a library serves reflection only, which is how the CPU-only tests use it. `vfs` and
		// `device` are documented back-references that must outlive the library. `root` is the directory holding the
		// program folders ("shaders://").
		ShaderLibrary(GraphicsDevice* device, const VirtualFileSystem& vfs, VfsPath root);
		~ShaderLibrary();

		ShaderLibrary(const ShaderLibrary&) = delete;
		ShaderLibrary& operator=(const ShaderLibrary&) = delete;

		// The shader of a variant, created on first use through GraphicsDevice::CreateShader with the stage and entry name of
		// its reflection, then cached (§8.12 ShaderLibrary::Get). Errors: InvalidState for a library without a device;
		// NotFound naming the expected .spv path when the variant was not compiled (an unknown program, entry or
		// permutation value, or a build that did not run CompileShaders.py); those of GetReflection; Gpu when creation
		// fails.
		[[nodiscard]] Result<nvrhi::ShaderHandle> Get(std::string_view program, std::string_view entry,
			std::span<const ShaderDefine> permutation = {});

		// The parsed .refl.json of a variant, cached. The pointer is never null and stays valid until Clear or the
		// library's destruction. Errors: NotFound naming the expected path; Io; those of ParseShaderReflection, with the
		// file as context.
		[[nodiscard]] Result<const ShaderReflection*> GetReflection(std::string_view program, std::string_view entry,
			std::span<const ShaderDefine> permutation = {});

		// Drops every cached shader and reflection, so the next Get reloads from the VFS (shader hot reload, §8.12; the
		// pipelines that used the old shaders keep them alive until they are rebuilt).
		void Clear();

		[[nodiscard]] const VfsPath& GetRoot() const { return m_Root; }
	private:
		VfsPath m_Root;
		GraphicsDevice* m_Device = nullptr;       // documented back-reference; null for a reflection-only library
		const VirtualFileSystem* m_Vfs = nullptr; // documented back-reference
		// By variant stem. std::map keeps every reflection at a stable address until Clear (GetReflection's pointers).
		std::map<std::string, nvrhi::ShaderHandle, std::less<>> m_Shaders;
		std::map<std::string, ShaderReflection, std::less<>> m_Reflections;
	};

}
