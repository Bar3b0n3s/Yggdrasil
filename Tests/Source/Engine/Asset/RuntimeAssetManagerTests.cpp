#include "TestsPCH.h"

#include "Engine/Asset/RuntimeAssetManager.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/AssetPipeline/PakWriter.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/RingBufferSink.h"
#include "Support/ExpectLog.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	namespace {

		constexpr AssetHandle TrackMesh{ 0x77e1a0c4d2b95f01ull };

		Buffer MakeGamePak(bool corruptMesh)
		{
			PakWriter writer;
			REQUIRE(writer.Add({ .Handle = TrackMesh, .Type = "Mesh", .Path = "Assets/Models/Track.glb#mesh:0:Straight", .Data = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Plane), 1) })
					.has_value());
			Buffer pak = writer.Build();
			if (corruptMesh)
			{
				// Flip a byte in the middle of the only entry's data (it starts right after the 64-byte header).
				pak[64 + 100] ^= std::byte{ 0x10 };
			}
			return pak;
		}

		// The manager's services for one test.
		struct RuntimeEnvironment
		{
			MainThreadQueue Queue;
			JobSystem Jobs{ 0, Queue };
			Scope<TypeRegistry> Registry = Test::CreateBuiltinRegistry();
			AssetLoaderRegistry Loaders;
		};

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("RuntimeAssetManager: serves cooked assets and built-ins from paks" * doctest::skip(true))
		{
			RuntimeEnvironment environment;
			RegisterBuiltinLoaders(environment.Loaders);
			RuntimeAssetManager manager({
				.Jobs = &environment.Jobs,
				.MainThread = &environment.Queue,
				.Registry = environment.Registry.get(),
				.Loaders = &environment.Loaders,
			});
			Result<Ref<const PakReader>> pak = PakReader::OpenMemory(MakeGamePak(false), "Game.pak");
			REQUIRE(pak.has_value());
			REQUIRE(manager.AddPak(*pak).has_value());

			CHECK(manager.Resolve("Assets/Models/Track.glb#mesh:0:Straight") == TrackMesh);
			CHECK(manager.Resolve(TrackMesh.ToString()) == TrackMesh);
			CHECK(manager.GetAssetType(TrackMesh) == AssetType::Mesh);
			CHECK(manager.GetReferencePath(TrackMesh) == "Assets/Models/Track.glb#mesh:0:Straight");
			CHECK(manager.GetMetadata(TrackMesh) == nullptr);
			Result<AssetRef<Asset>> loaded = manager.Load(TrackMesh);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK(AssetCast<MeshData>(*loaded) != nullptr);
			CHECK(manager.GetState(TrackMesh) == AssetState::Loaded);
			CHECK(manager.GetVersion(TrackMesh) == 1);

			// Procedural built-ins need no pak entry.
			CHECK(manager.Resolve("engine://Meshes/Cube") == BuiltinAssetHandles::CubeMesh);
			CHECK(manager.Load(BuiltinAssetHandles::CubeMesh).has_value());

			// The same pak twice: every handle is already served.
			Result<Ref<const PakReader>> again = PakReader::OpenMemory(MakeGamePak(false), "Again.pak");
			REQUIRE(again.has_value());
			const Status added = manager.AddPak(*again);
			REQUIRE_FALSE(added.has_value());
			CHECK(added.error().GetCode() == ErrorCode::AlreadyExists);
		}

		TEST_CASE("RuntimeAssetManager: a corrupted entry yields the placeholder and one diagnostic" * doctest::skip(true))
		{
			RuntimeEnvironment environment;
			RegisterBuiltinLoaders(environment.Loaders);
			RuntimeAssetManager manager({
				.Jobs = &environment.Jobs,
				.MainThread = &environment.Queue,
				.Registry = environment.Registry.get(),
				.Loaders = &environment.Loaders,
			});
			Result<Ref<const PakReader>> pak = PakReader::OpenMemory(MakeGamePak(true), "Game.pak");
			REQUIRE(pak.has_value());
			REQUIRE(manager.AddPak(*pak).has_value());

			Test::ExpectLog expected(LogLevel::Error, "corrupted");
			const AssetRef<MeshData> mesh = manager.GetOrPlaceholder<MeshData>(TrackMesh);
			REQUIRE(mesh != nullptr);
			CHECK(mesh.get() == AssetCast<MeshData>(manager.GetPlaceholder(AssetType::Mesh)).get());
			static_cast<void>(manager.GetOrPlaceholder<MeshData>(TrackMesh));
			CHECK(expected.GetMatchCount() == 1);
			CHECK(manager.GetState(TrackMesh) == AssetState::Failed);
			REQUIRE(manager.GetDiagnostics().size() == 1);
			CHECK(manager.GetDiagnostics().front().Code == AssetImportFailedCode);
		}

		TEST_CASE("RuntimeAssetManager: LoadAsync publishes on the main thread" * doctest::skip(true))
		{
			RuntimeEnvironment environment;
			RegisterBuiltinLoaders(environment.Loaders);
			RuntimeAssetManager manager({
				.Jobs = &environment.Jobs,
				.MainThread = &environment.Queue,
				.Registry = environment.Registry.get(),
				.Loaders = &environment.Loaders,
			});
			Result<Ref<const PakReader>> pak = PakReader::OpenMemory(MakeGamePak(false), "Game.pak");
			REQUIRE(pak.has_value());
			REQUIRE(manager.AddPak(*pak).has_value());
			JobHandle<AssetRef<Asset>> job = manager.LoadAsync(TrackMesh);
			manager.WaitIdle();
			REQUIRE(job.IsReady());
			CHECK(job.Take().has_value());
			CHECK(manager.GetState(TrackMesh) == AssetState::Loaded);
		}
	}

}
