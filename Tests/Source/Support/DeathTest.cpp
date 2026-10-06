#include "TestsPCH.h"
#include "Support/DeathTest.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Support/ChildProcess.h"
#include "Support/TestOptions.h"

#include <mutex>

namespace Engine {

	namespace Test {

		namespace {

			// The exit codes of a death-test child whose body did not terminate the process (§4.1 table).
			constexpr int DeathTestFailedExitCode = 1;
			constexpr int DeathTestUsageErrorExitCode = 2;

			struct DeathTestRegistration
			{
				std::string Name;
				DeathTestBody Body = nullptr;
				std::string File;
				int Line = 0;
			};

			struct DeathTestRegistry
			{
				std::mutex Mutex;
				std::vector<DeathTestRegistration> Registrations;
			};

		}

		namespace Utils {

			// Function-local static: ENGINE_DEATH_TEST registers during the static initialization of other files.
			static DeathTestRegistry& GetDeathTestRegistry()
			{
				static DeathTestRegistry s_Registry;
				return s_Registry;
			}

			// Every registration of `name`, in registration order: one for a valid name, several for a duplicate.
			static std::vector<DeathTestRegistration> FindRegistrations(std::string_view name)
			{
				DeathTestRegistry& registry = GetDeathTestRegistry();
				std::scoped_lock lock(registry.Mutex);
				std::vector<DeathTestRegistration> found;
				for (const DeathTestRegistration& registration : registry.Registrations)
				{
					if (registration.Name == name)
						found.push_back(registration);
				}
				return found;
			}

			// "Core/Assert.cpp:12 and Core/Other.cpp:40".
			static std::string DescribeLocations(const std::vector<DeathTestRegistration>& registrations)
			{
				std::string text;
				for (size_t index = 0; index < registrations.size(); ++index)
				{
					if (index > 0)
						text += index + 1 == registrations.size() ? " and " : ", ";
					text += std::format("{}:{}", registrations[index].File, registrations[index].Line);
				}
				return text;
			}

		}

		bool RegisterDeathTest(std::string_view name, DeathTestBody body, const char* file, int line)
		{
			ENGINE_CORE_ASSERT(body != nullptr, "Death test '{}' has no body", name);
			DeathTestRegistry& registry = Utils::GetDeathTestRegistry();
			std::scoped_lock lock(registry.Mutex);
			registry.Registrations.push_back({ std::string(name), body, file != nullptr ? std::string(file) : std::string(), line });
			return true;
		}

		DeathTestBody FindDeathTest(std::string_view name)
		{
			const std::vector<DeathTestRegistration> registrations = Utils::FindRegistrations(name);
			return registrations.empty() ? nullptr : registrations.front().Body;
		}

		std::vector<std::string> GetDeathTestNames()
		{
			DeathTestRegistry& registry = Utils::GetDeathTestRegistry();
			std::vector<std::string> names;
			{
				std::scoped_lock lock(registry.Mutex);
				names.reserve(registry.Registrations.size());
				for (const DeathTestRegistration& registration : registry.Registrations)
					names.push_back(registration.Name);
			}
			std::ranges::sort(names);
			return names;
		}

		int RunDeathTestBody(std::string_view name)
		{
			const std::vector<DeathTestRegistration> registrations = Utils::FindRegistrations(name);
			if (registrations.empty())
			{
				ENGINE_CORE_ERROR("No death test is registered as '{}'", name);
				return DeathTestUsageErrorExitCode;
			}
			if (registrations.size() > 1)
			{
				ENGINE_CORE_ERROR("Death test '{}' is registered more than once, at {}", name, Utils::DescribeLocations(registrations));
				return DeathTestUsageErrorExitCode;
			}

			registrations.front().Body();
			ENGINE_CORE_ERROR("Death test '{}' returned without dying", name);
			return DeathTestFailedExitCode;
		}

		Result<DeathTestResult> RunDeathTest(std::string_view name, std::chrono::milliseconds timeout)
		{
			if (FindDeathTest(name) == nullptr)
				return MakeError(ErrorCode::NotFound, "no death test is registered as '{}'", name);

			const std::vector<std::string> arguments = { std::format("--death-test={}", name) };
			std::string context = std::format("while running death test '{}'", name);
			Result<ChildProcessResult> spawned = RunChildProcess(GetTestOptions().ExecutablePath, arguments, timeout);
			ENGINE_TRY_ASSIGN(ChildProcessResult child, WithContext(std::move(spawned), std::move(context)));

			DeathTestResult result;
			result.ExitCode = child.ExitCode;
			result.StandardOutput = std::move(child.StandardOutput);
			result.StandardError = std::move(child.StandardError);
			return result;
		}

		std::string DescribeDeathMismatch(std::string_view name, std::string_view expectedSubstring)
		{
			const std::vector<DeathTestRegistration> registrations = Utils::FindRegistrations(name);
			if (registrations.size() > 1)
				return std::format("Death test '{}' is registered more than once, at {}", name, Utils::DescribeLocations(registrations));

			const Result<DeathTestResult> result = RunDeathTest(name);
			if (!result.has_value())
				return std::format("Death test '{}' did not run: {}", name, result.error());

			std::string mismatch;
			if (result->ExitCode != FatalCrashExitCode)
				mismatch = std::format("Death test '{}' exited with code {} instead of {}", name, result->ExitCode, FatalCrashExitCode);
			if (!result->StandardError.contains(expectedSubstring))
			{
				if (!mismatch.empty())
					mismatch += "; ";
				mismatch += std::format("the standard error of death test '{}' does not contain \"{}\"", name, expectedSubstring);
			}
			if (!mismatch.empty())
				mismatch += std::format("; its standard error was:\n{}", result->StandardError);
			return mismatch;
		}

	}

}
