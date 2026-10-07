#include "TestsPCH.h"

#include "Engine/Asset/PakMount.h"

#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/AssetPipeline/PakWriter.h"
#include "Support/AssetTestFixture.h"

namespace Engine {

	namespace {

		Ref<const PakReader> MakeShaderPak()
		{
			PakWriter writer;
			const std::string forward = "forward";
			const std::string triangle = "triangle";
			REQUIRE(writer.Add({ .Handle = AssetHandle(), .Type = "File", .Path = "Shaders/Forward_VSMain.spv", .Data = Buffer(AsBytes(forward).begin(), AsBytes(forward).end()) })
					.has_value());
			REQUIRE(writer.Add({ .Handle = AssetHandle(), .Type = "File", .Path = "Shaders/Passes/Triangle_VSMain.spv", .Data = Buffer(AsBytes(triangle).begin(), AsBytes(triangle).end()) })
					.has_value());
			REQUIRE(writer.Add({ .Handle = AssetHandle(0x101), .Type = "Mesh", .Path = "engine://Meshes/Cube", .Data = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Cube), 1) })
					.has_value());
			Result<Ref<const PakReader>> reader = PakReader::OpenMemory(writer.Build(), "Engine.pak");
			REQUIRE_MESSAGE(reader.has_value(), reader.error().ToString());
			return *reader;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("PakMount: plain files are served read-only and cooked assets are not files" * doctest::skip(true))
		{
			Result<Scope<PakMount>> created = PakMount::Create(MakeShaderPak());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("engine", std::move(*created)).has_value());

			Result<std::string> forward = vfs.ReadText(Test::ParseVfsPath("engine://Shaders/Forward_VSMain.spv"));
			REQUIRE(forward.has_value());
			CHECK(*forward == "forward");
			Result<FileInfo> directory = vfs.GetInfo(Test::ParseVfsPath("engine://Shaders/Passes"));
			REQUIRE(directory.has_value());
			CHECK(directory->IsDirectory);
			Result<std::vector<VfsEntry>> listed = vfs.List(Test::ParseVfsPath("engine://Shaders"), true);
			REQUIRE(listed.has_value());
			REQUIRE(listed->size() == 3);
			CHECK((*listed)[0].Path.ToString() == "engine://Shaders/Forward_VSMain.spv");
			CHECK((*listed)[1].Path.ToString() == "engine://Shaders/Passes");
			CHECK((*listed)[2].Path.ToString() == "engine://Shaders/Passes/Triangle_VSMain.spv");

			// The cube is a cooked asset (RuntimeAssetManager), not a file of the mount.
			CHECK_FALSE(vfs.Exists(Test::ParseVfsPath("engine://Meshes/Cube")));

			const std::string data = "x";
			const Status written = vfs.WriteFileAtomic(Test::ParseVfsPath("engine://Shaders/New.spv"), AsBytes(data));
			REQUIRE_FALSE(written.has_value());
			CHECK(written.error().GetCode() == ErrorCode::PermissionDenied);
			Result<Buffer> wrongCase = vfs.ReadFile(Test::ParseVfsPath("engine://shaders/Forward_VSMain.spv"));
			REQUIRE_FALSE(wrongCase.has_value());
			CHECK(wrongCase.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("PakMount: streams read from the pak" * doctest::skip(true))
		{
			Result<Scope<PakMount>> mount = PakMount::Create(MakeShaderPak());
			REQUIRE(mount.has_value());
			Result<Scope<IFileStream>> stream = (*mount)->Open(Test::ParseVfsPath("engine://Shaders/Forward_VSMain.spv"));
			REQUIRE_MESSAGE(stream.has_value(), stream.error().ToString());
			CHECK((*stream)->GetSize() == 7);
			std::array<std::byte, 3> head{};
			Result<size_t> read = (*stream)->Read(head);
			REQUIRE(read.has_value());
			CHECK(*read == 3);
			CHECK(AsStringView(head) == "for");
		}
	}

}
