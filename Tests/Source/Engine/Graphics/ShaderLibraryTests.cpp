#include "TestsPCH.h"

#include "Engine/Graphics/ShaderLibrary.h"

#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	// Mounts this configuration's compiled shaders as shaders:// on `vfs` and returns the library root.
	static VfsPath MountCompiledShaders(VirtualFileSystem& vfs)
	{
		Result<Scope<NativeDirectoryMount>> mount =
			NativeDirectoryMount::Create(std::filesystem::path(ENGINE_SHADER_DIRECTORY), MountAccess::ReadOnly);
		REQUIRE_MESSAGE(mount.has_value(), mount.error().ToString());
		REQUIRE(vfs.Mount(ShaderScheme, std::move(*mount)).has_value());
		Result<VfsPath> root = VfsPath::Create(ShaderScheme, "");
		REQUIRE(root.has_value());
		return *root;
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("ShaderLibrary: variant stems match CompileShaders.py" * doctest::skip(true))
		{
			CHECK(MakeShaderVariantStem("Triangle", "VSMain", {}) == "Triangle/VSMain");
			const std::vector<ShaderDefine> one = { { .Key = "SMOKE_SATURATE", .Value = "1" } };
			CHECK(MakeShaderVariantStem("Smoke", "CSMain", one) == "Smoke/CSMain.SMOKE_SATURATE-1");
			// Keys are sorted, whatever order the permutation lists them in.
			const std::vector<ShaderDefine> two = { { .Key = "B_KEY", .Value = "x" }, { .Key = "A_KEY", .Value = "0" } };
			CHECK(MakeShaderVariantStem("Forward", "PSMain", two) == "Forward/PSMain.A_KEY-0.B_KEY-x");
		}

		TEST_CASE("ShaderLibrary: reflection loads without a device and is cached" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			ShaderLibrary library(nullptr, vfs, MountCompiledShaders(vfs));
			const Result<const ShaderReflection*> first = library.GetReflection("Triangle", "PSMain");
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			REQUIRE(*first != nullptr);
			CHECK((*first)->Stage == nvrhi::ShaderType::Pixel);
			const Result<const ShaderReflection*> second = library.GetReflection("Triangle", "PSMain");
			REQUIRE(second.has_value());
			CHECK(*second == *first);

			// Without a device there are no shaders.
			const Result<nvrhi::ShaderHandle> shader = library.Get("Triangle", "PSMain");
			REQUIRE_FALSE(shader.has_value());
			CHECK(shader.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("ShaderLibrary: a variant that was not compiled is NotFound naming the expected file" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			ShaderLibrary library(nullptr, vfs, MountCompiledShaders(vfs));
			const Result<const ShaderReflection*> unknownProgram = library.GetReflection("NoSuchProgram", "VSMain");
			REQUIRE_FALSE(unknownProgram.has_value());
			CHECK(unknownProgram.error().GetCode() == ErrorCode::NotFound);
			CHECK(unknownProgram.error().ToString().contains("NoSuchProgram/VSMain"));

			const std::vector<ShaderDefine> unknownValue = { { .Key = "SMOKE_SATURATE", .Value = "2" } };
			const Result<const ShaderReflection*> unknownPermutation = library.GetReflection("Smoke", "CSMain", unknownValue);
			REQUIRE_FALSE(unknownPermutation.has_value());
			CHECK(unknownPermutation.error().GetCode() == ErrorCode::NotFound);
			CHECK(unknownPermutation.error().ToString().contains("Smoke/CSMain.SMOKE_SATURATE-2"));
		}

		TEST_CASE("ShaderLibrary: Get creates each shader once and Clear drops the cache"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			ShaderLibrary& library = gpu.GetShaders();
			const Result<nvrhi::ShaderHandle> first = library.Get("Triangle", "VSMain");
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK((*first)->getDesc().shaderType == nvrhi::ShaderType::Vertex);
			const Result<nvrhi::ShaderHandle> second = library.Get("Triangle", "VSMain");
			REQUIRE(second.has_value());
			CHECK(second->Get() == first->Get());
			library.Clear();
			const Result<nvrhi::ShaderHandle> reloaded = library.Get("Triangle", "VSMain");
			REQUIRE(reloaded.has_value());
			CHECK(reloaded->Get() != nullptr);
		}
	}

}
