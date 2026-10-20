#include "EnginePCH.h"
#include "Engine/Graphics/GpuRecreation.h"

#include "Engine/Core/Assert.h"

#include <utility>

namespace Engine {

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

	GpuRecreationRegistration GpuRecreationRegistry::Register(GpuRecreationHooks /*hooks*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void GpuRecreationRegistry::ReleaseAll()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status GpuRecreationRegistry::RecreateAll()
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 recreation protocol contract (Docs/Decisions/0029-m18-contract.md decision 26, stream A)"));
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

	void GpuRecreationRegistry::Unregister(uint64_t /*id*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
