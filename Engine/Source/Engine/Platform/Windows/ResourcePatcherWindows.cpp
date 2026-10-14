#include "EnginePCH.h"
#include "Engine/Platform/ResourcePatcher.h"

// ResourcePatcher::Patch on Windows: BeginUpdateResourceW, UpdateResourceW and EndUpdateResourceW (ResourcePatcher.h).

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Error.h"
	#include "Engine/Platform/Private/PathsUtf8.h"

	#include <windows.h>

	#include <chrono>
	#include <optional>
	#include <system_error>
	#include <thread>
	#include <vector>

namespace Engine {

	namespace Utils {

		// The resource types Patch writes or removes (winuser.h's RT_ICON, RT_GROUP_ICON and RT_VERSION, spelled for the W
		// functions whatever UNICODE says).
		constexpr WORD IconResourceType = 3;
		constexpr WORD IconGroupResourceType = 14;
		constexpr WORD VersionResourceType = 16;
		// How long a file another process holds (an antivirus scanner inspecting the fresh copy) is retried, and how long
		// each retry waits: a bound of the failure only, never part of a passing outcome.
		constexpr std::chrono::seconds SharingViolationTimeout{ 10 };
		constexpr std::chrono::milliseconds SharingViolationRetryDelay{ 50 };

		// One resource already in the executable, which Patch removes unless it writes the same id.
		struct ExistingResource
		{
			WORD Type = 0;
			WORD Id = 0;
			WORD Language = 0;
		};

		// The outcome of one step of an attempt: the system error code of the call that failed (0 on success) and its name.
		struct PatchFailure
		{
			DWORD Code = 0;
			const char* Call = "";
		};

		static std::string DescribeResourceError(DWORD code)
		{
			return std::system_category().message(static_cast<int>(code));
		}

		static bool IsSharingViolation(DWORD code)
		{
			return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
		}

		// The last error, never 0 (a call that failed without setting it reports ERROR_GEN_FAILURE).
		static DWORD GetFailureCode()
		{
			const DWORD code = GetLastError();
			return code == 0 ? static_cast<DWORD>(ERROR_GEN_FAILURE) : code;
		}

		// Frees a module loaded as a data file on destruction.
		class ScopedDataModule
		{
		public:
			explicit ScopedDataModule(HMODULE module)
				: m_Module(module)
			{
			}

			~ScopedDataModule()
			{
				if (m_Module != nullptr)
					FreeLibrary(m_Module);
			}

			ScopedDataModule(const ScopedDataModule&) = delete;
			ScopedDataModule& operator=(const ScopedDataModule&) = delete;

			[[nodiscard]] HMODULE Get() const { return m_Module; }
		private:
			HMODULE m_Module = nullptr;
		};

		// What the resource enumeration callbacks collect for one resource type.
		struct ResourceSearch
		{
			WORD Type = 0;
			std::vector<WORD> Ids{};
			std::vector<ExistingResource> Found{};
		};

		// Appends to `found` the integer-named icon and icon-group resources `executable` holds, with their languages: what an
		// earlier patch wrote. The module is released before the update opens the file.
		static PatchFailure FindIconResources(const std::filesystem::path& executable, std::vector<ExistingResource>& found)
		{
			const ScopedDataModule module(LoadLibraryExW(executable.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE));
			if (module.Get() == nullptr)
				return { .Code = GetFailureCode(), .Call = "LoadLibraryExW" };
			for (const WORD type : { IconResourceType, IconGroupResourceType })
			{
				ResourceSearch search{ .Type = type, .Ids = {}, .Found = {} };
				const auto collectName = [](HMODULE /*module*/, LPCWSTR /*type*/, LPWSTR name, LONG_PTR parameter) -> BOOL
				{
					if (IS_INTRESOURCE(name))
						reinterpret_cast<ResourceSearch*>(parameter)->Ids.push_back(static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name) & 0xFFFFu));
					return TRUE;
				};
				// A type the file has no resource of is ERROR_RESOURCE_TYPE_NOT_FOUND: nothing to remove.
				static_cast<void>(EnumResourceNamesW(module.Get(), MAKEINTRESOURCEW(type), collectName, reinterpret_cast<LONG_PTR>(&search)));
				for (const WORD id : search.Ids)
				{
					const auto collectLanguage = [](HMODULE /*module*/, LPCWSTR /*type*/, LPCWSTR name, WORD language, LONG_PTR parameter) -> BOOL
					{
						ResourceSearch& owner = *reinterpret_cast<ResourceSearch*>(parameter);
						owner.Found.push_back({ .Type = owner.Type, .Id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name) & 0xFFFFu), .Language = language });
						return TRUE;
					};
					static_cast<void>(EnumResourceLanguagesW(module.Get(), MAKEINTRESOURCEW(type), MAKEINTRESOURCEW(id), collectLanguage,
						reinterpret_cast<LONG_PTR>(&search)));
				}
				found.insert(found.end(), search.Found.begin(), search.Found.end());
			}
			return {};
		}

		// One attempt at the whole update: removes the stale icon resources, writes the new ones and the version, and commits;
		// a failed step discards the update, which leaves the file as it was.
		static PatchFailure UpdateResources(const std::filesystem::path& executable, const std::vector<ExistingResource>& stale,
			const std::optional<Buffer>& iconGroup, const ExecutableResourceSet& resources, const Buffer& version)
		{
			const HANDLE update = BeginUpdateResourceW(executable.c_str(), FALSE);
			if (update == nullptr)
				return { .Code = GetFailureCode(), .Call = "BeginUpdateResourceW" };
			const auto write = [update](WORD type, WORD id, const Buffer& data)
			{
				// UpdateResourceW takes the data as LPVOID although it only reads it (a const-incorrect C API).
				return UpdateResourceW(update, MAKEINTRESOURCEW(type), MAKEINTRESOURCEW(id), ResourcePatcher::LanguageId,
						   const_cast<std::byte*>(data.data()), static_cast<DWORD>(data.size()))
					!= FALSE;
			};
			const auto discard = [update](const char* call) -> PatchFailure
			{
				const DWORD code = GetFailureCode();
				static_cast<void>(EndUpdateResourceW(update, TRUE));
				return { .Code = code, .Call = call };
			};

			// What an earlier patch wrote and this one does not replace goes, so the icon group never names a stale image.
			for (const ExistingResource& resource : stale)
			{
				const bool replaced = resource.Language == ResourcePatcher::LanguageId
					&& ((resource.Type == IconGroupResourceType && resource.Id == ResourcePatcher::IconGroupId && iconGroup.has_value())
						|| (resource.Type == IconResourceType && resource.Id >= ResourcePatcher::FirstIconId
							&& resource.Id < ResourcePatcher::FirstIconId + resources.Icons.size()));
				if (replaced)
					continue;
				if (UpdateResourceW(update, MAKEINTRESOURCEW(resource.Type), MAKEINTRESOURCEW(resource.Id), resource.Language, nullptr, 0) == FALSE)
					return discard("UpdateResourceW (removing an earlier icon resource)");
			}
			for (size_t index = 0; index < resources.Icons.size(); ++index)
			{
				if (!write(IconResourceType, static_cast<WORD>(ResourcePatcher::FirstIconId + index), resources.Icons[index].Png))
					return discard("UpdateResourceW (RT_ICON)");
			}
			if (iconGroup.has_value() && !write(IconGroupResourceType, ResourcePatcher::IconGroupId, *iconGroup))
				return discard("UpdateResourceW (RT_GROUP_ICON)");
			if (!write(VersionResourceType, ResourcePatcher::VersionId, version))
				return discard("UpdateResourceW (RT_VERSION)");
			if (EndUpdateResourceW(update, FALSE) == FALSE)
				return { .Code = GetFailureCode(), .Call = "EndUpdateResourceW" };
			return {};
		}

	}

	Status ResourcePatcher::Patch(const std::filesystem::path& executable, const ExecutableResourceSet& resources)
	{
		ENGINE_TRY_ASSIGN(const Buffer version, BuildVersionInfo(resources.Version));
		std::optional<Buffer> iconGroup;
		if (!resources.Icons.empty())
		{
			ENGINE_TRY_ASSIGN(Buffer group, BuildIconGroup(resources.Icons));
			iconGroup = std::move(group);
		}

		const auto deadline = std::chrono::steady_clock::now() + Utils::SharingViolationTimeout;
		for (;;)
		{
			std::vector<Utils::ExistingResource> stale;
			Utils::PatchFailure failure = Utils::FindIconResources(executable, stale);
			if (failure.Code == 0)
				failure = Utils::UpdateResources(executable, stale, iconGroup, resources, version);
			if (failure.Code == 0)
				return {};
			if (!Utils::IsSharingViolation(failure.Code) || std::chrono::steady_clock::now() >= deadline)
			{
				return MakeError(ErrorCode::Io, "{} failed for '{}': {}", failure.Call, Utils::NativePathToUtf8(executable),
					Utils::DescribeResourceError(failure.Code));
			}
			// Another process holds the file (a scanner inspecting the fresh copy): wait and try again within the bound.
			std::this_thread::sleep_for(Utils::SharingViolationRetryDelay);
		}
	}

}

#endif
