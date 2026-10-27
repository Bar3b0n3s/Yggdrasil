#include "EnginePCH.h"
#include "Engine/Graphics/ShaderLibrary.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"

namespace Engine {

	namespace Utils {

		// The first word of every SPIR-V module (SPIR-V specification §3.1), in the byte order of the file.
		static constexpr uint32_t SpirvMagicNumber = 0x07230203u;

		static constexpr std::string_view CompileHint =
			"build the Shaders project (Scripts/CompileShaders.py) and check the names against Resources/Shaders/Shaders.json";

		// Reads one output file of a variant. A missing file is NotFound naming the expected path: the variant was not
		// compiled.
		static Result<Buffer> ReadVariantFile(const VirtualFileSystem& vfs, const VfsPath& root, std::string_view stem,
			std::string_view extension)
		{
			Result<VfsPath> path = root.Join(std::format("{}{}", stem, extension));
			if (!path)
			{
				std::string context = std::format("while locating the shader variant '{}' under '{}'", stem, root.ToString());
				return std::unexpected(std::move(path).error().WithContext(std::move(context)));
			}

			Result<Buffer> contents = vfs.ReadFile(*path);
			if (!contents && contents.error().GetCode() == ErrorCode::NotFound)
			{
				Error error(ErrorCode::NotFound, std::format("the shader variant '{}' was not compiled: '{}' does not exist", stem, path->ToString()));
				return std::unexpected(std::move(error).WithHint(std::string(CompileHint)));
			}
			if (!contents)
				return std::unexpected(std::move(contents).error().WithContext(std::format("while reading '{}'", path->ToString())));
			return contents;
		}

		// A SPIR-V binary is a non-empty sequence of 32-bit words starting with the magic number.
		static bool IsSpirvBinary(std::span<const std::byte> binary)
		{
			if (binary.empty() || binary.size() % sizeof(uint32_t) != 0)
				return false;
			uint32_t magic = 0;
			std::memcpy(&magic, binary.data(), sizeof(magic));
			return magic == SpirvMagicNumber;
		}

	}

	std::string MakeShaderVariantStem(std::string_view program, std::string_view entry, std::span<const ShaderDefine> permutation)
	{
		std::vector<const ShaderDefine*> defines;
		defines.reserve(permutation.size());
		for (const ShaderDefine& define : permutation)
			defines.push_back(&define);
		std::ranges::stable_sort(defines, std::less<>(), [](const ShaderDefine* define) -> const std::string&
		{
			return define->Key;
		});

		std::string stem = std::format("{}/{}", program, entry);
		for (const ShaderDefine* define : defines)
			stem += std::format(".{}-{}", define->Key, define->Value);
		return stem;
	}

	ShaderLibrary::ShaderLibrary(GraphicsDevice* device, const VirtualFileSystem& vfs, VfsPath root)
		: m_Root(std::move(root)), m_Device(device), m_Vfs(&vfs)
	{
		ENGINE_CORE_ASSERT(!m_Root.IsEmpty(), "ShaderLibrary needs a root directory");
		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 33): a lost device's shaders are dropped and their SPIR-V
		// kept; the next Get creates them again from it on the recovered device. The shaders a reload in progress read are
		// dropped too.
		if (m_Device != nullptr)
		{
			ShaderLibrary* owner = this;
			GpuRecreationHooks hooks;
			hooks.Name = "ShaderLibrary";
			hooks.Release = [owner]()
			{
				for (VariantMap* variants : { &owner->m_Shaders, &owner->m_ReloadedShaders })
				{
					for (auto& [stem, variant] : *variants)
						variant.Shader = nullptr;
				}
			};
			m_Recreation = m_Device->GetRecreation().Register(std::move(hooks));
		}
	}

	ShaderLibrary::~ShaderLibrary() = default;

	Result<nvrhi::ShaderHandle> ShaderLibrary::Get(std::string_view program, std::string_view entry,
		std::span<const ShaderDefine> permutation)
	{
		const std::string stem = MakeShaderVariantStem(program, entry, permutation);
		if (m_Device == nullptr)
			return MakeError(ErrorCode::InvalidState, "cannot create the shader '{}': this ShaderLibrary serves reflection only", stem);

		// A reload reads every variant again and keeps what it read apart until EndReload.
		VariantMap& variants = m_Reloading ? m_ReloadedShaders : m_Shaders;
		const auto cached = variants.find(stem);
		if (cached != variants.end() && cached->second.Shader != nullptr)
			return cached->second.Shader;

		// A variant whose shader a device loss dropped is created again from the bytes it was created from, which match its
		// cached reflection; any other variant is read from its file.
		Buffer read;
		if (cached == variants.end())
		{
			ENGINE_TRY_ASSIGN(read, Utils::ReadVariantFile(*m_Vfs, m_Root, stem, ".spv"));
			if (!Utils::IsSpirvBinary(read))
			{
				Error error(ErrorCode::Validation, std::format("'{}.spv' under '{}' is not a SPIR-V binary ({} bytes)", stem, m_Root.ToString(), read.size()));
				return std::unexpected(std::move(error).WithHint(std::string(Utils::CompileHint)));
			}
		}
		const std::span<const std::byte> binary = cached != variants.end() ? std::span<const std::byte>(cached->second.Binary) : std::span<const std::byte>(read);
		ENGINE_TRY_ASSIGN(const ShaderReflection* reflection, GetReflection(program, entry, permutation));

		nvrhi::ShaderDesc desc;
		desc.shaderType = reflection->Stage;
		desc.entryName = reflection->EntryPoint;
		desc.debugName = stem;
		ENGINE_TRY_ASSIGN(nvrhi::ShaderHandle shader, m_Device->CreateShader(desc, binary));
		if (cached != variants.end())
			cached->second.Shader = shader;
		else
			variants.emplace(stem, Detail::ShaderLibraryVariant{ .Binary = std::move(read), .Shader = shader });
		return shader;
	}

	Result<const ShaderReflection*> ShaderLibrary::GetReflection(std::string_view program, std::string_view entry,
		std::span<const ShaderDefine> permutation)
	{
		std::string stem = MakeShaderVariantStem(program, entry, permutation);
		ReflectionMap& reflections = m_Reloading ? m_ReloadedReflections : m_Reflections;
		if (const auto cached = reflections.find(stem); cached != reflections.end())
			return &cached->second;

		ENGINE_TRY_ASSIGN(const Buffer contents, Utils::ReadVariantFile(*m_Vfs, m_Root, stem, ".refl.json"));
		const std::string_view text(reinterpret_cast<const char*>(contents.data()), contents.size());
		Result<ShaderReflection> reflection = ParseShaderReflection(text);
		if (!reflection)
		{
			std::string context = std::format("while reading the shader reflection '{}.refl.json' under '{}'", stem, m_Root.ToString());
			return std::unexpected(std::move(reflection).error().WithContext(std::move(context)));
		}

		return &reflections.emplace(std::move(stem), std::move(*reflection)).first->second;
	}

	void ShaderLibrary::BeginReload()
	{
		ENGINE_CORE_ASSERT(!m_Reloading, "ShaderLibrary::BeginReload during a reload");
		ENGINE_CORE_ASSERT(m_ReloadedShaders.empty() && m_ReloadedReflections.empty(), "A reload's variants outlived its EndReload");
		m_Reloading = true;
	}

	void ShaderLibrary::EndReload(bool keep)
	{
		ENGINE_CORE_ASSERT(m_Reloading, "ShaderLibrary::EndReload without BeginReload");
		m_Reloading = false;
		if (!keep)
		{
			while (!m_ReloadedReflections.empty())
				m_RetiredReflections.push_back(m_ReloadedReflections.extract(m_ReloadedReflections.begin()));
			m_ReloadedShaders.clear();
			return;
		}

		while (!m_ReloadedReflections.empty())
		{
			// The node moves with its reflection at the same address, so GetReflection returns from now on the pointer the
			// reload handed out; the earlier reflection is retired, never freed.
			ReflectionMap::node_type reloaded = m_ReloadedReflections.extract(m_ReloadedReflections.begin());
			if (const auto earlier = m_Reflections.find(reloaded.key()); earlier != m_Reflections.end())
				m_RetiredReflections.push_back(m_Reflections.extract(earlier));
			// A variant whose reflection alone the reload read drops its earlier shader as well, which was created for the
			// replaced reflection: its next Get reads the SPIR-V again.
			if (!m_ReloadedShaders.contains(reloaded.key()))
				m_Shaders.erase(reloaded.key());
			m_Reflections.insert(std::move(reloaded));
		}
		for (auto& [stem, variant] : m_ReloadedShaders)
			m_Shaders.insert_or_assign(stem, std::move(variant));
		m_ReloadedShaders.clear();
	}

}
