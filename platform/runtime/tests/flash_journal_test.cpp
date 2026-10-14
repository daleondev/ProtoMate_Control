#include "libc/storage/FlashJournal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <vector>

namespace
{
    using runtime::storage::FlashDevice;
    using runtime::storage::FlashJournal;
    using Data = std::array<std::byte, FlashJournal::SECTOR_BYTES>;
    class FlashModel final : public FlashDevice
    {
      public:
        std::vector<std::byte> bytes = std::vector<std::byte>(FlashJournal::BYTES, std::byte{0xFF});
        std::set<std::size_t> programmed;
        std::set<std::size_t> eccErrors;
        int failAfter{-1};
        int operations{};
        std::array<unsigned, FlashJournal::BLOCKS> erases{};
        bool boundsViolation{};
        bool doubleProgram{};

        bool read(std::size_t at, std::span<std::byte> out) noexcept override
        {
            if (at > bytes.size() || out.size() > bytes.size() - at) { boundsViolation = true; return false; }
            for (auto bad : eccErrors) {
                if (bad < at + out.size() && bad + 32 > at) { return false; }
            }
            std::copy_n(bytes.begin() + at, out.size(), out.begin()); return true;
        }
        bool program(std::size_t at, std::span<const std::byte, 32> data) noexcept override
        {
            if (at % 32 || at > bytes.size() - 32) { boundsViolation = true; return false; }
            if (!programmed.insert(at).second) { doubleProgram = true; return false; }
            if (operations++ == failAfter) {
                std::copy_n(data.begin(), 11, bytes.begin() + at); // Torn flashword with invalid ECC.
                eccErrors.insert(at);
                return false;
            }
            std::copy(data.begin(), data.end(), bytes.begin() + at); return true;
        }
        bool erase(std::size_t block) noexcept override
        {
            if (block >= FlashJournal::BLOCKS) { boundsViolation = true; return false; }
            const auto at = block * FlashJournal::BLOCK_BYTES;
            const bool failed = operations++ == failAfter;
            const auto count = failed ? FlashJournal::BLOCK_BYTES / 2 : FlashJournal::BLOCK_BYTES;
            std::fill_n(bytes.begin() + at, count, std::byte{0xFF});
            std::erase_if(programmed, [=](auto p) { return p >= at && p < at + count; });
            std::erase_if(eccErrors, [=](auto p) { return p >= at && p < at + count; });
            if (failed) { eccErrors.insert(at + count); return false; }
            ++erases[block]; return true;
        }
    };
    Data pattern(unsigned seed)
    {
        Data data{};
        std::mt19937 random{seed};
        for (auto& byte : data) { byte = std::byte(random()); }
        return data;
    }
    void expectContents(FlashJournal& journal, const std::vector<Data>& expected)
    {
        Data actual{};
        for (std::size_t i{}; i < expected.size(); ++i) {
            ASSERT_TRUE(journal.read(i, actual)) << i;
            ASSERT_EQ(actual, expected[i]) << i;
        }
    }
}

TEST(FlashJournal, BoundsBlankReadsAndRemount)
{
    FlashModel flash;
    FlashJournal journal{flash};
    ASSERT_EQ(journal.mount(), FlashJournal::Mount::Blank);
    Data zero{}, read{};
    EXPECT_TRUE(journal.read(0, read)); EXPECT_EQ(read, zero);
    EXPECT_FALSE(journal.read(512, read)); EXPECT_FALSE(journal.write(512, zero));
    const auto content = pattern(42);
    ASSERT_TRUE(journal.write(511, content));
    const auto calls = flash.operations;
    EXPECT_TRUE(journal.write(511, content)); EXPECT_EQ(flash.operations, calls);
    FlashJournal reopened{flash}; ASSERT_EQ(reopened.mount(), FlashJournal::Mount::Ready);
    ASSERT_TRUE(reopened.read(511, read)); EXPECT_EQ(read, content);
    EXPECT_TRUE(reopened.release(511));
    FlashJournal released{flash}; ASSERT_EQ(released.mount(), FlashJournal::Mount::Ready);
    ASSERT_TRUE(released.read(511, read)); EXPECT_EQ(read, zero);
    EXPECT_FALSE(flash.boundsViolation); EXPECT_FALSE(flash.doubleProgram);
}

TEST(FlashJournal, FullVolumeRepeatedUpdatesReclaimAndPreserveColdData)
{
    FlashModel flash;
    FlashJournal journal{flash}; ASSERT_EQ(journal.mount(), FlashJournal::Mount::Blank);
    std::vector<Data> expected(FlashJournal::SECTORS);
    for (unsigned i{}; i < expected.size(); ++i) { expected[i] = pattern(i); ASSERT_TRUE(journal.write(i, expected[i])); }
    for (unsigned i{}; i < 1600; ++i) {
        const auto sector = i % 250;
        expected[sector] = pattern(i + 1000);
        ASSERT_TRUE(journal.write(sector, expected[sector])) << i;
        if (i % 100 == 0) { ASSERT_EQ(journal.mount(), FlashJournal::Mount::Ready); }
    }
    EXPECT_GT(journal.erases(), 5U);
    ASSERT_EQ(journal.mount(), FlashJournal::Mount::Ready);
    expectContents(journal, expected);
    EXPECT_FALSE(flash.doubleProgram); EXPECT_FALSE(flash.boundsViolation);
}

TEST(FlashJournal, EveryTornWriteKeepsThePreviousCommittedSector)
{
    FlashModel original;
    FlashJournal journal{original}; ASSERT_EQ(journal.mount(), FlashJournal::Mount::Blank);
    const auto before = pattern(1), after = pattern(2);
    ASSERT_TRUE(journal.write(3, before));
    for (int cut{}; cut < 17; ++cut) {
        auto flash = original;
        FlashJournal writing{flash}; ASSERT_EQ(writing.mount(), FlashJournal::Mount::Ready);
        flash.failAfter = flash.operations + cut;
        EXPECT_FALSE(writing.write(3, after)) << cut;
        flash.failAfter = -1;
        FlashJournal recovered{flash}; ASSERT_EQ(recovered.mount(), FlashJournal::Mount::Ready) << cut;
        Data value{}; ASSERT_TRUE(recovered.read(3, value)); EXPECT_EQ(value, before) << cut;
        ASSERT_TRUE(recovered.write(3, after));
        EXPECT_FALSE(flash.doubleProgram);
    }
}

TEST(FlashJournal, InterruptedReclamationAndEraseCanResume)
{
    FlashModel original;
    FlashJournal journal{original}; ASSERT_EQ(journal.mount(), FlashJournal::Mount::Blank);
    std::vector<Data> expected(FlashJournal::SECTORS);
    for (unsigned i{}; i < 720; ++i) {
        const auto sector = i % 512;
        expected[sector] = pattern(i);
        ASSERT_TRUE(journal.write(sector, expected[sector]));
    }
    auto probe = original;
    FlashJournal probeJournal{probe}; ASSERT_EQ(probeJournal.mount(), FlashJournal::Mount::Ready);
    ASSERT_TRUE(probeJournal.write(400, pattern(9000)));
    const auto operations = probe.operations - original.operations;
    ASSERT_GT(operations, 17);
    // Include both sides of every record boundary, the erase, and the final write.
    for (int cut{}; cut < operations; cut += 17) {
        for (const int delta : {0, 1, 16}) {
            if (cut + delta >= operations) { continue; }
            auto flash = original;
            FlashJournal writing{flash}; ASSERT_EQ(writing.mount(), FlashJournal::Mount::Ready);
            flash.failAfter = flash.operations + cut + delta;
            EXPECT_FALSE(writing.write(400, pattern(9000)));
            flash.failAfter = -1;
            FlashJournal recovered{flash}; ASSERT_EQ(recovered.mount(), FlashJournal::Mount::Ready) << cut + delta;
            expectContents(recovered, expected);
            ASSERT_TRUE(recovered.write(400, pattern(9001))) << cut + delta;
            EXPECT_FALSE(flash.doubleProgram); EXPECT_FALSE(flash.boundsViolation);
        }
    }
}

TEST(FlashJournal, RejectsUnknownDataAndCommittedCorruptionWithoutFormatting)
{
    FlashModel foreign;
    foreign.bytes[0] = std::byte{0};
    FlashJournal unknown{foreign}; EXPECT_EQ(unknown.mount(), FlashJournal::Mount::Corrupt);
    EXPECT_EQ(foreign.operations, 0);
    FlashModel flash;
    FlashJournal journal{flash}; ASSERT_EQ(journal.mount(), FlashJournal::Mount::Blank);
    ASSERT_TRUE(journal.write(0, pattern(1)));
    const auto programmed = *flash.programmed.begin();
    flash.bytes[programmed] ^= std::byte{1};
    EXPECT_EQ(journal.mount(), FlashJournal::Mount::Corrupt);
    EXPECT_FALSE(journal.write(1, pattern(2)));
}
