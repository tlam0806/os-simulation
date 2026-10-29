// The file system's on-disk layout (docs/machine-spec.md, C1), and host-side
// tools to build and inspect a disk image, like mkfs on Linux. Host access is
// untimed (Disk::host_read / host_write); the kernel's timed path comes later
// and reuses the encode / decode functions.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "disk/disk.hpp"



namespace fs {



// ---- sizes and places (C1) ----

inline constexpr uint32_t kBlockSize = 4096;                                // one page
inline constexpr uint32_t kSectorsPerBlock = kBlockSize / disk::kSectorSize; // 8
inline constexpr uint32_t kNumBlocks = disk::kNumSectors / kSectorsPerBlock; // 16384

inline constexpr uint32_t kMagic = 0x5346534F;   // the bytes "OSFS"
inline constexpr uint32_t kSuperblock = 0;
inline constexpr uint32_t kBitmapBlock = 1;
inline constexpr uint32_t kInodeStart = 2;
inline constexpr uint32_t kInodeBlocks = 16;
inline constexpr uint32_t kDataStart = kInodeStart + kInodeBlocks;          // 18

inline constexpr uint32_t kInodeSize = 64;
inline constexpr uint32_t kInodesPerBlock = kBlockSize / kInodeSize;        // 64
inline constexpr uint32_t kNumInodes = kInodeBlocks * kInodesPerBlock;      // 1024
inline constexpr uint32_t kRootInode = 1;

inline constexpr uint32_t kNumDirect = 12;
inline constexpr uint32_t kPointersPerBlock = kBlockSize / 4;              // 1024
inline constexpr uint32_t kMaxFileBlocks = kNumDirect + kPointersPerBlock;  // 1036

inline constexpr uint32_t kDirEntrySize = 32;
inline constexpr uint32_t kMaxNameLength = 27;

using Block = std::array<uint8_t, kBlockSize>;

// ---- the records, decoded ----

struct Superblock {
    uint32_t magic = 0;
    uint32_t block_count = 0;
    uint32_t inode_count = 0;
    uint32_t bitmap_start = 0;
    uint32_t inode_start = 0;
    uint32_t inode_blocks = 0;
    uint32_t data_start = 0;

    bool operator==(const Superblock&) const = default;
};

enum class InodeType : uint16_t { free = 0, file = 1, directory = 2 };

struct Inode {
    InodeType type = InodeType::free;
    uint16_t links = 0;
    uint32_t size = 0;
    std::array<uint32_t, kNumDirect> direct{};   // 0 = no block
    uint32_t indirect = 0;                       // 0 = no indirect block

    bool operator==(const Inode&) const = default;
};

struct DirEntry {
    uint32_t inode = 0;   // 0 = empty entry
    std::string name;

    bool operator==(const DirEntry&) const = default;
};

// ---- byte encoding (little-endian, offsets as in C1) ----

// The superblock as the first 28 bytes of a block; the rest of the block is 0.
Block encode_superblock(const Superblock& sb);
Superblock decode_superblock(std::span<const uint8_t, kBlockSize> block);

// One inode as its 64 bytes. Bytes 60..63 are 0.
std::array<uint8_t, kInodeSize> encode_inode(const Inode& inode);
Inode decode_inode(std::span<const uint8_t, kInodeSize> bytes);

// A name is valid if it is 1..kMaxNameLength bytes with no '/' and no zero byte.
bool valid_name(std::string_view name);

// One directory entry as its 32 bytes: the inode number, then the name padded
// with zero bytes. The name must be valid (or empty, for an empty entry).
std::array<uint8_t, kDirEntrySize> encode_dir_entry(const DirEntry& entry);
// The name is the bytes before the first zero byte.
DirEntry decode_dir_entry(std::span<const uint8_t, kDirEntrySize> bytes);

// ---- host access to a disk image (untimed) ----

// Block b, or nullopt if b >= kNumBlocks.
std::optional<Block> read_block(const disk::Disk& d, uint32_t b);
// Writes block b. Returns false, and writes nothing, if b >= kNumBlocks.
bool write_block(disk::Disk& d, uint32_t b, const Block& block);

// Inode i, or nullopt if i >= kNumInodes.
std::optional<Inode> read_inode(const disk::Disk& d, uint32_t i);
// Writes inode i's 64 bytes, leaving the other inodes in its block alone.
// Returns false, and writes nothing, if i >= kNumInodes.
bool write_inode(disk::Disk& d, uint32_t i, const Inode& inode);

// Makes an empty file system (C1), whatever the disk held before:
//   - the superblock
//   - the bitmap: blocks 0..kDataStart-1 in use, everything else free
//   - the inode table: every inode free, except the root (inode 1): an empty
//     directory, size 0, no blocks, links 1
// Data blocks are left as they were: they are free, so nobody reads them.
void format(disk::Disk& d);

// True if block 0 starts with kMagic.
bool is_formatted(const disk::Disk& d);

}  // namespace fs
