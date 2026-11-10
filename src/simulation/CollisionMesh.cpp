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
        constexpr std::size_t STL_HEADER_BYTES{ 80 }, STL_PREFIX_BYTES{ 84 }, STL_TRIANGLE_BYTES{ 50 },
          VECTOR_BYTES{ 12 }, FLOAT_BYTES{ 4 }, MAX_TRIANGLES{ 1'000'000 }, LEAF_TRIANGLES{ 8 };
        constexpr unsigned BYTE_BITS{ 8 };
        constexpr double MM_PER_METRE{ 1000 }, MAX_COORDINATE_MM{ 10000 }, MIN_NORMAL_SQUARED{ 1e-20 },
          PARALLEL_EPSILON{ 1e-14 }, SEGMENT_DETERMINANT_EPSILON{ 1e-30 },
          TRANSFORM_DETERMINANT_EPSILON{ 1e-12 };
        constexpr std::array CONTAINMENT_RAYS{ glm::dvec3{ 1, .371390676, .5291283 },
                                               glm::dvec3{ -.313719, 1, .71931 },
                                               glm::dvec3{ .719931, -.432711, 1 } };
        constexpr double EPS{ 1e-8 };
        bool finite(glm::dvec3 p)
        {
            return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        }
        std::uint32_t word(std::span<const std::byte> b, std::size_t at)
        {
            std::uint32_t v{};
            for (unsigned i{}; i < sizeof(v); ++i) {
                v |= std::uint32_t(std::to_integer<unsigned char>(b[at + i])) << (BYTE_BITS * i);
            }
            return v;
        }
    }
    CollisionMesh::CollisionMesh(std::span<const std::byte> bytes)
    {
        if (bytes.size() < STL_PREFIX_BYTES) {
            throw std::invalid_argument{ "truncated collision STL" };
        }
        const auto count{ word(bytes, STL_HEADER_BYTES) };
        if (!count || count > MAX_TRIANGLES ||
            bytes.size() != STL_PREFIX_BYTES + (std::size_t(count) * STL_TRIANGLE_BYTES)) {
            throw std::invalid_argument{ "invalid collision STL length/count" };
        }
        m_triangles.reserve(count);
        for (std::size_t i{}; i < count; ++i) {
            const auto point{ [&](std::size_t v) {
                const auto offset{ STL_PREFIX_BYTES + (i * STL_TRIANGLE_BYTES) + VECTOR_BYTES +
                                   (v * VECTOR_BYTES) };
                return glm::dvec3{ std::bit_cast<float>(word(bytes, offset)),
                                   std::bit_cast<float>(word(bytes, offset + FLOAT_BYTES)),
                                   std::bit_cast<float>(word(bytes, offset + (2 * FLOAT_BYTES))) } *
                       MM_PER_METRE;
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
        if (m_triangles.empty() || m_triangles.size() > MAX_TRIANGLES) {
            throw std::invalid_argument{ "invalid collision mesh capacity" };
        }
        for (const auto& t : m_triangles) {
            for (auto p : { t.a, t.b, t.c }) {
                if (!finite(p) || glm::any(glm::greaterThan(glm::abs(p), glm::dvec3{ MAX_COORDINATE_MM }))) {
                    throw std::invalid_argument{ "invalid collision mesh coordinates" };
                }
            }
            const auto n{ glm::cross(t.b - t.a, t.c - t.a) };
            if (glm::dot(n, n) < MIN_NORMAL_SQUARED) {
                throw std::invalid_argument{ "degenerate collision triangle" };
            }
        }
        initializeComponents();
        m_nodes.reserve(m_triangles.size() * 2);
        build(0, m_triangles.size());
    }
    void CollisionMesh::initializeComponents()
    {
        // Containment requires closed solids. Reject holes/non-manifold topology
        // rather than turning an arbitrary open surface into a switch actuator.
        constexpr std::size_t edge_coordinates{ 2UZ * 3 };
        struct Edge
        {
            std::array<double, edge_coordinates> vertices{};
            std::size_t triangle{};
        };
        std::vector<Edge> edges{};
        std::vector<std::size_t> parents{};
        parents.reserve(m_triangles.size());
        for (std::size_t i{}; i < m_triangles.size(); ++i) {
            parents.push_back(i);
        }
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
                const auto a{ vertices[i] };
                const auto b{ vertices[(i + 1) % 3] };
                std::array<double, 3> first{ a[0], a[1], a[2] };
                std::array<double, 3> second{ b[0], b[1], b[2] };
                if (first > second) {
                    std::swap(first, second);
                }
                edges.push_back(
                  { { first[0], first[1], first[2], second[0], second[1], second[2] }, triangle });
            }
            ++triangle;
        }
        std::ranges::sort(edges, [](const Edge& a, const Edge& b) { return a.vertices < b.vertices; });
        for (std::size_t i{}; i < edges.size();) {
            std::size_t end{ i + 1 };
            while (end < edges.size() && edges[end].vertices == edges[i].vertices) {
                ++end;
            }
            if (end - i != 2) {
                throw std::invalid_argument{ "collision solid must be closed and manifold" };
            }
            parents[root(edges[i].triangle)] = root(edges[i + 1].triangle);
            i = end;
        }
        for (std::size_t i{}; i < parents.size(); ++i) {
            if (root(i) == i) {
                m_components.push_back(m_triangles[i].a);
            }
        }
    }
    CollisionMesh::Bounds CollisionMesh::bounds(const Triangle& t)
    {
        return { .lo = glm::min(t.a, glm::min(t.b, t.c)), .hi = glm::max(t.a, glm::max(t.b, t.c)) };
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
        if (end - begin > LEAF_TRIANGLES) {
            const auto extent{ box.hi - box.lo };
            const glm::length_t xy_axis{ extent[0] > extent[1] ? 0 : 1 };
            const glm::length_t axis{ extent[xy_axis] > extent[2] ? xy_axis : 2 };
            const auto middle{ begin + ((end - begin) / 2) };
            std::nth_element(m_triangles.begin() + static_cast<std::ptrdiff_t>(begin),
                             m_triangles.begin() + static_cast<std::ptrdiff_t>(middle),
                             m_triangles.begin() + static_cast<std::ptrdiff_t>(end),
                             [axis](const Triangle& a, const Triangle& b) {
                return a.a[axis] + a.b[axis] + a.c[axis] < b.a[axis] + b.b[axis] + b.c[axis];
            });
            const auto left{ build(begin, middle) };
            const auto right{ build(middle, end) };
            m_nodes[index].left = left;
            m_nodes[index].right = right;
            m_nodes[index].count = 0;
        }
        return index;
    }
    bool CollisionMesh::trianglesOverlap(const Triangle& a, const Triangle& b)
    {
        const std::array ae{ a.b - a.a, a.c - a.b, a.a - a.c };
        const std::array be{ b.b - b.a, b.c - b.b, b.a - b.c };
        const auto an{ glm::cross(ae[0], ae[1]) };
        const auto bn{ glm::cross(be[0], be[1]) };
        const auto separates{ [&](glm::dvec3 axis) {
            const double length{ glm::length(axis) };
            if (length < PARALLEL_EPSILON) {
                return false;
            }
            axis /= length;
            const std::array x{ glm::dot(a.a, axis), glm::dot(a.b, axis), glm::dot(a.c, axis) };
            const std::array y{ glm::dot(b.a, axis), glm::dot(b.b, axis), glm::dot(b.c, axis) };
            const auto [xmin, xmax]{ std::ranges::minmax_element(x) };
            const auto [ymin, ymax]{ std::ranges::minmax_element(y) };
            return *xmax + EPS < *ymin || *ymax + EPS < *xmin;
        } };
        if (separates(an) || separates(bn)) {
            return false;
        }
        for (auto e : ae) {
            if (separates(glm::cross(an, e))) {
                return false; // coplanar triangles
            }
            for (auto f : be) {
                if (separates(glm::cross(e, f))) {
                    return false;
                }
            }
        }
        return std::ranges::all_of(be, [&](auto edge) { return !separates(glm::cross(bn, edge)); });
    }
    void CollisionMesh::ray(std::uint32_t index,
                            glm::dvec3 o,
                            glm::dvec3 d,
                            unsigned& crossings,
                            bool& ambiguous) const
    {
        const auto& node{ m_nodes[index] };
        double near{ 0 };
        double far{ std::numeric_limits<double>::infinity() };
        for (glm::length_t axis{}; axis < 3; ++axis) {
            double a{ (node.bounds.lo[axis] - o[axis]) / d[axis] };
            double b{ (node.bounds.hi[axis] - o[axis]) / d[axis] };
            if (a > b) {
                std::swap(a, b);
            }
            near = std::max(near, a);
            far = std::min(far, b);
            if (near > far + EPS) {
                return;
            }
        }
        if (!node.count) {
            ray(node.left, o, d, crossings, ambiguous);
            ray(node.right, o, d, crossings, ambiguous);
            return;
        }
        for (std::size_t i{ node.begin }; i < node.begin + node.count; ++i) {
            const auto& t{ m_triangles[i] };
            const auto e1{ t.b - t.a };
            const auto e2{ t.c - t.a };
            const auto p{ glm::cross(d, e2) };
            const double determinant{ glm::dot(e1, p) };
            if (std::abs(determinant) < PARALLEL_EPSILON) {
                continue;
            }
            const auto s{ o - t.a };
            const auto q{ glm::cross(s, e1) };
            const double u{ glm::dot(s, p) / determinant };
            const double v{ glm::dot(d, q) / determinant };
            const double distance{ glm::dot(e2, q) / determinant };
            if (u < -EPS || v < -EPS || u + v > 1 + EPS || distance < EPS) {
                continue;
            }
            if (std::min({ std::abs(u), std::abs(v), std::abs(1 - u - v) }) < EPS) {
                ambiguous = true;
            }
            ++crossings;
        }
    }
    bool CollisionMesh::contains(glm::dvec3 p) const
    {
        if (!finite(p)) {
            throw std::invalid_argument{ "invalid collision point" };
        }
        if (!overlap(m_nodes.front().bounds, { .lo = p, .hi = p })) {
            return false;
        }
        for (auto direction : CONTAINMENT_RAYS) {
            unsigned crossings{};
            bool ambiguous{};
            ray(0, p, direction, crossings, ambiguous);
            if (!ambiguous) {
                return (crossings % 2) != 0;
            }
        }
        throw std::runtime_error{ "ambiguous collision containment" };
    }
    double CollisionMesh::pointDistanceSquared(glm::dvec3 p, const Triangle& t)
    {
        const auto ab{ t.b - t.a };
        const auto ac{ t.c - t.a };
        const auto ap{ p - t.a };
        const double d1{ glm::dot(ab, ap) };
        const double d2{ glm::dot(ac, ap) };
        if (d1 <= 0 && d2 <= 0) {
            return glm::dot(ap, ap);
        }
        const auto bp{ p - t.b };
        const double d3{ glm::dot(ab, bp) };
        const double d4{ glm::dot(ac, bp) };
        if (d3 >= 0 && d4 <= d3) {
            return glm::dot(bp, bp);
        }
        const double vc{ (d1 * d4) - (d3 * d2) };
        if (vc <= 0 && d1 >= 0 && d3 <= 0) {
            const auto delta{ ap - ab * (d1 / (d1 - d3)) };
            return glm::dot(delta, delta);
        }
        const auto cp{ p - t.c };
        const double d5{ glm::dot(ab, cp) };
        const double d6{ glm::dot(ac, cp) };
        if (d6 >= 0 && d5 <= d6) {
            return glm::dot(cp, cp);
        }
        const double vb{ (d5 * d2) - (d1 * d6) };
        if (vb <= 0 && d2 >= 0 && d6 <= 0) {
            const auto delta{ ap - ac * (d2 / (d2 - d6)) };
            return glm::dot(delta, delta);
        }
        const double va{ (d3 * d6) - (d5 * d4) };
        if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
            const auto delta{ bp - (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))) };
            return glm::dot(delta, delta);
        }
        const double denominator{ 1 / (va + vb + vc) };
        const auto delta{ ap - ab * (vb * denominator) - ac * (vc * denominator) };
        return glm::dot(delta, delta);
    }
    double CollisionMesh::segmentDistanceSquared(const EdgeSegment& first, const EdgeSegment& second)
    {
        const auto& p{ first.begin };
        const auto& p2{ first.end };
        const auto& q{ second.begin };
        const auto& q2{ second.end };
        const auto d1{ p2 - p };
        const auto d2{ q2 - q };
        const auto r{ p - q };
        const double a{ glm::dot(d1, d1) };
        const double e{ glm::dot(d2, d2) };
        const double f{ glm::dot(d2, r) };
        const double c{ glm::dot(d1, r) };
        const double b{ glm::dot(d1, d2) };
        const double denominator{ (a * e) - (b * b) };
        double s{ denominator > SEGMENT_DETERMINANT_EPSILON
                    ? std::clamp((b * f - c * e) / denominator, 0.0, 1.0)
                    : 0.0 };
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
    }
    double CollisionMesh::triangleDistanceSquared(const Triangle& a, const Triangle& b)
    {
        if (trianglesOverlap(a, b)) {
            return 0;
        }
        double best{ std::min({ pointDistanceSquared(a.a, b),
                                pointDistanceSquared(a.b, b),
                                pointDistanceSquared(a.c, b),
                                pointDistanceSquared(b.a, a),
                                pointDistanceSquared(b.b, a),
                                pointDistanceSquared(b.c, a) }) };
        const std::array av{ a.a, a.b, a.c };
        const std::array bv{ b.a, b.b, b.c };
        for (unsigned i{}; i < 3; ++i) {
            for (unsigned j{}; j < 3; ++j) {
                best = std::min(best,
                                segmentDistanceSquared({ .begin = av[i], .end = av[(i + 1) % av.size()] },
                                                       { .begin = bv[j], .end = bv[(j + 1) % bv.size()] }));
            }
        }
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
        if (lower(node.bounds) > best) {
            return;
        }
        if (!node.count) {
            auto first{ node.left };
            auto second{ node.right };
            if (lower(m_nodes[first].bounds) > lower(m_nodes[second].bounds)) {
                std::swap(first, second);
            }
            distance(first, triangle, box, best);
            if (best > 0) {
                distance(second, triangle, box, best);
            }
            return;
        }
        for (std::size_t i{ node.begin }; i < node.begin + node.count && best > 0; ++i) {
            if (lower(bounds(m_triangles[i])) <= best) {
                best = std::min(best, triangleDistanceSquared(m_triangles[i], triangle));
            }
        }
    }
    double CollisionMesh::distance(const CollisionMesh& other, const glm::dmat4& transform) const
    {
        for (glm::length_t c{}; c < 4; ++c) {
            for (glm::length_t r{}; r < 4; ++r) {
                if (!std::isfinite(transform[c][r])) {
                    throw std::invalid_argument{ "invalid collision transform" };
                }
            }
        }
        if (transform[0][3] != 0 || transform[1][3] != 0 || transform[2][3] != 0 || transform[3][3] != 1 ||
            std::abs(glm::determinant(glm::dmat3{ transform })) < TRANSFORM_DETERMINANT_EPSILON) {
            throw std::invalid_argument{ "collision transform must be affine and invertible" };
        }
        double best{ std::numeric_limits<double>::infinity() };
        for (const auto& t : other.m_triangles) {
            const Triangle moved{ .a = glm::dvec3{ transform * glm::dvec4{ t.a, 1 } },
                                  .b = glm::dvec3{ transform * glm::dvec4{ t.b, 1 } },
                                  .c = glm::dvec3{ transform * glm::dvec4{ t.c, 1 } } };
            distance(0, moved, bounds(moved), best);
            if (best <= EPS * EPS) {
                return 0;
            }
        }
        for (const auto sample : other.m_components) {
            if (contains(glm::dvec3{ transform * glm::dvec4{ sample, 1 } })) {
                return 0;
            }
        }
        const auto inverse{ glm::inverse(transform) };
        for (const auto sample : m_components) {
            if (other.contains(glm::dvec3{ inverse * glm::dvec4{ sample, 1 } })) {
                return 0;
            }
        }
        return std::sqrt(best);
    }
    bool CollisionMesh::intersects(const CollisionMesh& other, const glm::dmat4& transform) const
    {
        return distance(other, transform) == 0;
    }
    double CollisionMesh::radiusFromZAxis(double x, double y, const glm::dmat4& transform) const noexcept
    {
        double radius{};
        for (const auto& t : m_triangles) {
            for (auto p : { t.a, t.b, t.c }) {
                const auto moved{ transform * glm::dvec4{ p, 1 } };
                radius = std::max(radius, std::hypot(moved[0] - x, moved[1] - y));
            }
        }
        return radius;
    }
}
