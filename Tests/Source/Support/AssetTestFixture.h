#pragma once

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Shared setup for asset tests (Roadmap M6): an EditorAssetManager over memory mounts, with every service it needs, so the
// registry, the importers, the cache and hot reload are tested without a disk, a window or a GPU (§15.1 T1).

namespace Engine {

	namespace Test {

		// One asset environment of one test: project:// and cache:// as MemoryMounts (project://Assets created), a JobSystem
		// with `workerCount` workers (0, the default: inline, every job runs inside the call that submits it, so tests are
		// deterministic; race tests that need overlapping jobs pass workers and synchronize through latches, never sleeps), a
		// main-thread queue, an event log, a frozen registry (the built-in components, the project settings, the Asset types
		// and the importers' settings types: Test::CreateBuiltinRegistry), the built-in importers and loaders, a
		// deterministic id generator, and an EditorAssetManager over them, not yet opened. A test may register an importer of
		// its own (GetImporters) before OpenProject. Failed setup steps fail the running test case. Main thread; not copyable
		// or movable (the manager points at the services; the manager and its jobs are gone before the services).
		class AssetTestFixture
		{
		public:
			explicit AssetTestFixture(uint64_t seed = 1, uint32_t workerCount = 0);
			~AssetTestFixture();

			AssetTestFixture(const AssetTestFixture&) = delete;
			AssetTestFixture& operator=(const AssetTestFixture&) = delete;

			[[nodiscard]] VirtualFileSystem& GetVfs() { return m_Vfs; }
			[[nodiscard]] MainThreadQueue& GetMainThreadQueue() { return m_MainThreadQueue; }
			[[nodiscard]] JobSystem& GetJobSystem() { return m_JobSystem; }
			[[nodiscard]] EventLog& GetEventLog() { return m_EventLog; }
			[[nodiscard]] const TypeRegistry& GetRegistry() const { return *m_Registry; }
			[[nodiscard]] UUIDGenerator& GetGenerator() { return m_Generator; }
			[[nodiscard]] ImporterRegistry& GetImporters() { return m_Importers; }
			[[nodiscard]] AssetLoaderRegistry& GetLoaders() { return m_Loaders; }
			[[nodiscard]] EditorAssetManager& GetManager() { return *m_Manager; }

			// project://<relative> ("Assets/Textures/Wood.png"); fails the test case for an invalid path.
			[[nodiscard]] VfsPath ProjectPath(std::string_view relative) const;

			// Writes `bytes` to project://<relative> directly through the VFS (an external write the editor did not make),
			// creating missing parent directories; fails the test case on error.
			void WriteProjectFile(std::string_view relative, std::span<const std::byte> bytes);
			void WriteProjectText(std::string_view relative, std::string_view text);

			// The bytes of project://<relative>; fails the test case on error.
			[[nodiscard]] Buffer ReadProjectFile(std::string_view relative);

			// EditorAssetManager::OpenProject on project://Assets with cache:// (writable); fails the test case on error.
			AssetRefreshReport OpenProject(bool hotReload = true);
		private:
			VirtualFileSystem m_Vfs;
			MainThreadQueue m_MainThreadQueue;
			JobSystem m_JobSystem; // inline unless the constructor got workers
			EventLog m_EventLog;
			Scope<TypeRegistry> m_Registry;
			UUIDGenerator m_Generator;
			ImporterRegistry m_Importers;
			AssetLoaderRegistry m_Loaders;
			Scope<EditorAssetManager> m_Manager;
		};

		// VfsPath::Parse(text); fails the test case on error.
		[[nodiscard]] VfsPath ParseVfsPath(std::string_view text);

		// A width x height RGBA8 PNG whose texels are a deterministic function of their position and `seed` (EncodePng from
		// Graphics/Image.h); fails the test case on error.
		[[nodiscard]] Buffer MakeTestPng(uint32_t width, uint32_t height, uint32_t seed = 0);

	}

}
