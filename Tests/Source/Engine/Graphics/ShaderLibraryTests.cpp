#include "TestsPCH.h"

#include "Engine/Graphics/ShaderLibrary.h"

#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/HeadlessGpuFixture.h"

#include <map>
#include <span>
#include <string>

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

	// Writes `bytes` to `path` (relative to the root of `mount`'s scheme "shaders"), creating its directory.
	static void WriteBytes(MemoryMount& mount, std::string_view path, std::span<const std::byte> bytes)
	{
		Result<VfsPath> file = VfsPath::Create(ShaderScheme, path);
		REQUIRE(file.has_value());
		REQUIRE(mount.CreateDirectories(file->GetParent()).has_value());
		REQUIRE(mount.WriteFileAtomic(*file, bytes).has_value());
	}

	static void WriteText(MemoryMount& mount, std::string_view path, std::string_view text)
	{
		WriteBytes(mount, path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
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

		TEST_CASE("ShaderLibrary: reads under any VFS root and keeps what it read")
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

			// A file changed after the first read is not seen: the library keeps the reflection it parsed, at the same address
			// (a rebuilt shader reaches the library only through a reload, BeginReload and EndReload).
			WriteText(mount, "Pak/Shaders/Probe/VSMain.refl.json",
				R"({ "entryPoints": [ { "name": "VSMain", "stage": "fragment" } ] })");
			const Result<const ShaderReflection*> cached = library.GetReflection("Probe", "VSMain");
			REQUIRE(cached.has_value());
			CHECK(*cached == *first);
			CHECK((*cached)->Stage == nvrhi::ShaderType::Vertex);
		}

		TEST_CASE("ShaderLibrary: a reload reads variants again, and a dropped reload restores them")
		{
			// M18 (C18.1, R-SHR; Docs/Decisions/0029-m18-contract.md decision 3): BeginReload and EndReload around a shader
			// reload; every reflection pointer handed out stays valid.
			VirtualFileSystem vfs;
			Scope<MemoryMount> ownedMount = CreateScope<MemoryMount>();
			MemoryMount& mount = *ownedMount;
			REQUIRE(vfs.Mount(ShaderScheme, std::move(ownedMount)).has_value());
			WriteText(mount, "Probe/VSMain.refl.json", MakeVertexReflection("VSMain"));
			Result<VfsPath> root = VfsPath::Create(ShaderScheme, "");
			REQUIRE(root.has_value());
			ShaderLibrary library(nullptr, vfs, *root);
			const Result<const ShaderReflection*> original = library.GetReflection("Probe", "VSMain");
			REQUIRE_MESSAGE(original.has_value(), original.error().ToString());
			CHECK((*original)->Stage == nvrhi::ShaderType::Vertex);

			// The build rewrote the variant: a reload reads it again, and dropping the reload restores the earlier one.
			WriteText(mount, "Probe/VSMain.refl.json", R"({ "entryPoints": [ { "name": "VSMain", "stage": "fragment" } ] })");
			library.BeginReload();
			const Result<const ShaderReflection*> reread = library.GetReflection("Probe", "VSMain");
			REQUIRE_MESSAGE(reread.has_value(), reread.error().ToString());
			CHECK((*reread)->Stage == nvrhi::ShaderType::Pixel);
			library.EndReload(false);
			const Result<const ShaderReflection*> restored = library.GetReflection("Probe", "VSMain");
			REQUIRE(restored.has_value());
			CHECK(*restored == *original);
			CHECK((*original)->Stage == nvrhi::ShaderType::Vertex);

			// A kept reload replaces the variant; the pointers handed out before stay valid.
			library.BeginReload();
			const Result<const ShaderReflection*> kept = library.GetReflection("Probe", "VSMain");
			REQUIRE(kept.has_value());
			library.EndReload(true);
			const Result<const ShaderReflection*> current = library.GetReflection("Probe", "VSMain");
			REQUIRE(current.has_value());
			CHECK(*current == *kept);
			CHECK((*current)->Stage == nvrhi::ShaderType::Pixel);
			CHECK((*original)->Stage == nvrhi::ShaderType::Vertex);
		}

		TEST_CASE("ShaderLibrary: a kept reload leaves the variants it did not read as they were")
		{
			// M18 (R-SHR; Docs/Decisions/0029-m18-contract.md decision 3): only what the reload read is replaced, so pipelines
			// outside the reload (ImGui's, the Runtime's blit) keep the variants the library cached before it.
			VirtualFileSystem vfs;
			Scope<MemoryMount> ownedMount = CreateScope<MemoryMount>();
			MemoryMount& mount = *ownedMount;
			REQUIRE(vfs.Mount(ShaderScheme, std::move(ownedMount)).has_value());
			WriteText(mount, "Read/VSMain.refl.json", MakeVertexReflection("VSMain"));
			WriteText(mount, "Unread/VSMain.refl.json", MakeVertexReflection("VSMain"));
			Result<VfsPath> root = VfsPath::Create(ShaderScheme, "");
			REQUIRE(root.has_value());
			ShaderLibrary library(nullptr, vfs, *root);
			const Result<const ShaderReflection*> unread = library.GetReflection("Unread", "VSMain");
			REQUIRE_MESSAGE(unread.has_value(), unread.error().ToString());
			REQUIRE(library.GetReflection("Read", "VSMain").has_value());

			// Both variants were rebuilt; the reload reads one of them.
			const std::string_view rebuilt = R"({ "entryPoints": [ { "name": "VSMain", "stage": "fragment" } ] })";
			WriteText(mount, "Read/VSMain.refl.json", rebuilt);
			WriteText(mount, "Unread/VSMain.refl.json", rebuilt);
			library.BeginReload();
			const Result<const ShaderReflection*> read = library.GetReflection("Read", "VSMain");
			REQUIRE(read.has_value());
			library.EndReload(true);

			const Result<const ShaderReflection*> readAfter = library.GetReflection("Read", "VSMain");
			REQUIRE(readAfter.has_value());
			CHECK(*readAfter == *read);
			CHECK((*readAfter)->Stage == nvrhi::ShaderType::Pixel);
			// A variant first asked for after the reload is read from the build output, like any other.
			const Result<const ShaderReflection*> unreadAfter = library.GetReflection("Unread", "VSMain");
			REQUIRE(unreadAfter.has_value());
			CHECK(*unreadAfter == *unread);
			CHECK((*unreadAfter)->Stage == nvrhi::ShaderType::Vertex);
		}

		TEST_CASE("ShaderLibrary: a failed reload of a variant keeps its earlier reflection")
		{
			// The reload's error is the variant's own (NotFound for a variant the build did not write, Parse for one it broke),
			// and dropping the reload leaves the earlier reflection in use.
			VirtualFileSystem vfs;
			Scope<MemoryMount> ownedMount = CreateScope<MemoryMount>();
			MemoryMount& mount = *ownedMount;
			REQUIRE(vfs.Mount(ShaderScheme, std::move(ownedMount)).has_value());
			WriteText(mount, "Probe/VSMain.refl.json", MakeVertexReflection("VSMain"));
			Result<VfsPath> root = VfsPath::Create(ShaderScheme, "");
			REQUIRE(root.has_value());
			ShaderLibrary library(nullptr, vfs, *root);
			const Result<const ShaderReflection*> original = library.GetReflection("Probe", "VSMain");
			REQUIRE_MESSAGE(original.has_value(), original.error().ToString());

			WriteText(mount, "Probe/VSMain.refl.json", "{ \"entryPoints\": ");
			library.BeginReload();
			const Result<const ShaderReflection*> broken = library.GetReflection("Probe", "VSMain");
			REQUIRE_FALSE(broken.has_value());
			CHECK(broken.error().GetCode() == ErrorCode::Parse);
			library.EndReload(false);

			Result<VfsPath> file = VfsPath::Create(ShaderScheme, "Probe/VSMain.refl.json");
			REQUIRE(file.has_value());
			REQUIRE(mount.Remove(*file).has_value());
			library.BeginReload();
			const Result<const ShaderReflection*> missing = library.GetReflection("Probe", "VSMain");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			library.EndReload(false);

			const Result<const ShaderReflection*> kept = library.GetReflection("Probe", "VSMain");
			REQUIRE(kept.has_value());
			CHECK(*kept == *original);
			CHECK((*kept)->Stage == nvrhi::ShaderType::Vertex);
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

		TEST_CASE("ShaderLibrary: Get creates each shader once" * doctest::test_suite(Test::GpuSuite))
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

			// A missing variant is NotFound naming the .spv it expected.
			const Result<nvrhi::ShaderHandle> missing = library.Get("Triangle", "GSMain");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().ToString().contains("Triangle/GSMain.spv"));
		}

		TEST_CASE("ShaderLibrary: a reload creates the shaders it reads again, and a dropped reload restores the earlier ones"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (R-SHR; Docs/Decisions/0029-m18-contract.md decision 3): the shader half of a reload. The Triangle program's
			// compiled files are copied into a memory mount, which the case rewrites as a rebuild of the Shaders project would.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			VirtualFileSystem vfs;
			Scope<MemoryMount> ownedMount = CreateScope<MemoryMount>();
			MemoryMount& mount = *ownedMount;
			REQUIRE(vfs.Mount(ShaderScheme, std::move(ownedMount)).has_value());
			std::map<std::string, Buffer> compiled;
			for (const std::string_view entry : { std::string_view("VSMain"), std::string_view("PSMain") })
			{
				for (const std::string_view extension : { std::string_view(".spv"), std::string_view(".refl.json") })
				{
					const std::string path = std::format("Triangle/{}{}", entry, extension);
					Result<VfsPath> file = VfsPath::Create(ShaderScheme, path);
					REQUIRE(file.has_value());
					Result<Buffer> contents = gpu.GetVfs().ReadFile(*file);
					REQUIRE_MESSAGE(contents.has_value(), contents.error().ToString());
					WriteBytes(mount, path, *contents);
					compiled.emplace(path, std::move(*contents));
				}
			}
			Result<VfsPath> root = VfsPath::Create(ShaderScheme, "");
			REQUIRE(root.has_value());
			{
				ShaderLibrary library(&device, vfs, *root);
				const Result<nvrhi::ShaderHandle> vertex = library.Get("Triangle", "VSMain");
				REQUIRE_MESSAGE(vertex.has_value(), vertex.error().ToString());
				const Result<nvrhi::ShaderHandle> pixel = library.Get("Triangle", "PSMain");
				REQUIRE_MESSAGE(pixel.has_value(), pixel.error().ToString());

				// A broken rebuild of the vertex shader fails the reload's Get; dropping the reload restores the shader created
				// before it.
				WriteText(mount, "Triangle/VSMain.spv", "not SPIR-V");
				library.BeginReload();
				const Result<nvrhi::ShaderHandle> broken = library.Get("Triangle", "VSMain");
				REQUIRE_FALSE(broken.has_value());
				CHECK(broken.error().GetCode() == ErrorCode::Validation);
				library.EndReload(false);
				const Result<nvrhi::ShaderHandle> restored = library.Get("Triangle", "VSMain");
				REQUIRE(restored.has_value());
				CHECK(restored->Get() == vertex->Get());

				// Fixed: a kept reload creates the vertex shader again from the rewritten file and keeps it. The pixel shader,
				// which the reload did not read, stays as it was although its file changed.
				WriteBytes(mount, "Triangle/VSMain.spv", compiled["Triangle/VSMain.spv"]);
				WriteText(mount, "Triangle/PSMain.spv", "not SPIR-V");
				library.BeginReload();
				const Result<nvrhi::ShaderHandle> reloaded = library.Get("Triangle", "VSMain");
				REQUIRE_MESSAGE(reloaded.has_value(), reloaded.error().ToString());
				CHECK(reloaded->Get() != vertex->Get());
				library.EndReload(true);
				const Result<nvrhi::ShaderHandle> current = library.Get("Triangle", "VSMain");
				REQUIRE(current.has_value());
				CHECK(current->Get() == reloaded->Get());
				const Result<nvrhi::ShaderHandle> unread = library.Get("Triangle", "PSMain");
				REQUIRE(unread.has_value());
				CHECK(unread->Get() == pixel->Get());

				// A kept reload that read only a variant's reflection drops the variant's earlier shader as well, so the next
				// Get creates it again from the build output.
				WriteBytes(mount, "Triangle/PSMain.spv", compiled["Triangle/PSMain.spv"]);
				library.BeginReload();
				REQUIRE(library.GetReflection("Triangle", "PSMain").has_value());
				library.EndReload(true);
				const Result<nvrhi::ShaderHandle> recreated = library.Get("Triangle", "PSMain");
				REQUIRE_MESSAGE(recreated.has_value(), recreated.error().ToString());
				CHECK(recreated->Get() != pixel->Get());
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("ShaderLibrary: a device loss drops the shaders, keeps every reflection and creates the shaders again"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.3, Docs/Decisions/0029-m18-contract.md decision 33): the library registers itself; a lost
			// device's shader modules are dropped, the reflections (CPU data) stay at their addresses, and the next Get creates
			// the shader on the recovered device.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			ShaderLibrary& library = gpu.GetShaders();
			Result<const ShaderReflection*> reflection = library.GetReflection("Triangle", "VSMain");
			REQUIRE_MESSAGE(reflection.has_value(), reflection.error().ToString());
			Result<nvrhi::ShaderHandle> before = library.Get("Triangle", "VSMain");
			REQUIRE_MESSAGE(before.has_value(), before.error().ToString());
			before->Reset();
			const std::vector<std::string> owners = device.GetRecreation().GetOwnerNames();
			CHECK(std::ranges::find(owners, std::string("ShaderLibrary")) != owners.end());

			device.GetDiagnostics().SetDeviceLost(true);
			device.ReportDeviceLost("ShaderLibraryTests");
			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			Result<const ShaderReflection*> kept = library.GetReflection("Triangle", "VSMain");
			REQUIRE(kept.has_value());
			CHECK(*kept == *reflection);
			Result<nvrhi::ShaderHandle> after = library.Get("Triangle", "VSMain");
			REQUIRE_MESSAGE(after.has_value(), after.error().ToString());
			CHECK(*after != nullptr);
		}

		TEST_CASE("ShaderLibrary: after a device loss a shader is created again from the SPIR-V read before, not from a rebuilt file"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 (C18.3, decision 33): each created variant keeps its SPIR-V as CPU data, so a recovery never pairs a rebuilt
			// file with the reflection read before it. The variant is copied from the build output into a memory mount and
			// created; then the file is overwritten with bytes that are not SPIR-V, as an unreloaded rebuild would change it,
			// and the shader is created again after the loss all the same.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			VirtualFileSystem vfs;
			Scope<MemoryMount> ownedMount = CreateScope<MemoryMount>();
			MemoryMount& mount = *ownedMount;
			REQUIRE(vfs.Mount(ShaderScheme, std::move(ownedMount)).has_value());
			for (const std::string_view extension : { std::string_view(".spv"), std::string_view(".refl.json") })
			{
				Result<VfsPath> path = VfsPath::Create(ShaderScheme, std::format("Triangle/VSMain{}", extension));
				REQUIRE(path.has_value());
				Result<Buffer> contents = gpu.GetVfs().ReadFile(*path);
				REQUIRE_MESSAGE(contents.has_value(), contents.error().ToString());
				REQUIRE(mount.CreateDirectories(path->GetParent()).has_value());
				REQUIRE(mount.WriteFileAtomic(*path, *contents).has_value());
			}
			Result<VfsPath> root = VfsPath::Create(ShaderScheme, "");
			REQUIRE(root.has_value());
			{
				ShaderLibrary library(&device, vfs, *root);
				Result<nvrhi::ShaderHandle> before = library.Get("Triangle", "VSMain");
				REQUIRE_MESSAGE(before.has_value(), before.error().ToString());
				before->Reset();
				WriteText(mount, "Triangle/VSMain.spv", "not SPIR-V");

				device.GetDiagnostics().SetDeviceLost(true);
				device.ReportDeviceLost("ShaderLibraryTests");
				const Status recovered = device.Recover(nullptr);
				REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
				Result<nvrhi::ShaderHandle> after = library.Get("Triangle", "VSMain");
				REQUIRE_MESSAGE(after.has_value(), after.error().ToString());
				CHECK(*after != nullptr);
			}
			device.RunGarbageCollection();
		}
	}

}
