#include "EnginePCH.h"
#include "Engine/Physics/Private/PhysicsEdgeRemoval.h"

#include <Jolt/Core/Array.h>
#include <Jolt/Core/STLLocalAllocator.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <numeric>
#include <utility>

namespace Engine {

	namespace {

		// The local capacities before the arrays reach the heap (Jolt's own collector uses the same).
		constexpr size_t LocalDelayedResults = 32;
		constexpr size_t LocalVoidedFeatures = 128;

		// A face vertex already accounted for by a kept contact, per sub-shape of the first body (a compound's parts are
		// tested separately).
		struct VoidedFeature
		{
			JPH::Float3 Position{};
			JPH::SubShapeID SubShape{};
		};

		// Collects the hits of one body pair's collision, keeps face contacts at once and decides on the others in Flush
		// (PhysicsEdgeRemoval.h), forwarding what it keeps to the chained collector.
		class EdgeRemovingCollector final : public JPH::CollideShapeCollector
		{
		public:
			EdgeRemovingCollector(JPH::CollideShapeCollector& chained, float vertexToleranceSq)
				: JPH::CollideShapeCollector(chained), m_Chained(chained), m_VertexToleranceSq(vertexToleranceSq)
			{
				m_Voided.reserve(LocalVoidedFeatures);
				m_Delayed.reserve(LocalDelayedResults);
			}

			EdgeRemovingCollector(const EdgeRemovingCollector&) = delete;
			EdgeRemovingCollector& operator=(const EdgeRemovingCollector&) = delete;

			void Reset() override
			{
				JPH::CollideShapeCollector::Reset();
				m_Chained.Reset();
				m_Voided.clear();
				m_Delayed.clear();
			}

			void OnBody(const JPH::Body& body) override
			{
				m_Chained.OnBody(body);
			}

			void AddHit(const JPH::CollideShapeResult& result) override
			{
				// A face needs three vertices for a normal; anything else is kept as it is.
				if (result.mShape2Face.size() < 3)
				{
					KeepAndVoid(result);
					return;
				}
				const JPH::Vec3 faceNormal = (result.mShape2Face[1] - result.mShape2Face[0]).Cross(result.mShape2Face[2] - result.mShape2Face[0]);
				const float faceLength = faceNormal.Length();
				if (faceLength < 1.0e-6f)
				{
					KeepAndVoid(result);
					return;
				}
				const JPH::Vec3 contactNormal = -result.mPenetrationAxis;
				const float contactLength = result.mPenetrationAxis.Length();
				if (faceNormal.Dot(contactNormal) > Detail::FaceContactCosine * contactLength * faceLength)
				{
					KeepAndVoid(result);
					return;
				}
				m_Delayed.push_back(result);
			}

			void OnBodyEnd() override
			{
				Flush();
				m_Chained.OnBodyEnd();
			}

			// Decides on the delayed contacts, deepest first: a contact whose closest vertex or edge of its face is voided
			// touches an internal feature and is dropped; every contact voids its face's vertices.
			void Flush()
			{
				JPH::Array<uint32_t, JPH::STLLocalAllocator<uint32_t, LocalDelayedResults>> order;
				order.resize(m_Delayed.size());
				std::iota(order.begin(), order.end(), 0u);
				// A total order (equal depths by arrival), so the result is the same with every standard library.
				std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b)
				{
					const float depthA = m_Delayed[a].mPenetrationDepth;
					const float depthB = m_Delayed[b].mPenetrationDepth;
					return depthA != depthB ? depthA > depthB : a < b;
				});

				for (const uint32_t index : order)
				{
					const JPH::CollideShapeResult& result = m_Delayed[index];
					const auto [first, second] = FindClosestFeature(result);
					const bool voided = IsVoided(result.mSubShapeID1, result.mShape2Face[first])
						&& (first == second || IsVoided(result.mSubShapeID1, result.mShape2Face[second]));
					if (!voided)
						Keep(result);
					VoidFeatures(result);
				}
				m_Voided.clear();
				m_Delayed.clear();
			}
		private:
			// The vertex (first == second) or edge (first, second) of the result's face closest to its contact point.
			[[nodiscard]] static std::pair<uint32_t, uint32_t> FindClosestFeature(const JPH::CollideShapeResult& result)
			{
				float bestDistanceSq = FLT_MAX;
				uint32_t bestFirst = 0;
				uint32_t bestSecond = 0;
				const auto count = static_cast<uint32_t>(result.mShape2Face.size());
				uint32_t previous = count - 1;
				JPH::Vec3 fromPrevious = result.mShape2Face[previous] - result.mContactPointOn2;
				for (uint32_t current = 0; current < count; ++current)
				{
					const JPH::Vec3 fromCurrent = result.mShape2Face[current] - result.mContactPointOn2;
					const JPH::Vec3 edge = fromCurrent - fromPrevious;
					const float edgeLengthSq = edge.LengthSq();
					// The edge's closest point to the contact as a fraction along it: at the previous vertex, inside, or at the
					// current vertex (which the next iteration tests as its previous one). A degenerate edge is its vertex.
					const float fraction = edgeLengthSq < FLT_EPSILON * FLT_EPSILON ? 0.0f : -fromPrevious.Dot(edge) / edgeLengthSq;
					if (fraction < 1.0e-6f)
					{
						const float distanceSq = fromPrevious.LengthSq();
						if (distanceSq < bestDistanceSq)
						{
							bestDistanceSq = distanceSq;
							bestFirst = previous;
							bestSecond = previous;
						}
					}
					else if (fraction < 1.0f - 1.0e-6f)
					{
						const float distanceSq = (fromPrevious + fraction * edge).LengthSq();
						if (distanceSq < bestDistanceSq)
						{
							bestDistanceSq = distanceSq;
							bestFirst = previous;
							bestSecond = current;
						}
					}
					previous = current;
					fromPrevious = fromCurrent;
				}
				return { bestFirst, bestSecond };
			}

			[[nodiscard]] bool IsVoided(const JPH::SubShapeID& subShape, JPH::Vec3Arg vertex) const
			{
				return std::any_of(m_Voided.begin(), m_Voided.end(), [this, &subShape, vertex](const VoidedFeature& feature)
				{
					return feature.SubShape == subShape && vertex.IsClose(JPH::Vec3(feature.Position), m_VertexToleranceSq);
				});
			}

			void VoidFeatures(const JPH::CollideShapeResult& result)
			{
				for (const JPH::Vec3& vertex : result.mShape2Face)
				{
					if (IsVoided(result.mSubShapeID1, vertex))
						continue;
					VoidedFeature feature;
					vertex.StoreFloat3(&feature.Position);
					feature.SubShape = result.mSubShapeID1;
					m_Voided.push_back(feature);
				}
			}

			void Keep(const JPH::CollideShapeResult& result)
			{
				m_Chained.SetContext(GetContext());
				m_Chained.AddHit(result);
				UpdateEarlyOutFraction(m_Chained.GetEarlyOutFraction());
			}

			void KeepAndVoid(const JPH::CollideShapeResult& result)
			{
				Keep(result);
				VoidFeatures(result);
			}
		private:
			JPH::CollideShapeCollector& m_Chained; // not owned: Jolt's collector for the pair, outliving this one
			float m_VertexToleranceSq = 0.0f;
			JPH::Array<VoidedFeature, JPH::STLLocalAllocator<VoidedFeature, LocalVoidedFeatures>> m_Voided;
			JPH::Array<JPH::CollideShapeResult, JPH::STLLocalAllocator<JPH::CollideShapeResult, LocalDelayedResults>> m_Delayed;
		};

	}

	namespace Detail {

		void CollideBodiesWithEdgeRemoval(const JPH::Body& body1, const JPH::Body& body2, JPH::Mat44Arg centerOfMassTransform1,
			JPH::Mat44Arg centerOfMassTransform2, JPH::CollideShapeSettings& settings, JPH::CollideShapeCollector& collector,
			const JPH::ShapeFilter& shapeFilter)
		{
			if (!body1.GetEnhancedInternalEdgeRemovalWithBody(body2))
			{
				JPH::PhysicsSystem::sDefaultSimCollideBodyVsBody(body1, body2, centerOfMassTransform1, centerOfMassTransform2, settings, collector, shapeFilter);
				return;
			}

			// The algorithm needs every edge and the touched faces (Jolt's collector has the same requirements).
			settings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
			settings.mCollectFacesMode = JPH::ECollectFacesMode::CollectFaces;
			const JPH::SubShapeIDCreator part1;
			const JPH::SubShapeIDCreator part2;
			EdgeRemovingCollector edgeRemoving(collector, settings.mInternalEdgeRemovalVertexToleranceSq);
			JPH::CollisionDispatch::sCollideShapeVsShape(body1.GetShape(), body2.GetShape(), JPH::Vec3::sOne(), JPH::Vec3::sOne(), centerOfMassTransform1,
				centerOfMassTransform2, part1, part2, settings, edgeRemoving, shapeFilter);
			edgeRemoving.Flush();
		}

	}

}
