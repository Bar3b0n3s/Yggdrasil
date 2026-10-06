#include "TestsPCH.h"
#include "Support/TempDirectory.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Support/TestCaseTracker.h"

#include <atomic>
#include <random>
#include <system_error>

namespace Engine {

	namespace Test {

		namespace Utils {

			// Attempts at an unused name before giving up; a collision needs another process with the same label and
			// the same 64-bit suffix.
			constexpr int MaxNameAttempts = 16;

			static bool IsValidLabel(std::string_view label)
			{
				return !label.empty() && std::ranges::all_of(label, [](char character)
				{
					return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
						|| (character >= '0' && character <= '9') || character == '-' || character == '_';
				});
			}

			static uint64_t MixBits(uint64_t value)
			{
				// SplitMix64's finalizer: every input bit affects every output bit.
				value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
				value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
				return value ^ (value >> 31);
			}

			// 64 bits that differ between processes (OS entropy) and between calls (a counter), so that parallel runs and
			// a parent with its child processes never pick the same name. Not simulation code: the name is never test data.
			static uint64_t NextNameSuffix()
			{
				static const uint64_t ProcessSeed = []()
				{
					std::random_device entropy;
					const uint64_t high = entropy();
					const uint64_t low = entropy();
					const uint64_t clockTicks = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
					return (high << 32) ^ low ^ MixBits(clockTicks);
				}();
				static std::atomic<uint64_t> s_Counter = 0;
				return MixBits(ProcessSeed + 0x9e3779b97f4a7c15ull * s_Counter.fetch_add(1, std::memory_order_relaxed));
			}

			static std::string PathToUtf8(const std::filesystem::path& path)
			{
				const std::u8string text = path.u8string();
				return std::string(reinterpret_cast<const char*>(text.data()), text.size());
			}

			static std::filesystem::path PathFromUtf8(std::string_view text)
			{
				return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
			}

			// Whether `relative` could name something outside the directory it is appended to.
			static bool LeavesDirectory(const std::filesystem::path& relative)
			{
				if (relative.has_root_path())
					return true;
				for (const std::filesystem::path& element : relative)
				{
					if (element == "..")
						return true;
				}
				return false;
			}

			// The documented failure: inside a test case, doctest FAIL stops the case (so it never writes elsewhere);
			// outside one (a death-test child) the process cannot continue safely either.
			static void FailCreation(const std::string& message)
			{
				if (GetRunningTestCase().has_value())
					FAIL(message);
				FatalError(FatalErrorKind::InitFailed, message);
			}

		}

		TempDirectory::TempDirectory(std::string_view label)
		{
			ENGINE_CORE_ASSERT(Utils::IsValidLabel(label), "TempDirectory label '{}' must be letters, digits, '-' and '_'", label);

			std::error_code error;
			std::filesystem::path root = std::filesystem::temp_directory_path(error);
			if (!error && !root.is_absolute())
				root = std::filesystem::absolute(root, error);
			if (error)
			{
				Utils::FailCreation(std::format("Cannot find the temporary directory: {}", error.message()));
				return;
			}
			root /= "EngineTests";
			std::filesystem::create_directories(root, error);
			if (error)
			{
				Utils::FailCreation(std::format("Cannot create '{}': {}", Utils::PathToUtf8(root), error.message()));
				return;
			}

			for (int attempt = 0; attempt < Utils::MaxNameAttempts; ++attempt)
			{
				std::filesystem::path candidate = root / std::format("{}-{:016x}", label, Utils::NextNameSuffix());
				const bool created = std::filesystem::create_directory(candidate, error);
				if (error)
				{
					Utils::FailCreation(std::format("Cannot create '{}': {}", Utils::PathToUtf8(candidate), error.message()));
					return;
				}
				if (created)
				{
					m_Path = std::move(candidate);
					return;
				}
			}
			Utils::FailCreation(std::format("Found no unused name for a '{}' directory in '{}' after {} attempts", label,
				Utils::PathToUtf8(root), Utils::MaxNameAttempts));
		}

		TempDirectory::~TempDirectory()
		{
			if (m_Path.empty())
				return;
			std::error_code error;
			std::filesystem::remove_all(m_Path, error);
			if (error)
				ENGINE_CORE_WARN("Cannot remove the temporary directory '{}': {}", Utils::PathToUtf8(m_Path), error.message());
		}

		std::filesystem::path TempDirectory::operator/(std::string_view relative) const
		{
			std::filesystem::path relativePath = Utils::PathFromUtf8(relative);
			ENGINE_CORE_ASSERT(!Utils::LeavesDirectory(relativePath),
				"TempDirectory paths stay inside the directory: '{}' is absolute or contains '..'", relative);
			relativePath.make_preferred();
			return m_Path / relativePath;
		}

	}

}
