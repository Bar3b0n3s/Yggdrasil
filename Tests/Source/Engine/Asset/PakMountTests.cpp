#include "TestsPCH.h"

#include "Engine/Asset/PakMount.h"

#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/AssetPipeline/PakWriter.h"
#include "Support/AssetTestFixture.h"

namespace Engine {

	namespace {

		Buffer MakeShaderPakBytes()
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
			return writer.Build();
		}

		Ref<const PakReader> MakeShaderPak()
		{
			Result<Ref<const PakReader>> reader = PakReader::OpenMemory(MakeShaderPakBytes(), "Engine.pak");
			REQUIRE_MESSAGE(reader.has_value(), reader.error().ToString());
			return *reader;
		}

		// The error code of a failed call; Unknown when it succeeded (so a CHECK against the expected code fails).
		template<typename T>
		ErrorCode ErrorCodeOf(const Result<T>& result)
		{
			return result.has_value() ? ErrorCode::Unknown : result.error().GetCode();
		}

		// A pak of plain files at `paths`, each holding its own path.
		Ref<const PakReader> MakeFilePak(const std::vector<std::string>& paths)
		{
			PakWriter writer;
			for (const std::string& path : paths)
				REQUIRE(writer.Add({ .Handle = AssetHandle(), .Type = "File", .Path = path, .Data = Buffer(AsBytes(path).begin(), AsBytes(path).end()) }).has_value());
			Result<Ref<const PakReader>> reader = PakReader::OpenMemory(writer.Build(), "Files.pak");
			REQUIRE_MESSAGE(reader.has_value(), reader.error().ToString());
			return *reader;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("PakMount: plain files are served read-only and cooked assets are not files")
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

		TEST_CASE("PakMount: streams read from the pak")
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

		TEST_CASE("PakMount: a stream seeks, stops at the end and outlives its mount")
		{
			Scope<IFileStream> stream;
			{
				Result<Scope<PakMount>> mount = PakMount::Create(MakeShaderPak());
				REQUIRE(mount.has_value());
				Result<Scope<IFileStream>> opened = (*mount)->Open(Test::ParseVfsPath("engine://Shaders/Passes/Triangle_VSMain.spv"));
				REQUIRE(opened.has_value());
				stream = std::move(*opened);
			}
			REQUIRE(stream->Seek(5).has_value());
			CHECK(stream->GetPosition() == 5);
			std::array<std::byte, 8> rest{};
			Result<size_t> read = stream->Read(rest);
			REQUIRE(read.has_value());
			CHECK(*read == 3);
			CHECK(AsStringView(std::span(rest).first(3)) == "gle");
			Result<size_t> end = stream->Read(rest);
			REQUIRE(end.has_value());
			CHECK(*end == 0);
			const Status beyond = stream->Seek(9);
			REQUIRE_FALSE(beyond.has_value());
			CHECK(beyond.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("PakMount: every mutation is PermissionDenied and the root lists the top level")
		{
			Result<Scope<PakMount>> created = PakMount::Create(MakeShaderPak());
			REQUIRE(created.has_value());
			PakMount& mount = **created;
			CHECK(mount.GetAccess() == MountAccess::ReadOnly);
			const VfsPath file = Test::ParseVfsPath("engine://Shaders/Forward_VSMain.spv");
			const VfsPath other = Test::ParseVfsPath("engine://Shaders/Renamed.spv");
			CHECK(ErrorCodeOf(mount.Remove(file)) == ErrorCode::PermissionDenied);
			CHECK(ErrorCodeOf(mount.Move(file, other)) == ErrorCode::PermissionDenied);
			CHECK(ErrorCodeOf(mount.CreateDirectories(Test::ParseVfsPath("engine://New"))) == ErrorCode::PermissionDenied);

			Result<std::vector<VfsEntry>> top = mount.List(Test::ParseVfsPath("engine://"), false);
			REQUIRE(top.has_value());
			REQUIRE(top->size() == 1);
			CHECK((*top)[0].Path.ToString() == "engine://Shaders");
			Result<FileInfo> info = mount.GetInfo(file);
			REQUIRE(info.has_value());
			CHECK_FALSE(info->IsDirectory);
			CHECK(info->Size == 7);
			Result<Buffer> directory = mount.ReadFile(Test::ParseVfsPath("engine://Shaders"));
			REQUIRE_FALSE(directory.has_value());
			CHECK(directory.error().GetCode() == ErrorCode::Io);
			CHECK(ErrorCodeOf(mount.ReadFile(Test::ParseVfsPath("engine://Shaders/Missing.spv"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.List(file, false)) == ErrorCode::Io);
			CHECK(ErrorCodeOf(mount.GetInfo(Test::ParseVfsPath("engine://Shaders/Forward_VSMain.spv/Below"))) == ErrorCode::NotFound);
		}

		TEST_CASE("PakMount: paths that break the case policy are refused at creation")
		{
			// Two files whose names differ only in case, two directories that do, and a file that is also a directory.
			for (const std::vector<std::string>& paths : std::vector<std::vector<std::string>>{
					 { "Shaders/A.spv", "Shaders/a.spv" },
					 { "Shaders/A.spv", "shaders/B.spv" },
					 { "Shaders", "Shaders/A.spv" },
				 })
			{
				CAPTURE(paths.front());
				Result<Scope<PakMount>> mount = PakMount::Create(MakeFilePak(paths));
				REQUIRE_FALSE(mount.has_value());
				CHECK(mount.error().GetCode() == ErrorCode::Validation);
			}
			CHECK(PakMount::Create(MakeFilePak({ "Shaders/A.spv", "Shaders/B.spv", "Textures/A.spv" })).has_value());
		}

		TEST_CASE("PakMount: a corrupted plain file fails its read with the hash error")
		{
			Buffer pak = MakeShaderPakBytes();
			Result<Ref<const PakReader>> intact = PakReader::OpenMemory(pak, "Intact.pak");
			REQUIRE(intact.has_value());
			pak[(*intact)->FindByPath("Shaders/Forward_VSMain.spv")->Offset] ^= std::byte{ 0x04 };
			Result<Ref<const PakReader>> reader = PakReader::OpenMemory(std::move(pak), "Engine.pak");
			REQUIRE(reader.has_value());
			Result<Scope<PakMount>> mount = PakMount::Create(*reader);
			REQUIRE(mount.has_value());
			Result<Buffer> read = (*mount)->ReadFile(Test::ParseVfsPath("engine://Shaders/Forward_VSMain.spv"));
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::Validation);
			CHECK(read.error().ToString().contains("corrupted"));
			CHECK_FALSE((*mount)->Open(Test::ParseVfsPath("engine://Shaders/Forward_VSMain.spv")).has_value());
		}
	}

}
