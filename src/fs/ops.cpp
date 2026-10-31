// Host-side file-system operations (docs/machine-spec.md, C1).
// TODO: every function below is a stub so the project builds. Replace them.
#include "fs/ops.hpp"

#include <algorithm>
#include <cassert>

#include "util/bytes.hpp"

namespace fs {

using util::get32;
using util::put32;

namespace {
std::vector<Inode> read_all_inodes(const disk::Disk& d) {
    std::vector<Inode> inodes;
    inodes.reserve(kNumInodes);
    for (uint32_t block_num = kInodeStart; block_num < kInodeStart + kInodeBlocks; ++block_num) {
        auto readed = read_block(d, block_num);
        assert(readed);
        for (uint32_t i = 0; i < kInodesPerBlock; ++i) {
            uint32_t inode_num = (block_num - kInodeStart) * kInodesPerBlock + i;
            if (inode_num >= kNumInodes) {
                break;
            }
            std::span<const uint8_t, kInodeSize> inode_bytes(readed->data() + i * kInodeSize, kInodeSize);
            inodes.push_back(decode_inode(inode_bytes));
        }
    }
    return inodes;
}

bool block_in_use(const Block& block, uint32_t block_num) { 
    assert(block_num < kNumBlocks);
    uint8_t byte = block[block_num / 8];
    return (byte >> (block_num % 8)) & 1;
}

void set_block_in_use(disk::Disk& disk, Block& block, uint32_t block_num) { 
    assert(block_num < kNumBlocks);
    block[block_num / 8] |= 1 << (block_num % 8);
    write_block(disk, kBitmapBlock, block);
}


}  // namespace

std::optional<uint32_t> allocate_block(disk::Disk& disk) { 
    auto readed = read_block(disk, kBitmapBlock);
    assert(readed);
    for (uint32_t b = 0; b < kNumBlocks; ++b) {
        if (!block_in_use(*readed, b)) {
            set_block_in_use(disk, *readed, b);
            Block zero_block{};
            write_block(disk, b, zero_block);
            return b;
        }
    } 
    return std::nullopt;
}

bool block_in_use(const disk::Disk& disk, uint32_t block_num) { 
    assert(block_num < kNumBlocks);
    auto readed = read_block(disk, kBitmapBlock);
    assert(readed);
    uint8_t byte = (*readed)[block_num / 8];
    return (byte >> (block_num % 8)) & 1;
}

uint32_t free_block_count(const disk::Disk& disk) { 
    uint32_t count = 0;
    auto readed = read_block(disk, kBitmapBlock);
    assert(readed);
    for (uint32_t i = 0; i < kNumBlocks; ++i) {
        count += !block_in_use(*readed, i);
    }
    return count;
}

std::optional<uint32_t> allocate_inode(disk::Disk& disk, InodeType type) { 
    std::vector<Inode> inodes = read_all_inodes(disk);
    for (uint32_t i = 1; i < kNumInodes; ++i) {
        if (inodes[i].type == InodeType::free) {
            Inode new_inode{};
            new_inode.type = type;
            write_inode(disk, i, new_inode);
            return i;
        }
    }
    return std::nullopt;
}

uint32_t free_inode_count(const disk::Disk& disk) { 
    uint32_t count = 0; 
    std::vector<Inode> inodes = read_all_inodes(disk);
    for (uint32_t i = 1; i < kNumInodes; ++i) {
        count += (inodes[i].type == InodeType::free);
    }
    return count;
}

std::optional<uint32_t> block_of(const disk::Disk& disk, const Inode& inode, uint32_t k) { 
    uint32_t file_size = inode.size / kBlockSize + (inode.size % kBlockSize != 0 ? 1 : 0);
    if (k >= file_size) {
        return std::nullopt;
    }
    if (k < kNumDirect) {
        return inode.direct[k];
    }
    auto readed = read_block(disk, inode.indirect);
    assert(readed);
    return get32(*readed, (k - kNumDirect) * 4);
}

std::optional<std::vector<uint8_t>> read_file(const disk::Disk& disk, uint32_t inode_num) { 
    if (inode_num >= kNumInodes) {
        return std::nullopt;
    }
    Inode inode = *read_inode(disk, inode_num);
    if (inode.type == InodeType::free) {
        return std::nullopt;
    }
    std::vector<uint8_t> file_data;
    file_data.reserve(inode.size);
    uint32_t k = 0;
    uint32_t bytes_remaining = inode.size;
    while (auto block = block_of(disk, inode, k)) {
        uint32_t reading_bytes = std::min(bytes_remaining, kBlockSize);
        auto readed = read_block(disk, *block);
        assert(readed);
        file_data.insert(file_data.end(), readed->begin(), readed->begin() + reading_bytes);
        bytes_remaining -= reading_bytes;
        ++k;
    }
    return file_data;
}

std::vector<DirEntry> list_dir(const disk::Disk& disk, uint32_t inode_num) { 
    Inode inode = *read_inode(disk, inode_num);
    if (inode.type != InodeType::directory) {
        return {};
    }
    auto file_data = read_file(disk, inode_num);
    assert(file_data);
    assert(inode.size % kDirEntrySize == 0);
    std::vector<DirEntry> entries;
    entries.reserve(inode.size / kDirEntrySize);
    for (size_t offset = 0; offset < file_data->size(); offset += kDirEntrySize) {
        std::span<const uint8_t, kDirEntrySize> entry_bytes(file_data->data() + offset, kDirEntrySize);
        DirEntry entry = decode_dir_entry(entry_bytes);
        if (entry.inode != 0) {
            entries.push_back(entry);
        }
    }
    return entries;
}

std::optional<uint32_t> lookup(const disk::Disk& disk, uint32_t inode_num, std::string_view name) { 
    auto entries = list_dir(disk, inode_num);
    for (const auto& entry : entries) {
        if (entry.name == name) {
            return entry.inode;
        }
    }
    return std::nullopt; 
}

std::variant<uint32_t, Error> resolve(const disk::Disk& disk, std::string_view path) { 
    if (path.empty() || path[0] != '/') {
        return Error::bad_name;
    }
    uint32_t current_inode = kRootInode; // root inode
    std::size_t pos = 1;
    while (pos < path.size()) {
        size_t next_slash = path.find('/', pos);
        if (next_slash == pos) {
            ++pos;
            continue;
        }
        std::string_view name = (next_slash == std::string_view::npos) ? path.substr(pos) : path.substr(pos, next_slash - pos);
        if (!valid_name(name)) {
            return Error::bad_name;
        }
        if (read_inode(disk, current_inode)->type != InodeType::directory) {
            return Error::not_a_directory;
        }
        auto next_inode = lookup(disk, current_inode, name);
        if (!next_inode) {
            return Error::not_found;
        }
        current_inode = *next_inode;
        pos = (next_slash == std::string_view::npos) ? path.size() : next_slash + 1;
    }
    return current_inode;
}

namespace {

// Takes a new block and makes it file block k of n. File block 12, the first
// in the indirect block, takes the indirect block first (ops.hpp, create_file).
uint32_t add_block(disk::Disk& d, Inode& n, uint32_t k) {
    if (k == kNumDirect) n.indirect = *allocate_block(d);
    const uint32_t b = *allocate_block(d);
    if (k < kNumDirect) {
        n.direct[k] = b;
    } else {
        Block ind = *read_block(d, n.indirect);
        put32(ind, 4 * (k - kNumDirect), b);
        write_block(d, n.indirect, ind);
    }
    return b;
}

// Byte offset of dir's first empty entry, or its size if it has none.
uint32_t free_slot(const disk::Disk& d, uint32_t dir) {
    const std::vector<uint8_t> bytes = *read_file(d, dir);
    for (uint32_t off = 0; off < bytes.size(); off += kDirEntrySize)
        if (get32(bytes, off) == 0) return off;
    return static_cast<uint32_t>(bytes.size());
}

// create_file and make_directory: look first, act only once nothing can fail.
std::variant<uint32_t, Error> create(disk::Disk& d, uint32_t dir, std::string_view name, InodeType type,
                                     std::span<const uint8_t> contents) {
    // look
    std::optional<Inode> parent = read_inode(d, dir);
    if (!parent || parent->type != InodeType::directory) return Error::not_a_directory;
    if (!valid_name(name)) return Error::bad_name;
    if (lookup(d, dir, name)) return Error::exists;
    const size_t nblocks = (contents.size() + kBlockSize - 1) / kBlockSize;
    if (nblocks > kMaxFileBlocks) return Error::too_big;
    if (free_inode_count(d) == 0) return Error::no_inodes;
    const uint32_t slot = free_slot(d, dir);
    const uint32_t dir_block = slot / kBlockSize;
    const bool grow_dir = slot == parent->size && slot % kBlockSize == 0;
    const size_t needed = nblocks + (nblocks > kNumDirect) + grow_dir + (grow_dir && dir_block == kNumDirect);
    if (free_block_count(d) < needed) return Error::no_space;

    // act: the new inode and its blocks, in file order
    const uint32_t i = *allocate_inode(d, type);
    Inode n{type, 1, static_cast<uint32_t>(contents.size()), {}, 0};
    for (uint32_t k = 0; k < nblocks; k++) {
        const auto chunk = contents.subspan(k * kBlockSize, std::min<size_t>(kBlockSize, contents.size() - k * kBlockSize));
        Block b{};
        std::copy(chunk.begin(), chunk.end(), b.begin());
        write_block(d, add_block(d, n, k), b);
    }
    write_inode(d, i, n);

    // act: the entry, in the free slot or at the end of the directory
    if (grow_dir) add_block(d, *parent, dir_block);
    if (slot == parent->size) parent->size += kDirEntrySize;
    write_inode(d, dir, *parent);
    const uint32_t b = *block_of(d, *parent, dir_block);
    Block blk = *read_block(d, b);
    const auto entry = encode_dir_entry({i, std::string(name)});
    std::copy(entry.begin(), entry.end(), blk.begin() + slot % kBlockSize);
    write_block(d, b, blk);
    return i;
}

}  // namespace

std::variant<uint32_t, Error> create_file(disk::Disk& d, uint32_t dir, std::string_view name,
                                          std::span<const uint8_t> contents) {
    return create(d, dir, name, InodeType::file, contents);
}

std::variant<uint32_t, Error> make_directory(disk::Disk& d, uint32_t dir, std::string_view name) {
    return create(d, dir, name, InodeType::directory, {});
}

}  // namespace fs
