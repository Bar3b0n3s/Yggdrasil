#include "TestsPCH.h"

#include "Engine/Graphics/ShaderLibrary.h"

#include "Engine/Core/Mounts/MemoryMount.h"
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

	// A minimal reflection document of a vertex shader whose entry point is `entry`.
	static std::string MakeVertexReflection(std::string_view entry)
	{
		return std::format(R"({{ "parameters": [], "entryPoints": [ {{ "name": "{}", "stage": "vertex", "bindings": [] }} ] }})", entry);
	}

	// Writes `text` to `path` (relative to the root of `mount`'s scheme "shaders"), creating its directory.
	static void WriteText(MemoryMount& mount, std::string_view path, std::string_view text)
	{
		Result<VfsPath> file = VfsPath::Create(ShaderScheme, path);
		REQUIRE(file.has_value());
		REQUIRE(mount.CreateDirectories(file->GetParent()).has_value());
		const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(text.data()), text.size());
		REQUIRE(mount.WriteFileAtomic(*file, bytes).has_value());
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("ShaderLibrary: variant stems match CompileShaders.py")
		{
			CHECK(MakeShaderVariantStem("Triangle", "VSMain", {}) == "Triangle/VSMain");
			const std::vector<ShaderDefine> one = { { .Key = "SMOKE_SATURATE", .Value = "1" } };
			CHECK(MakeShaderVariantStem("Smoke", "CSMain", one) == "Smoke/CSMain.SMOKE_SATURATE-1");
			// Keys are sorted, whatever order the permutation lists them in.
			const std::vector<ShaderDefine> two = { { .Key = "B_KEY", .Value = "x" }, { .Key = "A_KEY", .Value = "0" } };
			CHECK(MakeShaderVariantStem("Forward", "PSMain", two) == "Forward/PSMain.A_KEY-0.B_KEY-x");
		}

		TEST_CASE("ShaderLibrary: reflection loads without a device and is cached")
		{
			VirtualFileSystem vfs;
			ShaderLibrary library(nullptr, vfs, MountCompiledShaders(vfs));
			CHECK(library.GetRoot().ToString() == "shaders://");
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

		TEST_CASE("ShaderLibrary: a variant that was not compiled is NotFound naming the expected file")
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

		TEST_CASE("ShaderLibrary: reads under any VFS root and Clear reloads from it")
		{
			// The library sees only its root: here a directory of a memory mount instead of the compiled output.
			VirtualFileSystem vfs;
			Scope<MemoryMount> ownedMount = CreateScope<MemoryMount>();
			MemoryMount& mount = *ownedMount;
			REQUIRE(vfs.Mount(ShaderScheme, std::move(ownedMount)).has_value());
			WriteText(mount, "Pak/Shaders/Probe/VSMain.refl.json", MakeVertexReflection("VSMain"));
			Result<VfsPath> root = VfsPath::Create(ShaderScheme, "Pak/Shaders");
			REQUIRE(root.has_value());
			ShaderLibrary library(nullptr, vfs, *root);

			const Result<const ShaderReflection*> first = library.GetReflection("Probe", "VSMain");
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK((*first)->Stage == nvrhi::ShaderType::Vertex);

			// A changed file is not seen until Clear (shader hot reload clears the library, §8.12).
			WriteText(mount, "Pak/Shaders/Probe/VSMain.refl.json",
				R"({ "entryPoints": [ { "name": "VSMain", "stage": "fragment" } ] })");
			const Result<const ShaderReflection*> cached = library.GetReflection("Probe", "VSMain");
			REQUIRE(cached.has_value());
			CHECK((*cached)->Stage == nvrhi::ShaderType::Vertex);
			library.Clear();
			const Result<const ShaderReflection*> reloaded = library.GetReflection("Probe", "VSMain");
			REQUIRE_MESSAGE(reloaded.has_value(), reloaded.error().ToString());
			CHECK((*reloaded)->Stage == nvrhi::ShaderType::Pixel);
		}

		TEST_CASE("ShaderLibrary: a malformed reflection is an error naming the file")
		{
			VirtualFileSystem vfs;
			Scope<MemoryMount> ownedMount = CreateScope<MemoryMount>();
			MemoryMount& mount = *ownedMount;
			REQUIRE(vfs.Mount(ShaderScheme, std::move(ownedMount)).has_value());
			WriteText(mount, "Broken/VSMain.refl.json", "{ \"entryPoints\": ");
			WriteText(mount, "Shape/VSMain.refl.json", R"({ "entryPoints": [ { "name": "VSMain", "stage": "mesh" } ] })");
			Result<VfsPath> root = VfsPath::Create(ShaderScheme, "");
			REQUIRE(root.has_value());
			ShaderLibrary library(nullptr, vfs, *root);

			const Result<const ShaderReflection*> broken = library.GetReflection("Broken", "VSMain");
			REQUIRE_FALSE(broken.has_value());
			CHECK(broken.error().GetCode() == ErrorCode::Parse);
			CHECK(broken.error().ToString().contains("Broken/VSMain.refl.json"));

			const Result<const ShaderReflection*> shape = library.GetReflection("Shape", "VSMain");
			REQUIRE_FALSE(shape.has_value());
			CHECK(shape.error().GetCode() == ErrorCode::Validation);
			CHECK(shape.error().ToString().contains("Shape/VSMain.refl.json"));
			CHECK(shape.error().ToString().contains("mesh"));
		}

		TEST_CASE("ShaderLibrary: Get creates each shader once and Clear drops the cache" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			ShaderLibrary& library = gpu.GetShaders();
			const Result<nvrhi::ShaderHandle> first = library.Get("Triangle", "VSMain");
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK((*first)->getDesc().shaderType == nvrhi::ShaderType::Vertex);
			CHECK((*first)->getDesc().entryName == "VSMain");
			const Result<nvrhi::ShaderHandle> second = library.Get("Triangle", "VSMain");
			REQUIRE(second.has_value());
			CHECK(second->Get() == first->Get());
			library.Clear();
			const Result<nvrhi::ShaderHandle> reloaded = library.Get("Triangle", "VSMain");
			REQUIRE(reloaded.has_value());
			CHECK(reloaded->Get() != nullptr);

			// A missing variant is NotFound naming the .spv it expected.
			const Result<nvrhi::ShaderHandle> missing = library.Get("Triangle", "GSMain");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().ToString().contains("Triangle/GSMain.spv"));
		}
	}

}
