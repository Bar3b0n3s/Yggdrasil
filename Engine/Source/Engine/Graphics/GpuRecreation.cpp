#include "EnginePCH.h"
#include "Engine/Graphics/GpuRecreation.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		// Sets the registry's running flag for its lifetime, so the registry stops running however its loop ends: a
		// vk::SystemError thrown out of an owner's NVRHI call in a hook unwinds through it (which then fails the recovery).
		class RunningScope
		{
		public:
			explicit RunningScope(bool& running)
				: m_Running(&running)
			{
				*m_Running = true;
			}

			~RunningScope()
			{
				*m_Running = false;
			}

			RunningScope(const RunningScope&) = delete;
			RunningScope& operator=(const RunningScope&) = delete;
		private:
			bool* m_Running = nullptr; // the registry's m_Running, which outlives the scope
		};

	}

	GpuRecreationRegistration::GpuRecreationRegistration(GpuRecreationRegistry& registry, uint64_t id)
		: m_Registry(&registry), m_Id(id)
	{
	}

	GpuRecreationRegistration::~GpuRecreationRegistration()
	{
		Reset();
	}

	GpuRecreationRegistration::GpuRecreationRegistration(GpuRecreationRegistration&& other) noexcept
		: m_Registry(std::exchange(other.m_Registry, nullptr)), m_Id(std::exchange(other.m_Id, 0))
	{
	}

	GpuRecreationRegistration& GpuRecreationRegistration::operator=(GpuRecreationRegistration&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_Registry = std::exchange(other.m_Registry, nullptr);
			m_Id = std::exchange(other.m_Id, 0);
		}
		return *this;
	}

	void GpuRecreationRegistration::Reset()
	{
		if (m_Registry != nullptr)
			m_Registry->Unregister(m_Id);
		m_Registry = nullptr;
		m_Id = 0;
	}

	GpuRecreationRegistry::GpuRecreationRegistry() = default;

	GpuRecreationRegistry::~GpuRecreationRegistry()
	{
		ENGINE_CORE_ASSERT(m_Owners.empty(), "GpuRecreationRegistry destroyed with {} owners still registered", m_Owners.size());
	}

	GpuRecreationRegistration GpuRecreationRegistry::Register(GpuRecreationHooks hooks)
	{
		ENGINE_CORE_ASSERT(!m_Running, "GpuRecreationRegistry::Register of '{}' while the owners are released, recreated or trimmed", hooks.Name);
		ENGINE_CORE_ASSERT(!hooks.Name.empty(), "GpuRecreationRegistry::Register needs the owner's name");
		ENGINE_CORE_ASSERT(hooks.Release != nullptr, "GpuRecreationRegistry::Register of '{}' needs a Release hook", hooks.Name);
		const uint64_t id = ++m_LastOwnerId;
		m_Owners.push_back(Owner{ .Id = id, .Hooks = std::move(hooks) });
		return GpuRecreationRegistration(*this, id);
	}

	void GpuRecreationRegistry::ReleaseAll()
	{
		ENGINE_CORE_ASSERT(!m_Running, "GpuRecreationRegistry::ReleaseAll while the owners are released or recreated");
		// No hook registers or unregisters (asserted there), so the owners stay where they are while the loop runs.
		const Utils::RunningScope running(m_Running);
		for (auto owner = m_Owners.rbegin(); owner != m_Owners.rend(); ++owner)
			owner->Hooks.Release();
	}

	Status GpuRecreationRegistry::RecreateAll()
	{
		ENGINE_CORE_ASSERT(!m_Running, "GpuRecreationRegistry::RecreateAll while the owners are released or recreated");
		const Utils::RunningScope running(m_Running);
		for (const Owner& owner : m_Owners)
		{
			if (!owner.Hooks.Recreate)
				continue;
			Status recreated = owner.Hooks.Recreate();
			if (!recreated.has_value())
				return WithContext(std::move(recreated), std::format("while recreating '{}'", owner.Hooks.Name));
		}
		return {};
	}

	size_t GpuRecreationRegistry::TrimUnused()
	{
		ENGINE_CORE_ASSERT(!m_Running, "GpuRecreationRegistry::TrimUnused while the owners are released, recreated or trimmed");
		// No hook registers or unregisters (asserted there), so the owners stay where they are while the loop runs.
		const Utils::RunningScope running(m_Running);
		size_t trimmed = 0;
		for (auto owner = m_Owners.rbegin(); owner != m_Owners.rend(); ++owner)
		{
			if (!owner->Hooks.TrimUnused)
				continue;
			owner->Hooks.TrimUnused();
			++trimmed;
		}
		return trimmed;
	}

	std::vector<std::string> GpuRecreationRegistry::GetOwnerNames() const
	{
		std::vector<std::string> names;
		names.reserve(m_Owners.size());
		for (const Owner& owner : m_Owners)
			names.push_back(owner.Hooks.Name);
		return names;
	}

	size_t GpuRecreationRegistry::GetOwnerCount() const
	{
		return m_Owners.size();
	}

	void GpuRecreationRegistry::Unregister(uint64_t id)
	{
		ENGINE_CORE_ASSERT(!m_Running, "GpuRecreationRegistry: an owner unregistered while the owners are released, recreated or trimmed");
		const auto owner = std::ranges::find(m_Owners, id, &Owner::Id);
		ENGINE_CORE_ASSERT(owner != m_Owners.end(), "GpuRecreationRegistry: owner {} is not registered", id);
		if (owner != m_Owners.end())
			m_Owners.erase(owner);
	}

}
