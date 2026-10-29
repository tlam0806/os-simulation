// The file system's on-disk layout (docs/machine-spec.md, C1), host side.
//
//   fs_sizes_     the constants agree with the spec and the disk
//   fs_super_     superblock bytes
//   fs_inode_     inode bytes
//   fs_dirent_    directory entry bytes and valid names
//   fs_block_     read_block / write_block on a disk image
//   fs_inodeio_   read_inode / write_inode: one 64-byte slot in the table
//   fs_format_    an empty file system
#include <cstdint>
#include <string>
#include <vector>

#include "disk/disk.hpp"
#include "fs/layout.hpp"
#include "test.hpp"

using fs::Block;
using fs::DirEntry;
using fs::Inode;
using fs::InodeType;

namespace {

uint32_t word(std::span<const uint8_t> bytes, size_t at) {
    return bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | static_cast<uint32_t>(bytes[at + 3]) << 24;
}

uint16_t half(std::span<const uint8_t> bytes, size_t at) {
    return static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8);
}

// A block whose bytes are 1, 2, 3, ... (wrapping).
Block pattern(uint8_t start = 1) {
    Block b{};
    for (size_t i = 0; i < b.size(); i++) b[i] = static_cast<uint8_t>(start + i);
    return b;
}

// An inode with every field set to something distinct.
Inode busy_inode() {
    Inode n;
    n.type = InodeType::file;
    n.links = 3;
    n.size = 0x00012345;
    for (uint32_t k = 0; k < fs::kNumDirect; k++) n.direct[k] = 100 + k;
    n.indirect = 0xABCD;
    return n;
}

}  // namespace

// =================== sizes ===================

TEST(fs_sizes_blocks_cover_the_disk) {
    CHECK_EQ(fs::kBlockSize, 4096u);
    CHECK_EQ(fs::kSectorsPerBlock, 8u);
    CHECK_EQ(fs::kNumBlocks, 16384u);
    CHECK_EQ(fs::kNumBlocks * fs::kSectorsPerBlock, disk::kNumSectors);
}

TEST(fs_sizes_regions) {
    CHECK_EQ(fs::kInodesPerBlock, 64u);
    CHECK_EQ(fs::kNumInodes, 1024u);
    CHECK_EQ(fs::kDataStart, 18u);
    CHECK_EQ(fs::kMaxFileBlocks * fs::kBlockSize, 4243456u);
    CHECK_EQ(fs::kBlockSize / fs::kDirEntrySize, 128u);   // entries per block
}

// =================== superblock ===================

TEST(fs_super_bytes) {
    const fs::Superblock sb{fs::kMagic, 16384, 1024, 1, 2, 16, 18};
    const Block b = fs::encode_superblock(sb);
    CHECK_EQ(b[0], static_cast<uint8_t>('O'));   // magic reads "OSFS"
    CHECK_EQ(b[1], static_cast<uint8_t>('S'));
    CHECK_EQ(b[2], static_cast<uint8_t>('F'));
    CHECK_EQ(b[3], static_cast<uint8_t>('S'));
    CHECK_EQ(word(b, 4), 16384u);
    CHECK_EQ(word(b, 8), 1024u);
    CHECK_EQ(word(b, 12), 1u);
    CHECK_EQ(word(b, 16), 2u);
    CHECK_EQ(word(b, 20), 16u);
    CHECK_EQ(word(b, 24), 18u);
    bool rest_zero = true;
    for (size_t i = 28; i < b.size(); i++) rest_zero = rest_zero && b[i] == 0;
    CHECK(rest_zero);
}

TEST(fs_super_round_trip) {
    const fs::Superblock sb{0x11223344, 5, 6, 7, 8, 9, 10};
    const Block b = fs::encode_superblock(sb);
    CHECK(fs::decode_superblock(b) == sb);
}

// =================== inodes ===================

TEST(fs_inode_bytes) {
    const auto b = fs::encode_inode(busy_inode());
    CHECK_EQ(half(b, 0), 1u);              // type: file
    CHECK_EQ(half(b, 2), 3u);              // links
    CHECK_EQ(word(b, 4), 0x00012345u);     // size
    CHECK_EQ(word(b, 8), 100u);            // direct[0]
    CHECK_EQ(word(b, 52), 111u);           // direct[11]
    CHECK_EQ(word(b, 56), 0xABCDu);        // indirect
    CHECK_EQ(word(b, 60), 0u);             // unused
}

TEST(fs_inode_directory_type_is_2) {
    Inode n;
    n.type = InodeType::directory;
    CHECK_EQ(half(fs::encode_inode(n), 0), 2u);
}

TEST(fs_inode_free_inode_is_all_zeros) {
    const auto b = fs::encode_inode(Inode{});
    bool all_zero = true;
    for (uint8_t x : b) all_zero = all_zero && x == 0;
    CHECK(all_zero);
}

TEST(fs_inode_round_trip) {
    const Inode n = busy_inode();
    const auto b = fs::encode_inode(n);
    CHECK(fs::decode_inode(b) == n);
}

// =================== directory entries ===================

TEST(fs_dirent_bytes) {
    const auto b = fs::encode_dir_entry({0x01020304, "hello"});
    CHECK_EQ(word(b, 0), 0x01020304u);
    CHECK_EQ(std::string(b.begin() + 4, b.begin() + 9), std::string("hello"));
    bool padded = true;
    for (size_t i = 9; i < b.size(); i++) padded = padded && b[i] == 0;
    CHECK(padded);
}

TEST(fs_dirent_longest_name_keeps_a_zero_byte) {
    const std::string name(27, 'x');
    const auto b = fs::encode_dir_entry({7, name});
    CHECK_EQ(b[30], static_cast<uint8_t>('x'));   // byte 26 of the name
    CHECK_EQ(b[31], 0u);
    CHECK(fs::decode_dir_entry(b) == (DirEntry{7, name}));
}

TEST(fs_dirent_round_trip) {
    const DirEntry e{42, "bin"};
    CHECK(fs::decode_dir_entry(fs::encode_dir_entry(e)) == e);
}

TEST(fs_dirent_empty_entry_is_all_zeros) {
    const auto b = fs::encode_dir_entry({0, ""});
    bool all_zero = true;
    for (uint8_t x : b) all_zero = all_zero && x == 0;
    CHECK(all_zero);
    CHECK(fs::decode_dir_entry(b) == (DirEntry{0, ""}));
}

TEST(fs_dirent_decode_stops_at_first_zero) {
    std::array<uint8_t, fs::kDirEntrySize> b{};
    b[0] = 5;
    b[4] = 'a';
    b[5] = 'b';
    b[6] = 0;
    b[7] = 'z';   // after the end of the name: ignored
    CHECK(fs::decode_dir_entry(b) == (DirEntry{5, "ab"}));
}

TEST(fs_dirent_valid_names) {
    CHECK(fs::valid_name("a"));
    CHECK(fs::valid_name("hello.txt"));
    CHECK(fs::valid_name(std::string(27, 'n')));
    CHECK(!fs::valid_name(""));
    CHECK(!fs::valid_name(std::string(28, 'n')));
    CHECK(!fs::valid_name("a/b"));
    CHECK(!fs::valid_name(std::string("a\0b", 3)));
}

// =================== blocks ===================

TEST(fs_block_write_then_read) {
    disk::Disk d;
    const Block b = pattern();
    CHECK(fs::write_block(d, 5, b));
    const auto back = fs::read_block(d, 5);
    CHECK(back.has_value());
    if (back) CHECK(*back == b);
}

TEST(fs_block_is_eight_sectors) {
    // Block 5 is sectors 40..47.
    disk::Disk d;
    CHECK(fs::write_block(d, 5, pattern()));
    const auto sectors = d.host_read(40, 8);
    CHECK(sectors.has_value());
    if (sectors) {
        CHECK_EQ((*sectors)[0], 1u);
        CHECK_EQ((*sectors)[4095], static_cast<uint8_t>(1 + 4095));
    }
    const auto before = d.host_read(39, 1);
    const auto after = d.host_read(48, 1);
    CHECK(before && (*before)[511] == 0);   // neighbours untouched
    CHECK(after && (*after)[0] == 0);
}

TEST(fs_block_last_block) {
    disk::Disk d;
    CHECK(fs::write_block(d, fs::kNumBlocks - 1, pattern(9)));
    const auto back = fs::read_block(d, fs::kNumBlocks - 1);
    CHECK(back && (*back)[0] == 9);
}

TEST(fs_block_past_the_end_is_refused) {
    disk::Disk d;
    CHECK(!fs::write_block(d, fs::kNumBlocks, pattern()));
    CHECK(!fs::read_block(d, fs::kNumBlocks).has_value());
    CHECK(!fs::read_block(d, 0xFFFFFFFF).has_value());
}

// =================== inode table ===================

TEST(fs_inodeio_write_then_read) {
    disk::Disk d;
    CHECK(fs::write_inode(d, 7, busy_inode()));
    const auto back = fs::read_inode(d, 7);
    CHECK(back.has_value());
    if (back) CHECK(*back == busy_inode());
}

TEST(fs_inodeio_slot_position) {
    // Inode 65 is the second slot of the second inode block: block 3, offset 64.
    disk::Disk d;
    CHECK(fs::write_inode(d, 65, busy_inode()));
    const auto b = fs::read_block(d, 3);
    CHECK(b.has_value());
    if (b) {
        CHECK_EQ(half(*b, 64), 1u);           // type
        CHECK_EQ(word(*b, 64 + 4), 0x00012345u);
    }
}

TEST(fs_inodeio_neighbours_are_untouched) {
    disk::Disk d;
    CHECK(fs::write_block(d, fs::kInodeStart, pattern()));
    CHECK(fs::write_inode(d, 1, Inode{}));            // bytes 64..127 of block 2
    const auto b = fs::read_block(d, fs::kInodeStart);
    CHECK(b.has_value());
    if (b) {
        CHECK_EQ((*b)[63], static_cast<uint8_t>(1 + 63));     // inode 0 kept
        CHECK_EQ((*b)[64], 0u);                               // inode 1 cleared
        CHECK_EQ((*b)[127], 0u);
        CHECK_EQ((*b)[128], static_cast<uint8_t>(1 + 128));   // inode 2 kept
    }
}

TEST(fs_inodeio_out_of_range) {
    disk::Disk d;
    CHECK(!fs::write_inode(d, fs::kNumInodes, busy_inode()));
    CHECK(!fs::read_inode(d, fs::kNumInodes).has_value());
    CHECK(fs::read_inode(d, fs::kNumInodes - 1).has_value());
}

// =================== format ===================

TEST(fs_format_blank_disk_is_not_formatted) {
    disk::Disk d;
    CHECK(!fs::is_formatted(d));
}

TEST(fs_format_then_formatted) {
    disk::Disk d;
    fs::format(d);
    CHECK(fs::is_formatted(d));
}

TEST(fs_format_wrong_magic_is_not_formatted) {
    disk::Disk d;
    fs::format(d);
    CHECK(d.host_write(0, std::vector<uint8_t>{'O', 'S', 'F', 'X'}));
    CHECK(!fs::is_formatted(d));
}

TEST(fs_format_superblock) {
    disk::Disk d;
    fs::format(d);
    const auto b = fs::read_block(d, fs::kSuperblock);
    CHECK(b.has_value());
    if (b) CHECK(fs::decode_superblock(*b) == (fs::Superblock{fs::kMagic, 16384, 1024, 1, 2, 16, 18}));
}

TEST(fs_format_bitmap_marks_the_metadata_blocks) {
    // Blocks 0..17 in use: bytes 0 and 1 are 0xFF, byte 2 has bits 0 and 1.
    disk::Disk d;
    fs::format(d);
    const auto b = fs::read_block(d, fs::kBitmapBlock);
    CHECK(b.has_value());
    if (b) {
        CHECK_EQ((*b)[0], 0xFFu);
        CHECK_EQ((*b)[1], 0xFFu);
        CHECK_EQ((*b)[2], 0x03u);
        bool rest_zero = true;
        for (size_t i = 3; i < b->size(); i++) rest_zero = rest_zero && (*b)[i] == 0;
        CHECK(rest_zero);
    }
}

TEST(fs_format_root_is_an_empty_directory) {
    disk::Disk d;
    fs::format(d);
    Inode root;
    root.type = InodeType::directory;
    root.links = 1;
    const auto got = fs::read_inode(d, fs::kRootInode);
    CHECK(got.has_value());
    if (got) CHECK(*got == root);
}

TEST(fs_format_every_other_inode_is_free) {
    disk::Disk d;
    fs::format(d);
    bool all_free = true;
    for (uint32_t i = 0; i < fs::kNumInodes; i++) {
        if (i == fs::kRootInode) continue;
        const auto n = fs::read_inode(d, i);
        all_free = all_free && n.has_value() && *n == Inode{};
    }
    CHECK(all_free);
}

TEST(fs_format_clears_an_old_file_system) {
    // Old inodes and bitmap bits from before are gone; data blocks are not touched.
    disk::Disk d;
    for (uint32_t b = 0; b < fs::kDataStart + 1; b++) CHECK(fs::write_block(d, b, pattern()));
    fs::format(d);
    const auto n = fs::read_inode(d, 500);
    CHECK(n && *n == Inode{});
    const auto bitmap = fs::read_block(d, fs::kBitmapBlock);
    CHECK(bitmap && (*bitmap)[3] == 0 && (*bitmap)[4095] == 0);
    const auto data = fs::read_block(d, fs::kDataStart);
    CHECK(data && *data == pattern());
}
