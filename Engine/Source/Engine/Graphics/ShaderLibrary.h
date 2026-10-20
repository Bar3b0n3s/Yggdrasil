#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Graphics/GpuRecreation.h"
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

	namespace Detail {

		// One variant a ShaderLibrary created a shader for (at namespace scope: a nested struct with default member
		// initializers is not default-constructible inside its enclosing class on Clang with libstdc++): its SPIR-V bytes,
		// CPU data kept across a device loss, and its shader, null after a loss until the next Get creates it again from
		// Binary.
		struct ShaderLibraryVariant
		{
			Buffer Binary{};
			nvrhi::ShaderHandle Shader{};
		};

	}

	// Loads and caches shader variants. Not copyable or movable; main thread only (§4.11; the VFS reads themselves are
	// thread-safe).
	//
	// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 26 and 33): a library with a device registers with the
	// device's recreation registry (as "ShaderLibrary"): when the device is lost it drops every shader it created (every
	// nvrhi::ShaderHandle it holds, a reload's included), and the next Get creates the shader again on the recovered device
	// from the SPIR-V bytes the library read before the loss, never from the file again: like the reflections, each cached
	// variant's SPIR-V is CPU data kept with it, so a recovered shader always matches its reflection, whatever a rebuild
	// wrote to the shader output meanwhile (a new build reaches the library only through a reload). The reflections stay,
	// so every pointer GetReflection returned stays valid.
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

		// The parsed .refl.json of a variant, cached. The pointer is never null and stays valid until the library's
		// destruction. Errors: NotFound naming the expected path; Io; those of ParseShaderReflection, with the
		// file as context.
		[[nodiscard]] Result<const ShaderReflection*> GetReflection(std::string_view program, std::string_view entry,
			std::span<const ShaderDefine> permutation = {});

		// M18 (R-SHR, the editor's shader hot reload; Docs/Decisions/0029-m18-contract.md decision 3): the two halves of a
		// reload, around SceneRendererPipelines::CreatePipelines. After BeginReload, the first Get or GetReflection of each
		// variant reads its compiled files again (the .spv and .refl.json a CompileShaders.py run or a build of the Shaders
		// project rewrote), while the entries cached before stay alive. EndReload(true) keeps what the reload read (the
		// shader, its SPIR-V bytes and its reflection), replacing the earlier entries of the same variants, so a later Get of
		// a variant the reload read sees the new shader: a debug view's first pipelines specialize the forward variants that
		// CreatePipelines read. A variant the reload did not read keeps its earlier entry: ImGui's per-format pipelines and
		// the Runtime's blit are not part of the reload and keep the shaders the library cached before it, also when they
		// are created later (a change to them needs a restart). EndReload(false) drops what the reload read and restores the
		// earlier entries, so a failed reload leaves the library as it was. Either way every pointer GetReflection returned
		// stays valid until the library's destruction (replaced reflections are kept, not freed). Asserted: BeginReload not
		// during a reload, EndReload only during one.
		void BeginReload();
		void EndReload(bool keep);

		[[nodiscard]] const VfsPath& GetRoot() const { return m_Root; }
	private:
		VfsPath m_Root;
		GraphicsDevice* m_Device = nullptr;       // documented back-reference; null for a reflection-only library
		const VirtualFileSystem* m_Vfs = nullptr; // documented back-reference
		// By variant stem. std::map keeps every reflection at a stable address (GetReflection's pointers).
		std::map<std::string, Detail::ShaderLibraryVariant, std::less<>> m_Shaders;
		std::map<std::string, ShaderReflection, std::less<>> m_Reflections;
		// M18 (C18.3): the library's registration with its device (a library with a device only); its Release drops every
		// shader handle the library holds and keeps their SPIR-V bytes.
		GpuRecreationRegistration m_Recreation;
	};

}
