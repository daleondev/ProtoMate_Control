#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <vector>

namespace simulation
{
    // Immutable triangle BVH. Construction allocates; collision queries do not.
    // STL coordinates are metres/Z-up; mechanics and queries use millimetres.
    class CollisionMesh
    {
      public:
        struct Triangle
        {
            glm::dvec3 a{}, b{}, c{};
        };
        explicit CollisionMesh(std::span<const std::byte> bytes);
        explicit CollisionMesh(std::vector<Triangle> triangles);
        bool intersects(const CollisionMesh& other, const glm::dmat4& transform) const;
        bool contains(glm::dvec3 point) const;
        double distance(const CollisionMesh& other, const glm::dmat4& transform) const;
        double radiusFromZAxis(double x, double y, const glm::dmat4& transform) const noexcept;
        std::size_t triangles() const noexcept { return m_triangles.size(); }

      private:
        struct Bounds
        {
            glm::dvec3 lo{}, hi{};
        };
        struct Node
        {
            Bounds bounds{};
            std::uint32_t begin{}, count{}, left{}, right{};
        };
        static Bounds bounds(const Triangle& /*t*/);
        static bool overlap(const Bounds& /*a*/, const Bounds& /*b*/);
        static double triangleDistanceSquared(const Triangle& /*a*/, const Triangle& /*b*/);
        void distance(std::uint32_t /*index*/,
                      const Triangle& /*triangle*/,
                      const Bounds& /*box*/,
                      double& best) const;
        static bool trianglesOverlap(const Triangle& /*a*/, const Triangle& /*b*/);
        std::uint32_t build(std::size_t begin, std::size_t end);
        void ray(std::uint32_t index,
                 glm::dvec3 origin,
                 glm::dvec3 direction,
                 unsigned& crossings,
                 bool& ambiguous) const;
        void initialize();
        void initializeComponents();
        struct EdgeSegment
        {
            glm::dvec3 begin{}, end{};
        };
        static double pointDistanceSquared(glm::dvec3 point, const Triangle& triangle);
        static double segmentDistanceSquared(const EdgeSegment& first, const EdgeSegment& second);
        std::vector<Triangle> m_triangles{};
        std::vector<Node> m_nodes{};
        std::vector<glm::dvec3> m_components{};
    };
}
