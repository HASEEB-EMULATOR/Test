#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace haseeb::loader {

struct Ext4DirEntry {
    uint32_t inode = 0;
    uint8_t file_type = 0; // 1=REG, 2=DIR, 7=SYMLINK
    std::string name;
};

struct Ext4Stat {
    uint32_t inode = 0;
    uint16_t mode = 0;
    uint64_t size = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t links_count = 0;

    bool IsRegular() const { return (mode & 0xF000u) == 0x8000u; }
    bool IsDirectory() const { return (mode & 0xF000u) == 0x4000u; }
    bool IsSymlink() const { return (mode & 0xF000u) == 0xA000u; }
};

// Mount-free, read-only raw ext4 image parser supporting 4K blocks, 64-bit group descriptors,
// extent trees (depth 0..5), indirect blocks, fast/block symlinks, and directory traversal.
class Ext4Reader {
public:
    Ext4Reader() = default;
    ~Ext4Reader();

    Ext4Reader(const Ext4Reader&) = delete;
    Ext4Reader& operator=(const Ext4Reader&) = delete;

    bool Open(const std::string& image_path);
    void Close();

    bool IsOpen() const { return fd_ >= 0; }
    uint32_t BlockSize() const { return block_size_; }
    uint64_t ImageSizeBytes() const { return image_size_; }
    const std::string& VolumeName() const { return volume_name_; }
    const std::string& LastError() const { return last_error_; }

    bool Stat(const std::string& guest_path, Ext4Stat& out_stat) const;
    bool Exists(const std::string& guest_path) const;
    bool ReadSymlink(const std::string& guest_path, std::string& out_target) const;
    bool ReadDir(const std::string& guest_path, std::vector<Ext4DirEntry>& out_entries) const;
    bool ReadFile(const std::string& guest_path, std::vector<uint8_t>& out_data) const;
    bool ReadFileString(const std::string& guest_path, std::string& out_text) const;

private:
    struct RawInode;

    bool ReadBytesAt(uint64_t offset, void* dst, size_t len) const;
    bool ReadBlock(uint64_t block_num, void* dst) const;
    bool ReadInode(uint32_t inode_num, RawInode& out_inode) const;
    bool ReadInodeData(const RawInode& inode, std::vector<uint8_t>& out_data) const;
    bool ReadInodeBlock(const RawInode& inode, uint32_t logical_block, uint64_t& out_phys_block) const;
    bool SearchExtentTree(const uint8_t* node_bytes, uint32_t logical_block, uint64_t& out_phys_block) const;
    bool ReadSymlinkFromInode(const RawInode& inode, std::string& out_target) const;
    bool ReadDirFromInode(const RawInode& inode, std::vector<Ext4DirEntry>& out_entries) const;
    bool ResolvePathInode(const std::string& guest_path,
                          uint32_t& out_inode_num,
                          RawInode& out_inode,
                          bool follow_final_symlink = true,
                          int depth = 0) const;

    int fd_ = -1;
    uint64_t image_size_ = 0;
    uint32_t block_size_ = 4096;
    uint32_t first_data_block_ = 0;
    uint32_t blocks_per_group_ = 0;
    uint32_t inodes_per_group_ = 0;
    uint32_t inodes_count_ = 0;
    uint64_t blocks_count_ = 0;
    uint16_t inode_size_ = 256;
    uint16_t desc_size_ = 32;
    uint32_t feature_incompat_ = 0;
    uint32_t group_count_ = 0;
    uint64_t bgdt_offset_ = 0;
    std::string volume_name_;
    mutable std::string last_error_;
};

} // namespace haseeb::loader
