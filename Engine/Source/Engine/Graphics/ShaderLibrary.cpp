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
	}

	ShaderLibrary::~ShaderLibrary() = default;

	Result<nvrhi::ShaderHandle> ShaderLibrary::Get(std::string_view program, std::string_view entry,
		std::span<const ShaderDefine> permutation)
	{
		const std::string stem = MakeShaderVariantStem(program, entry, permutation);
		if (m_Device == nullptr)
			return MakeError(ErrorCode::InvalidState, "cannot create the shader '{}': this ShaderLibrary serves reflection only", stem);

		if (const auto cached = m_Shaders.find(stem); cached != m_Shaders.end())
			return cached->second;

		ENGINE_TRY_ASSIGN(const Buffer binary, Utils::ReadVariantFile(*m_Vfs, m_Root, stem, ".spv"));
		if (!Utils::IsSpirvBinary(binary))
		{
			Error error(ErrorCode::Validation, std::format("'{}.spv' under '{}' is not a SPIR-V binary ({} bytes)", stem, m_Root.ToString(), binary.size()));
			return std::unexpected(std::move(error).WithHint(std::string(Utils::CompileHint)));
		}
		ENGINE_TRY_ASSIGN(const ShaderReflection* reflection, GetReflection(program, entry, permutation));

		nvrhi::ShaderDesc desc;
		desc.shaderType = reflection->Stage;
		desc.entryName = reflection->EntryPoint;
		desc.debugName = stem;
		ENGINE_TRY_ASSIGN(nvrhi::ShaderHandle shader, m_Device->CreateShader(desc, binary));
		m_Shaders.emplace(stem, shader);
		return shader;
	}

	Result<const ShaderReflection*> ShaderLibrary::GetReflection(std::string_view program, std::string_view entry,
		std::span<const ShaderDefine> permutation)
	{
		std::string stem = MakeShaderVariantStem(program, entry, permutation);
		if (const auto cached = m_Reflections.find(stem); cached != m_Reflections.end())
			return &cached->second;

		ENGINE_TRY_ASSIGN(const Buffer contents, Utils::ReadVariantFile(*m_Vfs, m_Root, stem, ".refl.json"));
		const std::string_view text(reinterpret_cast<const char*>(contents.data()), contents.size());
		Result<ShaderReflection> reflection = ParseShaderReflection(text);
		if (!reflection)
		{
			std::string context = std::format("while reading the shader reflection '{}.refl.json' under '{}'", stem, m_Root.ToString());
			return std::unexpected(std::move(reflection).error().WithContext(std::move(context)));
		}

		return &m_Reflections.emplace(std::move(stem), std::move(*reflection)).first->second;
	}

	void ShaderLibrary::Clear()
	{
		m_Shaders.clear();
		m_Reflections.clear();
	}

}
