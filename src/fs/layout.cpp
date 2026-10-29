// The file system's on-disk layout (docs/machine-spec.md, C1).
// TODO: every function below is a stub so the project builds. Replace them.
#include "fs/layout.hpp"

namespace fs {

namespace {
    uint16_t get16(std::span<const uint8_t> f, size_t at) {
    return static_cast<uint16_t>(f[at] | f[at + 1] << 8);
    }

    uint32_t get32(std::span<const uint8_t> f, size_t at) {
        return uint32_t{f[at]} | uint32_t{f[at + 1]} << 8 | uint32_t{f[at + 2]} << 16 |
            uint32_t{f[at + 3]} << 24;
    }
    void put16(std::span<uint8_t> f, size_t at, uint16_t value) {
        f[at] = static_cast<uint8_t>(value);
        f[at + 1] = static_cast<uint8_t>(value >> 8);
    }
    void put32(std::span<uint8_t> f, size_t at, uint32_t value) {
        f[at] = static_cast<uint8_t>(value);
        f[at + 1] = static_cast<uint8_t>(value >> 8);
        f[at + 2] = static_cast<uint8_t>(value >> 16);
        f[at + 3] = static_cast<uint8_t>(value >> 24);
    }
}

Block encode_superblock(const Superblock& sp) { 
    Block block{};
    put32(block, 0, sp.magic);
    put32(block, 4, sp.block_count);
    put32(block, 8, sp.inode_count);
    put32(block, 12, sp.bitmap_start);
    put32(block, 16, sp.inode_start);
    put32(block, 20, sp.inode_blocks);
    put32(block, 24, sp.data_start);
    return block;
}   

Superblock decode_superblock(std::span<const uint8_t, kBlockSize> block) {
    Superblock sb;
    sb.magic = get32(block, 0);
    sb.block_count = get32(block, 4);
    sb.inode_count = get32(block, 8);
    sb.bitmap_start = get32(block, 12);
    sb.inode_start = get32(block, 16);
    sb.inode_blocks = get32(block, 20);
    sb.data_start = get32(block, 24);
    return sb;
}

// struct Inode {
//     InodeType type = InodeType::free;
//     uint16_t links = 0;
//     uint32_t size = 0;
//     std::array<uint32_t, kNumDirect> direct{};   // 0 = no block
//     uint32_t indirect = 0;                       // 0 = no indirect block

//     bool operator==(const Inode&) const = default;
// };

std::array<uint8_t, kInodeSize> encode_inode(const Inode& inode) { 
    std::array<uint8_t, kInodeSize> bytes{};
    put16(bytes, 0, static_cast<uint16_t>(inode.type));
    put16(bytes, 2, inode.links);
    put32(bytes, 4, inode.size);
    for (size_t i = 0; i < kNumDirect; ++i) {
        put32(bytes, 8 + i * 4, inode.direct[i]);
    }
    put32(bytes, 56, inode.indirect);
    return bytes;
}

Inode decode_inode(std::span<const uint8_t, kInodeSize> bytes) { 
    Inode inode;
    inode.type = static_cast<InodeType>(get16(bytes, 0));
    inode.links = get16(bytes, 2);
    inode.size = get32(bytes, 4);
    for (size_t i = 0; i < kNumDirect; ++i) {
        inode.direct[i] = get32(bytes, 8 + i * 4);
    }
    inode.indirect = get32(bytes, 56);
    return inode;
}

bool valid_name(std::string_view name) { 
    if (name.empty()) {
        return false;
    }
    if (name.size() > kMaxNameLength) {
        return false;
    }
    if (name.find('/') != std::string_view::npos) {
        return false;
    }
    if (name.find('\0') != std::string_view::npos) {
        return false;
    }
    return true;
}

std::array<uint8_t, kDirEntrySize> encode_dir_entry(const DirEntry& entry) { 
    std::array<uint8_t, kDirEntrySize> bytes{};
    put32(bytes, 0, entry.inode);
    for (size_t i = 0; i < entry.name.size(); ++i) {
        bytes[4 + i] = static_cast<uint8_t>(entry.name[i]);
    }
    return bytes;
}

DirEntry decode_dir_entry(std::span<const uint8_t, kDirEntrySize> bytes) { 
    DirEntry entry;
    entry.inode = get32(bytes, 0);
    size_t name_length = 0;
    while (name_length < kMaxNameLength && bytes[4 + name_length] != 0) {
        ++name_length;
    }
    entry.name = std::string(reinterpret_cast<const char*>(&bytes[4]), name_length);
    return entry;
}

std::optional<Block> read_block(const disk::Disk& disk, uint32_t block_num) { 
    if (block_num >= kNumBlocks) {
        return std::nullopt;
    }
    uint32_t sector_num = block_num * kSectorsPerBlock;
    std::optional<std::vector<uint8_t>> sectors = disk.host_read(sector_num, kSectorsPerBlock);
    if (!sectors) {
        return std::nullopt;
    }
    Block block{};
    std::copy(sectors->begin(), sectors->end(), block.begin());
    return block;
}


bool write_block(disk::Disk& disk, uint32_t block_num, const Block& block) { 
    if (block_num >= kNumBlocks) {
        return false;
    }
    uint32_t sector_num = block_num * kSectorsPerBlock;
    auto written = disk.host_write(sector_num, block);
    return written;
}

std::optional<Inode> read_inode(const disk::Disk& disk, uint32_t inode_num) { 
    if (inode_num >= kNumInodes) {
        return std::nullopt;
    }
    uint32_t inodes_per_block = kBlockSize / kInodeSize;
    uint32_t block_num = inode_num / inodes_per_block + kInodeStart;
    uint32_t offset = (inode_num % inodes_per_block) * kInodeSize;
    std::optional<Block> block = read_block(disk, block_num);
    if (!block) {
        return std::nullopt;
    }
    std::span<const uint8_t, kInodeSize> inode_bytes(block->data() + offset, kInodeSize);
    Inode inode = decode_inode(inode_bytes);
    return inode;
}

bool write_inode(disk::Disk& disk, uint32_t inode_num, const Inode& inode) { 
    if (inode_num >= kNumInodes) {
        return false;
    }
    uint32_t inodes_per_block = kBlockSize / kInodeSize;
    uint32_t block_num = inode_num / inodes_per_block + kInodeStart; 
    uint32_t offset = (inode_num % inodes_per_block) * kInodeSize;
    std::optional<Block> block = read_block(disk, block_num);
    if (!block) {
        return false;
    }
    std::array<uint8_t, kInodeSize> inode_bytes = encode_inode(inode);
    std::copy(inode_bytes.begin(), inode_bytes.end(), block->begin() + offset);
    return write_block(disk, block_num, *block);
}

void format(disk::Disk& disk) {
    Superblock sb{
        kMagic,
        kNumBlocks,
        kNumInodes,
        kBitmapBlock,
        kInodeStart,
        kInodeBlocks,
        kDataStart
    };
    write_block(disk, kSuperblock, encode_superblock(sb));

    Block bitmap_block{};
    for (uint32_t i = 0; i < kDataStart; ++i) {
        bitmap_block[i / 8] |= 1 << (i % 8);
    }
    write_block(disk, kBitmapBlock, bitmap_block);

    
    Inode empty_inode{};
    for (uint32_t i = 0; i < kNumInodes; ++i) {
        write_inode(disk, i, empty_inode);
    }
    
    Inode root_inode{
        InodeType::directory,
        1,
        0,
        {},
        0
    };
    write_inode(disk, kRootInode, root_inode);

}

bool is_formatted(const disk::Disk& disk) { 
    auto readed = read_block(disk, kSuperblock);
    if (!readed) {
        return false;
    }
    Superblock sb = decode_superblock(*readed);
    return sb.magic == kMagic;
}

}  // namespace fs
