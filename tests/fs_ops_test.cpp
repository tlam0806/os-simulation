// Host-side file-system operations (docs/machine-spec.md, C1; L11 "Create").
//
//   fs_alloc_    allocate_block / allocate_inode and the free counts
//   fs_file_     create_file, read_file, block_of: direct and indirect blocks
//   fs_dir_      entries: lookup, list_dir, growing a directory, reusing a slot
//   fs_create_   create_file errors; the disk is unchanged after one
//   fs_mkdir_    make_directory and nesting
//   fs_path_     resolve
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "disk/disk.hpp"
#include "fs/layout.hpp"
#include "fs/ops.hpp"
#include "test.hpp"

using fs::Block;
using fs::DirEntry;
using fs::Error;
using fs::Inode;
using fs::InodeType;

namespace {

constexpr uint32_t kRoot = fs::kRootInode;
constexpr uint32_t kFreshFreeBlocks = fs::kNumBlocks - fs::kDataStart - fs::kSwapBlocks;   // 15854: the swap area is in use (D1)
constexpr uint32_t kFreshFreeInodes = fs::kNumInodes - 2;                // 1022: not 0, not the root

// A formatted disk.
struct Fs {
    disk::Disk d;
    Fs() { fs::format(d); }
};

// n bytes that differ from block to block, so a block in the wrong place shows.
std::vector<uint8_t> contents(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(i + 13 * (i / fs::kBlockSize) + 1);
    return v;
}

std::vector<uint8_t> text(const std::string& s) { return {s.begin(), s.end()}; }

bool is_ok(const std::variant<uint32_t, Error>& r) { return std::holds_alternative<uint32_t>(r); }

uint32_t ok(const std::variant<uint32_t, Error>& r) {
    CHECK(is_ok(r));
    return is_ok(r) ? std::get<uint32_t>(r) : 0;
}

bool is_error(const std::variant<uint32_t, Error>& r, Error e) {
    return std::holds_alternative<Error>(r) && std::get<Error>(r) == e;
}

Inode inode(const disk::Disk& d, uint32_t i) {
    const auto n = fs::read_inode(d, i);
    CHECK(n.has_value());
    return n.value_or(Inode{});
}

// Leaves exactly the blocks in `free` free; every other block is in use.
void only_free(disk::Disk& d, std::vector<uint32_t> free) {
    Block bitmap;
    bitmap.fill(0xFF);
    for (uint32_t b : free) bitmap[b / 8] &= static_cast<uint8_t>(~(1u << (b % 8)));
    CHECK(fs::write_block(d, fs::kBitmapBlock, bitmap));
}

}  // namespace

// =================== allocation ===================

TEST(fs_alloc_fresh_counts) {
    Fs f;
    CHECK_EQ(fs::free_block_count(f.d), kFreshFreeBlocks);
    CHECK_EQ(fs::free_inode_count(f.d), kFreshFreeInodes);
    CHECK(fs::block_in_use(f.d, 0));
    CHECK(fs::block_in_use(f.d, fs::kDataStart - 1));
    CHECK(!fs::block_in_use(f.d, fs::kDataStart));
}

TEST(fs_alloc_blocks_lowest_first) {
    Fs f;
    CHECK_EQ(fs::allocate_block(f.d).value_or(0), 18u);
    CHECK_EQ(fs::allocate_block(f.d).value_or(0), 19u);
    CHECK(fs::block_in_use(f.d, 18));
    CHECK(fs::block_in_use(f.d, 19));
    CHECK_EQ(fs::free_block_count(f.d), kFreshFreeBlocks - 2);
}

TEST(fs_alloc_block_is_zeroed) {
    Fs f;
    Block junk;
    junk.fill(0xAB);
    CHECK(fs::write_block(f.d, 18, junk));   // left over from before format
    CHECK_EQ(fs::allocate_block(f.d).value_or(0), 18u);
    const auto b = fs::read_block(f.d, 18);
    CHECK(b && *b == Block{});
}

TEST(fs_alloc_block_takes_a_hole) {
    Fs f;
    only_free(f.d, {9000, 300});
    CHECK_EQ(fs::free_block_count(f.d), 2u);
    CHECK_EQ(fs::allocate_block(f.d).value_or(0), 300u);
    CHECK_EQ(fs::allocate_block(f.d).value_or(0), 9000u);
    CHECK(!fs::allocate_block(f.d).has_value());   // full
    CHECK_EQ(fs::free_block_count(f.d), 0u);
}

TEST(fs_alloc_last_block) {
    Fs f;
    only_free(f.d, {fs::kNumBlocks - 1});
    CHECK_EQ(fs::allocate_block(f.d).value_or(0), fs::kNumBlocks - 1);
    CHECK(!fs::allocate_block(f.d).has_value());
}

TEST(fs_alloc_inodes_lowest_first) {
    Fs f;
    CHECK_EQ(fs::allocate_inode(f.d, InodeType::file).value_or(0), 2u);
    CHECK_EQ(fs::allocate_inode(f.d, InodeType::directory).value_or(0), 3u);
    Inode want;
    want.type = InodeType::directory;
    CHECK(inode(f.d, 3) == want);
    CHECK_EQ(fs::free_inode_count(f.d), kFreshFreeInodes - 2);
}

TEST(fs_alloc_inodes_run_out) {
    Fs f;
    for (uint32_t i = 0; i < kFreshFreeInodes; i++) CHECK(fs::allocate_inode(f.d, InodeType::file).has_value());
    CHECK_EQ(fs::free_inode_count(f.d), 0u);
    CHECK(!fs::allocate_inode(f.d, InodeType::file).has_value());
    CHECK(inode(f.d, 0) == Inode{});   // inode 0 is never handed out
}

// =================== files: contents and blocks ===================

TEST(fs_file_small) {
    Fs f;
    const uint32_t i = ok(fs::create_file(f.d, kRoot, "hello.txt", text("hi there")));
    CHECK_EQ(i, 2u);
    const Inode n = inode(f.d, i);
    CHECK(n.type == InodeType::file);
    CHECK_EQ(n.links, 1u);
    CHECK_EQ(n.size, 8u);
    CHECK_EQ(n.direct[0], 18u);   // the file's block comes first...
    CHECK_EQ(n.direct[1], 0u);
    CHECK_EQ(n.indirect, 0u);
    CHECK_EQ(inode(f.d, kRoot).direct[0], 19u);   // ...then the root's first block
    const auto back = fs::read_file(f.d, i);
    CHECK(back && *back == text("hi there"));
}

TEST(fs_file_rest_of_last_block_is_zero) {
    Fs f;
    const uint32_t i = ok(fs::create_file(f.d, kRoot, "a", text("abc")));
    const auto b = fs::read_block(f.d, inode(f.d, i).direct[0]);
    CHECK(b.has_value());
    if (b) {
        CHECK_EQ((*b)[2], static_cast<uint8_t>('c'));
        CHECK_EQ((*b)[3], 0u);
        CHECK_EQ((*b)[4095], 0u);
    }
}

TEST(fs_file_empty) {
    Fs f;
    const uint32_t i = ok(fs::create_file(f.d, kRoot, "empty", {}));
    const Inode n = inode(f.d, i);
    CHECK_EQ(n.size, 0u);
    CHECK_EQ(n.direct[0], 0u);
    CHECK(!fs::block_of(f.d, n, 0).has_value());
    const auto back = fs::read_file(f.d, i);
    CHECK(back && back->empty());
    CHECK_EQ(fs::free_block_count(f.d), kFreshFreeBlocks - 1);   // only the root's block
}

TEST(fs_file_block_boundaries) {
    Fs f;
    const uint32_t one = ok(fs::create_file(f.d, kRoot, "one", contents(4096)));
    const uint32_t two = ok(fs::create_file(f.d, kRoot, "two", contents(4097)));
    CHECK(fs::block_of(f.d, inode(f.d, one), 0).has_value());
    CHECK(!fs::block_of(f.d, inode(f.d, one), 1).has_value());
    CHECK(fs::block_of(f.d, inode(f.d, two), 1).has_value());
    CHECK(!fs::block_of(f.d, inode(f.d, two), 2).has_value());
    const auto back = fs::read_file(f.d, two);
    CHECK(back && *back == contents(4097));
}

TEST(fs_file_twelve_blocks_need_no_indirect) {
    Fs f;
    const uint32_t i = ok(fs::create_file(f.d, kRoot, "f", contents(12 * 4096)));
    const Inode n = inode(f.d, i);
    CHECK_EQ(n.indirect, 0u);
    for (uint32_t k = 0; k < 12; k++) CHECK_EQ(n.direct[k], 18 + k);
    const auto back = fs::read_file(f.d, i);
    CHECK(back && *back == contents(12 * 4096));
}

TEST(fs_file_thirteen_blocks_use_the_indirect_block) {
    // Blocks 18..29 for file blocks 0..11, 30 for the indirect block, 31 for
    // file block 12, then 32 for the root's entry.
    Fs f;
    const uint32_t i = ok(fs::create_file(f.d, kRoot, "f", contents(12 * 4096 + 1)));
    const Inode n = inode(f.d, i);
    CHECK_EQ(n.direct[11], 29u);
    CHECK_EQ(n.indirect, 30u);
    CHECK_EQ(fs::block_of(f.d, n, 12).value_or(0), 31u);
    CHECK(!fs::block_of(f.d, n, 13).has_value());
    const auto ind = fs::read_block(f.d, 30);
    CHECK(ind.has_value());
    if (ind) {
        CHECK_EQ((*ind)[0], 31u);   // entry 0, little-endian
        CHECK_EQ((*ind)[4], 0u);    // entry 1: none
    }
    CHECK_EQ(inode(f.d, kRoot).direct[0], 32u);
    const auto back = fs::read_file(f.d, i);
    CHECK(back && *back == contents(12 * 4096 + 1));
}

TEST(fs_file_largest) {
    Fs f;
    const size_t max = static_cast<size_t>(fs::kMaxFileBlocks) * fs::kBlockSize;
    const uint32_t i = ok(fs::create_file(f.d, kRoot, "big", contents(max)));
    const Inode n = inode(f.d, i);
    CHECK(fs::block_of(f.d, n, fs::kMaxFileBlocks - 1).has_value());
    const auto back = fs::read_file(f.d, i);
    CHECK(back && *back == contents(max));
    // 1036 data blocks + the indirect block + the root's block
    CHECK_EQ(fs::free_block_count(f.d), kFreshFreeBlocks - fs::kMaxFileBlocks - 2);
}

TEST(fs_file_read_free_or_out_of_range) {
    Fs f;
    CHECK(!fs::read_file(f.d, 5).has_value());              // free
    CHECK(!fs::read_file(f.d, fs::kNumInodes).has_value());
    const auto root = fs::read_file(f.d, kRoot);            // a directory reads as raw entries
    CHECK(root && root->empty());
}

// =================== directory entries ===================

TEST(fs_dir_entry_bytes) {
    Fs f;
    const uint32_t i = ok(fs::create_file(f.d, kRoot, "abc", text("x")));
    CHECK_EQ(inode(f.d, kRoot).size, 32u);
    const auto raw = fs::read_file(f.d, kRoot);
    CHECK(raw && raw->size() == 32);
    if (raw && raw->size() == 32) {
        std::array<uint8_t, fs::kDirEntrySize> e{};
        std::copy(raw->begin(), raw->end(), e.begin());
        CHECK(fs::decode_dir_entry(e) == (DirEntry{i, "abc"}));
    }
}

TEST(fs_dir_lookup_and_list) {
    Fs f;
    const uint32_t a = ok(fs::create_file(f.d, kRoot, "a", text("1")));
    const uint32_t b = ok(fs::create_file(f.d, kRoot, "b", text("2")));
    CHECK_EQ(fs::lookup(f.d, kRoot, "a").value_or(0), a);
    CHECK_EQ(fs::lookup(f.d, kRoot, "b").value_or(0), b);
    CHECK(!fs::lookup(f.d, kRoot, "c").has_value());
    CHECK(!fs::lookup(f.d, kRoot, "A").has_value());   // case matters
    CHECK(fs::list_dir(f.d, kRoot) == (std::vector<DirEntry>{{a, "a"}, {b, "b"}}));
}

TEST(fs_dir_list_of_a_file_is_empty) {
    Fs f;
    const uint32_t a = ok(fs::create_file(f.d, kRoot, "a", text("1")));
    CHECK(fs::list_dir(f.d, a).empty());
    CHECK(!fs::lookup(f.d, a, "a").has_value());
}

TEST(fs_dir_grows_a_block_at_a_time) {
    // 128 entries fill the root's first block; the 129th needs a second.
    Fs f;
    for (int k = 0; k < 128; k++) ok(fs::create_file(f.d, kRoot, "f" + std::to_string(k), {}));
    Inode root = inode(f.d, kRoot);
    CHECK_EQ(root.size, 4096u);
    CHECK(root.direct[0] != 0);
    CHECK_EQ(root.direct[1], 0u);
    ok(fs::create_file(f.d, kRoot, "f128", {}));
    root = inode(f.d, kRoot);
    CHECK_EQ(root.size, 4096u + 32u);
    CHECK(root.direct[1] != 0);
    CHECK_EQ(fs::list_dir(f.d, kRoot).size(), 129u);
    CHECK(fs::lookup(f.d, kRoot, "f128").has_value());
    CHECK(fs::lookup(f.d, kRoot, "f0").has_value());
}

TEST(fs_dir_reuses_an_empty_slot) {
    // Empty the first entry by hand (there is no delete yet): the next file
    // takes that slot, and the directory doesn't grow.
    Fs f;
    ok(fs::create_file(f.d, kRoot, "a", {}));
    const uint32_t b = ok(fs::create_file(f.d, kRoot, "b", {}));
    const uint32_t root_block = inode(f.d, kRoot).direct[0];
    auto blk = fs::read_block(f.d, root_block);
    CHECK(blk.has_value());
    if (!blk) return;
    for (uint32_t k = 0; k < fs::kDirEntrySize; k++) (*blk)[k] = 0;
    CHECK(fs::write_block(f.d, root_block, *blk));
    CHECK(fs::list_dir(f.d, kRoot) == (std::vector<DirEntry>{{b, "b"}}));
    const uint32_t c = ok(fs::create_file(f.d, kRoot, "c", {}));
    CHECK_EQ(inode(f.d, kRoot).size, 64u);
    CHECK(fs::list_dir(f.d, kRoot) == (std::vector<DirEntry>{{c, "c"}, {b, "b"}}));
}

// =================== create_file errors ===================

TEST(fs_create_name_exists) {
    Fs f;
    ok(fs::create_file(f.d, kRoot, "a", text("1")));
    CHECK(is_error(fs::create_file(f.d, kRoot, "a", text("2")), Error::exists));
    CHECK_EQ(fs::list_dir(f.d, kRoot).size(), 1u);
}

TEST(fs_create_bad_names) {
    Fs f;
    CHECK(is_error(fs::create_file(f.d, kRoot, "", {}), Error::bad_name));
    CHECK(is_error(fs::create_file(f.d, kRoot, "a/b", {}), Error::bad_name));
    CHECK(is_error(fs::create_file(f.d, kRoot, std::string(28, 'n'), {}), Error::bad_name));
    CHECK(is_ok(fs::create_file(f.d, kRoot, std::string(27, 'n'), {})));
}

TEST(fs_create_in_something_not_a_directory) {
    Fs f;
    const uint32_t a = ok(fs::create_file(f.d, kRoot, "a", {}));
    CHECK(is_error(fs::create_file(f.d, a, "b", {}), Error::not_a_directory));       // a file
    CHECK(is_error(fs::create_file(f.d, 500, "b", {}), Error::not_a_directory));     // free
    CHECK(is_error(fs::create_file(f.d, fs::kNumInodes, "b", {}), Error::not_a_directory));
    CHECK(is_error(fs::create_file(f.d, a, "", {}), Error::not_a_directory));        // checked first
}

TEST(fs_create_too_big) {
    Fs f;
    const size_t max = static_cast<size_t>(fs::kMaxFileBlocks) * fs::kBlockSize;
    CHECK(is_error(fs::create_file(f.d, kRoot, "big", contents(max + 1)), Error::too_big));
    CHECK_EQ(fs::free_block_count(f.d), kFreshFreeBlocks);
    CHECK_EQ(fs::free_inode_count(f.d), kFreshFreeInodes);
}

TEST(fs_create_no_space_changes_nothing) {
    // One free block: a one-block file also needs a block for the root's entry.
    Fs f;
    only_free(f.d, {100});
    CHECK(is_error(fs::create_file(f.d, kRoot, "a", text("x")), Error::no_space));
    CHECK_EQ(fs::free_block_count(f.d), 1u);
    CHECK_EQ(fs::free_inode_count(f.d), kFreshFreeInodes);
    CHECK_EQ(inode(f.d, kRoot).size, 0u);
    // An empty file needs only the root's block: that fits.
    CHECK(is_ok(fs::create_file(f.d, kRoot, "b", {})));
    CHECK_EQ(fs::free_block_count(f.d), 0u);
}

TEST(fs_create_no_space_counts_the_indirect_block) {
    // 13 file blocks need 14 blocks with the indirect one, + 1 for the root.
    Fs f;
    std::vector<uint32_t> free;
    for (uint32_t b = 100; b < 114; b++) free.push_back(b);   // 14 free
    only_free(f.d, free);
    CHECK(is_error(fs::create_file(f.d, kRoot, "f", contents(12 * 4096 + 1)), Error::no_space));
    CHECK_EQ(fs::free_block_count(f.d), 14u);
    CHECK(is_ok(fs::create_file(f.d, kRoot, "g", contents(12 * 4096))));   // 12 + the root = 13: fits
}

TEST(fs_create_no_inodes_changes_nothing) {
    Fs f;
    while (fs::allocate_inode(f.d, InodeType::file)) {}
    const uint32_t blocks = fs::free_block_count(f.d);
    CHECK(is_error(fs::create_file(f.d, kRoot, "a", text("x")), Error::no_inodes));
    CHECK_EQ(fs::free_block_count(f.d), blocks);
    CHECK_EQ(inode(f.d, kRoot).size, 0u);
}

// =================== directories ===================

TEST(fs_mkdir_makes_an_empty_directory) {
    Fs f;
    const uint32_t bin = ok(fs::make_directory(f.d, kRoot, "bin"));
    Inode want;
    want.type = InodeType::directory;
    want.links = 1;
    CHECK(inode(f.d, bin) == want);
    CHECK_EQ(fs::lookup(f.d, kRoot, "bin").value_or(0), bin);
    CHECK(fs::list_dir(f.d, bin).empty());
}

TEST(fs_mkdir_files_inside) {
    Fs f;
    const uint32_t bin = ok(fs::make_directory(f.d, kRoot, "bin"));
    const uint32_t prog = ok(fs::create_file(f.d, bin, "prog", text("elf")));
    CHECK_EQ(fs::lookup(f.d, bin, "prog").value_or(0), prog);
    CHECK(!fs::lookup(f.d, kRoot, "prog").has_value());   // not in the root
    CHECK(fs::list_dir(f.d, kRoot) == (std::vector<DirEntry>{{bin, "bin"}}));
}

TEST(fs_mkdir_errors) {
    Fs f;
    const uint32_t a = ok(fs::create_file(f.d, kRoot, "a", {}));
    CHECK(is_error(fs::make_directory(f.d, kRoot, "a"), Error::exists));
    CHECK(is_error(fs::make_directory(f.d, a, "x"), Error::not_a_directory));
    CHECK(is_error(fs::make_directory(f.d, kRoot, "x/y"), Error::bad_name));
}

// =================== paths ===================

TEST(fs_path_resolves) {
    Fs f;
    const uint32_t bin = ok(fs::make_directory(f.d, kRoot, "bin"));
    const uint32_t sub = ok(fs::make_directory(f.d, bin, "sub"));
    const uint32_t prog = ok(fs::create_file(f.d, sub, "prog", text("x")));
    const uint32_t top = ok(fs::create_file(f.d, kRoot, "top", text("y")));
    CHECK_EQ(ok(fs::resolve(f.d, "/")), kRoot);
    CHECK_EQ(ok(fs::resolve(f.d, "/bin")), bin);
    CHECK_EQ(ok(fs::resolve(f.d, "/bin/sub/prog")), prog);
    CHECK_EQ(ok(fs::resolve(f.d, "/top")), top);
}

TEST(fs_path_extra_slashes_are_skipped) {
    Fs f;
    const uint32_t bin = ok(fs::make_directory(f.d, kRoot, "bin"));
    const uint32_t prog = ok(fs::create_file(f.d, bin, "prog", {}));
    CHECK_EQ(ok(fs::resolve(f.d, "//bin///prog")), prog);
    CHECK_EQ(ok(fs::resolve(f.d, "/bin/")), bin);
    CHECK_EQ(ok(fs::resolve(f.d, "///")), kRoot);
}

TEST(fs_path_errors) {
    Fs f;
    const uint32_t bin = ok(fs::make_directory(f.d, kRoot, "bin"));
    ok(fs::create_file(f.d, bin, "prog", {}));
    CHECK(is_error(fs::resolve(f.d, ""), Error::bad_name));
    CHECK(is_error(fs::resolve(f.d, "bin/prog"), Error::bad_name));            // relative
    CHECK(is_error(fs::resolve(f.d, "/" + std::string(28, 'n')), Error::bad_name));
    CHECK(is_error(fs::resolve(f.d, "/nope"), Error::not_found));
    CHECK(is_error(fs::resolve(f.d, "/bin/nope"), Error::not_found));
    CHECK(is_error(fs::resolve(f.d, "/bin/prog/x"), Error::not_a_directory));  // prog is a file
}
