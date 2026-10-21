#include "mem/mem.hpp"
#include "test.hpp"

namespace {

constexpr uint32_t kRamSize = mem::Ram::kRamSize;

}  // namespace

TEST(ram_starts_zeroed) {
    mem::Ram r;
    CHECK_EQ(r.read(0, 4).value(), 0);
    CHECK_EQ(r.read(kRamSize / 2, 4).value(), 0);
    CHECK_EQ(r.read(kRamSize - 4, 4).value(), 0);
}

TEST(ram_is_little_endian) {
    mem::Ram r;
    CHECK(r.write(100, 4, 0x12345678));
    CHECK_EQ(r.read(100, 1).value(), 0x78);   // lowest address = least significant byte
    CHECK_EQ(r.read(101, 1).value(), 0x56);
    CHECK_EQ(r.read(102, 1).value(), 0x34);
    CHECK_EQ(r.read(103, 1).value(), 0x12);
    CHECK_EQ(r.read(100, 2).value(), 0x5678);
    CHECK_EQ(r.read(102, 2).value(), 0x1234);
    CHECK_EQ(r.read(100, 4).value(), 0x12345678);
}

TEST(ram_write_size_limits_bytes_written) {
    mem::Ram r;
    CHECK(r.write(200, 4, 0xaabbccdd));
    CHECK(r.write(200, 1, 0x11223344));   // stores only the low byte, 0x44
    CHECK_EQ(r.read(200, 4).value(), 0xaabbcc44);
    CHECK(r.write(202, 2, 0x55667788));   // stores only the low half, 0x7788
    CHECK_EQ(r.read(200, 4).value(), 0x7788cc44);
}

TEST(ram_works_at_both_ends) {
    mem::Ram r;
    CHECK(r.write(0, 4, 0xdeadbeef));
    CHECK_EQ(r.read(0, 4).value(), 0xdeadbeef);

    CHECK(r.write(kRamSize - 4, 4, 0xcafef00d));
    CHECK_EQ(r.read(kRamSize - 4, 4).value(), 0xcafef00d);
    CHECK_EQ(r.read(kRamSize - 2, 2).value(), 0xcafe);
    CHECK_EQ(r.read(kRamSize - 1, 1).value(), 0xca);
}

TEST(ram_rejects_access_past_the_end) {
    mem::Ram r;
    // Each access starts inside RAM, or just past it, but its last byte is outside.
    CHECK(!r.read(kRamSize - 3, 4).has_value());
    CHECK(!r.read(kRamSize - 1, 2).has_value());
    CHECK(!r.read(kRamSize, 1).has_value());
    CHECK(!r.write(kRamSize - 3, 4, 0));
    CHECK(!r.write(kRamSize - 1, 2, 0));
    CHECK(!r.write(kRamSize, 1, 0));
}

TEST(ram_rejects_addresses_that_would_wrap) {
    mem::Ram r;
    // address + size overflows 32 bits; a naive bounds check would accept these.
    CHECK(!r.read(0xffffffff, 1).has_value());
    CHECK(!r.read(0xfffffffe, 4).has_value());
    CHECK(!r.read(0x80000000, 4).has_value());
    CHECK(!r.write(0xffffffff, 1, 0));
    CHECK(!r.write(0xfffffffe, 4, 0));
}

TEST(ram_failed_write_changes_nothing) {
    mem::Ram r;
    CHECK(r.write(kRamSize - 2, 2, 0xbeef));
    CHECK(!r.write(kRamSize - 2, 4, 0x12345678));   // 2 bytes in range, 2 out: must write none
    CHECK_EQ(r.read(kRamSize - 2, 2).value(), 0xbeef);
}

TEST(ram_allows_unaligned_access) {
    // Alignment is the CPU's rule (spec A1), not RAM's.
    mem::Ram r;
    CHECK(r.write(1, 4, 0x01020304));
    CHECK_EQ(r.read(1, 4).value(), 0x01020304);
}
