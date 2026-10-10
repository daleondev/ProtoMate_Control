#pragma once
#include "ITransport.hpp"
#include <bit>
#include <cstdint>
#include <limits>
#include <pneumo/common.hpp>
#include <stdexcept>
#include <string>

namespace p2p
{
    inline constexpr std::size_t MaxPayload = 2048;
    // Explicit little-endian primitives; no ABI-dependent object copies.
    class Writer
    {
      public:
        Bytes bytes;
        template<std::unsigned_integral T>
        void integer(T value)
        {
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                bytes.push_back(std::byte(value & 0xffU));
                value >>= 8U;
            }
        }
        void boolean(bool value) { integer<std::uint8_t>(value ? 1 : 0); }
        void real(double value)
        {
            static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
            integer(std::bit_cast<std::uint64_t>(value));
        }
        void text(std::string_view value)
        {
            if (value.size() > 255)
                throw std::length_error("p2p text exceeds 255 bytes");
            integer<std::uint8_t>(static_cast<std::uint8_t>(value.size()));
            for (auto c : value)
                bytes.push_back(std::byte(static_cast<unsigned char>(c)));
        }
    };
    class Reader
    {
      public:
        explicit Reader(std::span<const std::byte> value)
          : m_bytes(value)
        {
        }
        template<std::unsigned_integral T>
        T integer()
        {
            if (m_bytes.size() < sizeof(T))
                throw std::invalid_argument("truncated p2p value");
            T result{};
            for (std::size_t i = 0; i < sizeof(T); ++i)
                result |= T(std::to_integer<T>(m_bytes[i]) << (8U * i));
            m_bytes = m_bytes.subspan(sizeof(T));
            return result;
        }
        bool boolean()
        {
            auto v = integer<std::uint8_t>();
            if (v > 1)
                throw std::invalid_argument("invalid p2p bool");
            return v != 0;
        }
        double real() { return std::bit_cast<double>(integer<std::uint64_t>()); }
        std::string text()
        {
            auto n = integer<std::uint8_t>();
            if (m_bytes.size() < n)
                throw std::invalid_argument("truncated p2p text");
            std::string s;
            for (auto b : m_bytes.first(n))
                s.push_back(char(std::to_integer<unsigned char>(b)));
            m_bytes = m_bytes.subspan(n);
            return s;
        }
        void finish() const
        {
            if (!m_bytes.empty())
                throw std::invalid_argument("trailing p2p data");
        }
        std::span<const std::byte> remaining() const { return m_bytes; }

      private:
        std::span<const std::byte> m_bytes;
    };
    // Compound messages opt in with an explicit specialization. Native pneumo
    // serialization is never implicitly treated as a portable wire format.
    template<typename T>
    struct Codec;
    template<std::integral T>
    struct Codec<T>
    {
        static Bytes encode(T value)
        {
            Writer w;
            if constexpr (std::same_as<T, bool>)
                w.boolean(value);
            else
                w.integer(std::bit_cast<std::make_unsigned_t<T>>(value));
            return std::move(w.bytes);
        }
        static T decode(std::span<const std::byte> bytes)
        {
            Reader r(bytes);
            T v;
            if constexpr (std::same_as<T, bool>)
                v = r.boolean();
            else
                v = std::bit_cast<T>(r.integer<std::make_unsigned_t<T>>());
            r.finish();
            return v;
        }
    };
    template<>
    struct Codec<double>
    {
        static Bytes encode(double value)
        {
            Writer w;
            w.real(value);
            return std::move(w.bytes);
        }
        static double decode(std::span<const std::byte> bytes)
        {
            Reader r(bytes);
            auto v = r.real();
            r.finish();
            return v;
        }
    };
    template<typename T>
    Bytes encode(const T& value)
    {
        auto b = Codec<T>::encode(value);
        if (b.size() > MaxPayload)
            throw std::length_error("p2p payload too large");
        return b;
    }
    template<typename T>
    T decode(std::span<const std::byte> bytes)
    {
        if (bytes.size() > MaxPayload)
            throw std::length_error("p2p payload too large");
        return Codec<T>::decode(bytes);
    }
}
