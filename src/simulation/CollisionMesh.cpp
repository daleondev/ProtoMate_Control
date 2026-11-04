#include "CollisionMesh.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace simulation
{
    namespace
    {
        constexpr double EPS{ 1e-8 };
        bool finite(glm::dvec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
        std::uint32_t word(std::span<const std::byte> b, std::size_t at)
        {
            std::uint32_t v{};
            for (unsigned i{}; i < 4; ++i)
                v |= std::uint32_t(std::to_integer<unsigned char>(b[at + i])) << (8 * i);
            return v;
        }
    }
    CollisionMesh::CollisionMesh(std::span<const std::byte> bytes)
    {
        if (bytes.size() < 84)
            throw std::invalid_argument{ "truncated collision STL" };
        const auto count{ word(bytes, 80) };
        if (!count || count > 1000000 || bytes.size() != 84 + std::size_t(count) * 50)
            throw std::invalid_argument{ "invalid collision STL length/count" };
        m_triangles.reserve(count);
        for (std::size_t i{}; i < count; ++i) {
            const auto point{ [&](std::size_t v) {
                const auto offset{ 84 + i * 50 + 12 + v * 12 };
                return glm::dvec3{ std::bit_cast<float>(word(bytes, offset)),
                                   std::bit_cast<float>(word(bytes, offset + 4)),
                                   std::bit_cast<float>(word(bytes, offset + 8)) } *
                       1000.0;
            } };
            m_triangles.push_back({ point(0), point(1), point(2) });
        }
        initialize();
    }
    CollisionMesh::CollisionMesh(std::vector<Triangle> triangles)
      : m_triangles{ std::move(triangles) }
    {
        initialize();
    }
    void CollisionMesh::initialize()
    {
        if (m_triangles.empty() || m_triangles.size() > 1000000)
            throw std::invalid_argument{ "invalid collision mesh capacity" };
        for (const auto& t : m_triangles) {
            for (auto p : { t.a, t.b, t.c })
                if (!finite(p) || glm::any(glm::greaterThan(glm::abs(p), glm::dvec3{ 10000 })))
                    throw std::invalid_argument{ "invalid collision mesh coordinates" };
            const auto n{ glm::cross(t.b - t.a, t.c - t.a) };
            if (glm::dot(n, n) < 1e-20)
                throw std::invalid_argument{ "degenerate collision triangle" };
        }
        // Containment requires closed solids. Reject holes/non-manifold topology
        // rather than turning an arbitrary open surface into a switch actuator.
        struct Edge
        {
            std::array<double, 6> vertices{};
            std::size_t triangle{};
        };
        std::vector<Edge> edges{};
        std::vector<std::size_t> parents{};
        parents.reserve(m_triangles.size());
        for (std::size_t i{}; i < m_triangles.size(); ++i)
            parents.push_back(i);
        const auto root{ [&](std::size_t i) {
            while (parents[i] != i) {
                parents[i] = parents[parents[i]];
                i = parents[i];
            }
            return i;
        } };
        std::size_t triangle{};
        edges.reserve(m_triangles.size() * 3);
        for (const auto& t : m_triangles) {
            const std::array vertices{ t.a, t.b, t.c };
            for (unsigned i{}; i < 3; ++i) {
                const auto a{ vertices[i] }, b{ vertices[(i + 1) % 3] };
                std::array<double, 3> first{ a.x, a.y, a.z }, second{ b.x, b.y, b.z };
                if (first > second)
                    std::swap(first, second);
                edges.push_back(
                  { { first[0], first[1], first[2], second[0], second[1], second[2] }, triangle });
            }
            ++triangle;
        }
        std::sort(
          edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.vertices < b.vertices; });
        for (std::size_t i{}; i < edges.size();) {
            std::size_t end{ i + 1 };
            while (end < edges.size() && edges[end].vertices == edges[i].vertices)
                ++end;
            if (end - i != 2)
                throw std::invalid_argument{ "collision solid must be closed and manifold" };
            parents[root(edges[i].triangle)] = root(edges[i + 1].triangle);
            i = end;
        }
        for (std::size_t i{}; i < parents.size(); ++i)
            if (root(i) == i)
                m_components.push_back(m_triangles[i].a);
        m_nodes.reserve(m_triangles.size() * 2);
        build(0, m_triangles.size());
    }
    CollisionMesh::Bounds CollisionMesh::bounds(const Triangle& t)
    {
        return { glm::min(t.a, glm::min(t.b, t.c)), glm::max(t.a, glm::max(t.b, t.c)) };
    }
    bool CollisionMesh::overlap(const Bounds& a, const Bounds& b)
    {
        return glm::all(glm::lessThanEqual(a.lo, b.hi + EPS)) &&
               glm::all(glm::lessThanEqual(b.lo, a.hi + EPS));
    }
    std::uint32_t CollisionMesh::build(std::size_t begin, std::size_t end)
    {
        auto box{ bounds(m_triangles[begin]) };
        for (std::size_t i{ begin + 1 }; i < end; ++i) {
            const auto b{ bounds(m_triangles[i]) };
            box.lo = glm::min(box.lo, b.lo);
            box.hi = glm::max(box.hi, b.hi);
        }
        const auto index{ static_cast<std::uint32_t>(m_nodes.size()) };
        m_nodes.push_back(
          { box, static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end - begin) });
        if (end - begin > 8) {
            const auto extent{ box.hi - box.lo };
            const unsigned axis{ extent.x > extent.y ? (extent.x > extent.z ? 0U : 2U)
                                                     : (extent.y > extent.z ? 1U : 2U) };
            const auto middle{ begin + (end - begin) / 2 };
            std::nth_element(m_triangles.begin() + begin,
                             m_triangles.begin() + middle,
                             m_triangles.begin() + end,
                             [axis](const Triangle& a, const Triangle& b) {
                return a.a[axis] + a.b[axis] + a.c[axis] < b.a[axis] + b.b[axis] + b.c[axis];
            });
            const auto left{ build(begin, middle) }, right{ build(middle, end) };
            m_nodes[index].left = left;
            m_nodes[index].right = right;
            m_nodes[index].count = 0;
        }
        return index;
    }
    bool CollisionMesh::trianglesOverlap(const Triangle& a, const Triangle& b)
    {
        const std::array ae{ a.b - a.a, a.c - a.b, a.a - a.c }, be{ b.b - b.a, b.c - b.b, b.a - b.c };
        const auto an{ glm::cross(ae[0], ae[1]) }, bn{ glm::cross(be[0], be[1]) };
        const auto separates{ [&](glm::dvec3 axis) {
            const double length{ glm::length(axis) };
            if (length < 1e-14)
                return false;
            axis /= length;
            const std::array x{ glm::dot(a.a, axis), glm::dot(a.b, axis), glm::dot(a.c, axis) };
            const std::array y{ glm::dot(b.a, axis), glm::dot(b.b, axis), glm::dot(b.c, axis) };
            const auto [xmin, xmax]{ std::minmax_element(x.begin(), x.end()) };
            const auto [ymin, ymax]{ std::minmax_element(y.begin(), y.end()) };
            return *xmax + EPS < *ymin || *ymax + EPS < *xmin;
        } };
        if (separates(an) || separates(bn))
            return false;
        for (auto e : ae) {
            if (separates(glm::cross(an, e)))
                return false; // coplanar triangles
            for (auto f : be)
                if (separates(glm::cross(e, f)))
                    return false;
        }
        for (auto e : be)
            if (separates(glm::cross(bn, e)))
                return false;
        return true;
    }
    void CollisionMesh::ray(std::uint32_t index,
                            glm::dvec3 o,
                            glm::dvec3 d,
                            unsigned& crossings,
                            bool& ambiguous) const
    {
        const auto& node{ m_nodes[index] };
        double near{ 0 }, far{ std::numeric_limits<double>::infinity() };
        for (unsigned axis{}; axis < 3; ++axis) {
            double a{ (node.bounds.lo[axis] - o[axis]) / d[axis] },
              b{ (node.bounds.hi[axis] - o[axis]) / d[axis] };
            if (a > b)
                std::swap(a, b);
            near = std::max(near, a);
            far = std::min(far, b);
            if (near > far + EPS)
                return;
        }
        if (!node.count) {
            ray(node.left, o, d, crossings, ambiguous);
            ray(node.right, o, d, crossings, ambiguous);
            return;
        }
        for (std::size_t i{ node.begin }; i < node.begin + node.count; ++i) {
            const auto& t{ m_triangles[i] };
            const auto e1{ t.b - t.a }, e2{ t.c - t.a }, p{ glm::cross(d, e2) };
            const double determinant{ glm::dot(e1, p) };
            if (std::abs(determinant) < 1e-14)
                continue;
            const auto s{ o - t.a }, q{ glm::cross(s, e1) };
            const double u{ glm::dot(s, p) / determinant }, v{ glm::dot(d, q) / determinant },
              distance{ glm::dot(e2, q) / determinant };
            if (u < -EPS || v < -EPS || u + v > 1 + EPS || distance < EPS)
                continue;
            if (std::min({ std::abs(u), std::abs(v), std::abs(1 - u - v) }) < EPS)
                ambiguous = true;
            ++crossings;
        }
    }
    bool CollisionMesh::contains(glm::dvec3 p) const
    {
        if (!finite(p))
            throw std::invalid_argument{ "invalid collision point" };
        if (!overlap(m_nodes.front().bounds, { p, p }))
            return false;
        for (auto direction : { glm::dvec3{ 1, .371390676, .5291283 },
                                glm::dvec3{ -.313719, 1, .71931 },
                                glm::dvec3{ .719931, -.432711, 1 } }) {
            unsigned crossings{};
            bool ambiguous{};
            ray(0, p, direction, crossings, ambiguous);
            if (!ambiguous)
                return (crossings % 2) != 0;
        }
        throw std::runtime_error{ "ambiguous collision containment" };
    }
    double CollisionMesh::triangleDistanceSquared(const Triangle& a, const Triangle& b)
    {
        const auto point{ [](glm::dvec3 p, const Triangle& t) {
            const auto ab{ t.b - t.a }, ac{ t.c - t.a }, ap{ p - t.a };
            const double d1{ glm::dot(ab, ap) }, d2{ glm::dot(ac, ap) };
            if (d1 <= 0 && d2 <= 0)
                return glm::dot(ap, ap);
            const auto bp{ p - t.b };
            const double d3{ glm::dot(ab, bp) }, d4{ glm::dot(ac, bp) };
            if (d3 >= 0 && d4 <= d3)
                return glm::dot(bp, bp);
            const double vc{ d1 * d4 - d3 * d2 };
            if (vc <= 0 && d1 >= 0 && d3 <= 0) {
                const auto delta{ ap - ab * (d1 / (d1 - d3)) };
                return glm::dot(delta, delta);
            }
            const auto cp{ p - t.c };
            const double d5{ glm::dot(ab, cp) }, d6{ glm::dot(ac, cp) };
            if (d6 >= 0 && d5 <= d6)
                return glm::dot(cp, cp);
            const double vb{ d5 * d2 - d1 * d6 };
            if (vb <= 0 && d2 >= 0 && d6 <= 0) {
                const auto delta{ ap - ac * (d2 / (d2 - d6)) };
                return glm::dot(delta, delta);
            }
            const double va{ d3 * d6 - d5 * d4 };
            if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
                const auto delta{ bp - (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))) };
                return glm::dot(delta, delta);
            }
            const double denominator{ 1 / (va + vb + vc) };
            const auto delta{ ap - ab * (vb * denominator) - ac * (vc * denominator) };
            return glm::dot(delta, delta);
        } };
        const auto segment{ [](glm::dvec3 p, glm::dvec3 p2, glm::dvec3 q, glm::dvec3 q2) {
            const auto d1{ p2 - p }, d2{ q2 - q }, r{ p - q };
            const double a{ glm::dot(d1, d1) }, e{ glm::dot(d2, d2) }, f{ glm::dot(d2, r) },
              c{ glm::dot(d1, r) }, b{ glm::dot(d1, d2) };
            const double denominator{ a * e - b * b };
            double s{ denominator > 1e-30 ? std::clamp((b * f - c * e) / denominator, 0.0, 1.0) : 0.0 };
            double t{ (b * s + f) / e };
            if (t < 0) {
                t = 0;
                s = std::clamp(-c / a, 0.0, 1.0);
            }
            else if (t > 1) {
                t = 1;
                s = std::clamp((b - c) / a, 0.0, 1.0);
            }
            const auto delta{ r + d1 * s - d2 * t };
            return glm::dot(delta, delta);
        } };
        if (trianglesOverlap(a, b))
            return 0;
        double best{ std::min(
          { point(a.a, b), point(a.b, b), point(a.c, b), point(b.a, a), point(b.b, a), point(b.c, a) }) };
        const std::array av{ a.a, a.b, a.c }, bv{ b.a, b.b, b.c };
        for (unsigned i{}; i < 3; ++i)
            for (unsigned j{}; j < 3; ++j)
                best = std::min(best, segment(av[i], av[(i + 1) % 3], bv[j], bv[(j + 1) % 3]));
        return best;
    }
    void CollisionMesh::distance(std::uint32_t index,
                                 const Triangle& triangle,
                                 const Bounds& box,
                                 double& best) const
    {
        const auto lower{ [&](const Bounds& b) {
            const auto delta{ glm::max(glm::max(box.lo - b.hi, b.lo - box.hi), glm::dvec3{ 0 }) };
            return glm::dot(delta, delta);
        } };
        const auto& node{ m_nodes[index] };
        if (lower(node.bounds) > best)
            return;
        if (!node.count) {
            auto first{ node.left }, second{ node.right };
            if (lower(m_nodes[first].bounds) > lower(m_nodes[second].bounds))
                std::swap(first, second);
            distance(first, triangle, box, best);
            if (best > 0)
                distance(second, triangle, box, best);
            return;
        }
        for (std::size_t i{ node.begin }; i < node.begin + node.count && best > 0; ++i)
            if (lower(bounds(m_triangles[i])) <= best)
                best = std::min(best, triangleDistanceSquared(m_triangles[i], triangle));
    }
    double CollisionMesh::distance(const CollisionMesh& other, const glm::dmat4& transform) const
    {
        for (unsigned c{}; c < 4; ++c)
            for (unsigned r{}; r < 4; ++r)
                if (!std::isfinite(transform[c][r]))
                    throw std::invalid_argument{ "invalid collision transform" };
        if (transform[0][3] != 0 || transform[1][3] != 0 || transform[2][3] != 0 || transform[3][3] != 1 ||
            std::abs(glm::determinant(glm::dmat3{ transform })) < 1e-12)
            throw std::invalid_argument{ "collision transform must be affine and invertible" };
        double best{ std::numeric_limits<double>::infinity() };
        for (const auto& t : other.m_triangles) {
            const Triangle moved{ glm::dvec3{ transform * glm::dvec4{ t.a, 1 } },
                                  glm::dvec3{ transform * glm::dvec4{ t.b, 1 } },
                                  glm::dvec3{ transform * glm::dvec4{ t.c, 1 } } };
            distance(0, moved, bounds(moved), best);
            if (best <= EPS * EPS)
                return 0;
        }
        for (const auto sample : other.m_components)
            if (contains(glm::dvec3{ transform * glm::dvec4{ sample, 1 } }))
                return 0;
        const auto inverse{ glm::inverse(transform) };
        for (const auto sample : m_components)
            if (other.contains(glm::dvec3{ inverse * glm::dvec4{ sample, 1 } }))
                return 0;
        return std::sqrt(best);
    }
    bool CollisionMesh::intersects(const CollisionMesh& other, const glm::dmat4& transform) const
    {
        return distance(other, transform) == 0;
    }
    double CollisionMesh::radiusFromZAxis(double x, double y, const glm::dmat4& transform) const noexcept
    {
        double radius{};
        for (const auto& t : m_triangles)
            for (auto p : { t.a, t.b, t.c }) {
                const auto moved{ transform * glm::dvec4{ p, 1 } };
                radius = std::max(radius, std::hypot(moved.x - x, moved.y - y));
            }
        return radius;
    }
}
