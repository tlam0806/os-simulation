// Host-side file-system operations on a disk image (docs/machine-spec.md, C1),
// like mkfs / debugfs on Linux: tests use them to put files on a disk before
// the kernel runs, and to check what is on it afterwards. Untimed, through
// Disk::host_read / host_write (via read_block / write_block in layout.hpp).
//
// Every function expects a formatted disk.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "disk/disk.hpp"
#include "fs/layout.hpp"

namespace fs {

enum class Error {
    bad_name,          // not a valid name (layout.hpp, valid_name), or a bad path
    not_found,         // no entry with that name
    not_a_directory,   // the inode should be a directory and isn't (or is free)
    exists,            // the name is already in the directory
    too_big,           // more than kMaxFileBlocks blocks
    no_space,          // not enough free blocks
    no_inodes,         // no free inode
};

// ---- free space (the bitmap) ----

// Takes the lowest-numbered free block: marks it in use in the bitmap and
// fills it with zeros. nullopt if every block is in use.
std::optional<uint32_t> allocate_block(disk::Disk& d);

// Whether block b's bitmap bit is set.
bool block_in_use(const disk::Disk& d, uint32_t b);

// How many of the kNumBlocks blocks are free.
uint32_t free_block_count(const disk::Disk& d);

// ---- inodes ----

// Takes the lowest-numbered free inode (never inode 0): writes an inode of
// this type with everything else 0 (links 0, size 0, no blocks). nullopt if
// none is free.
std::optional<uint32_t> allocate_inode(disk::Disk& d, InodeType type);

// How many inodes are free, not counting inode 0.
uint32_t free_inode_count(const disk::Disk& d);

// ---- file contents ----

// The disk block that holds file block k of this inode: direct[k] for k < 12,
// else entry k - 12 of the indirect block. nullopt if the file has no block k
// (k >= ceil(size / kBlockSize)).
std::optional<uint32_t> block_of(const disk::Disk& d, const Inode& inode, uint32_t k);

// The size bytes of inode i (a file, or a directory's raw entries). nullopt
// if i is out of range or free.
std::optional<std::vector<uint8_t>> read_file(const disk::Disk& d, uint32_t i);

// ---- directories ----

// The non-empty entries of directory dir, in the order they appear. Empty if
// dir isn't a directory.
std::vector<DirEntry> list_dir(const disk::Disk& d, uint32_t dir);

// The inode named `name` in directory dir, or nullopt.
std::optional<uint32_t> lookup(const disk::Disk& d, uint32_t dir, std::string_view name);

// The inode a path names. Paths are absolute: they start with '/', and the
// names between slashes are looked up one directory at a time from the root.
// Empty names (as in "//a" or "a/") are skipped, so "/" is the root.
//   bad_name:        empty, doesn't start with '/', or a name isn't valid
//   not_found:       a name isn't in its directory
//   not_a_directory: a name before the last one is a file
std::variant<uint32_t, Error> resolve(const disk::Disk& d, std::string_view path);

// ---- creating (L11, "Walkthrough on file operation: Create") ----

// Creates a regular file named `name` in directory dir holding `contents`,
// and returns its inode number. The new inode: type file, links 1, size =
// contents.size().
//
// Blocks are taken with allocate_block, in this order:
//   1. the file's blocks in file order, with the indirect block (if more
//      than 12 blocks) taken just before file block 12
//   2. a new block for the directory, if the entry needs one (below)
// The directory entry goes into the first empty slot of dir. With no empty
// slot, it is added at the end: dir's size grows by kDirEntrySize, and when
// the old size is a multiple of kBlockSize the directory gets a new block.
//
// Errors, checked in this order. On any error the disk is unchanged.
//   not_a_directory: dir is out of range, free, or a regular file
//   bad_name:        !valid_name(name)
//   exists:          dir already has an entry with this name
//   too_big:         more than kMaxFileBlocks blocks
//   no_inodes:       no free inode
//   no_space:        fewer free blocks than all the blocks above need
std::variant<uint32_t, Error> create_file(disk::Disk& d, uint32_t dir, std::string_view name,
                                          std::span<const uint8_t> contents);

// Creates an empty directory named `name` in directory dir, like the root
// after format: type directory, links 1, size 0, no blocks. Same entry rules
// and errors as create_file (too_big can't happen).
std::variant<uint32_t, Error> make_directory(disk::Disk& d, uint32_t dir, std::string_view name);

}  // namespace fs
