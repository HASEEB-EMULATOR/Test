#include "haseeb/ext4_reader.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace haseeb::loader {

namespace {

inline constexpr uint16_t kExt4SuperMagic = 0xEF53u;
inline constexpr uint16_t kExt4ExtentMagic = 0xF30Au;
inline constexpr uint32_t kExt4RootIno = 2u;
inline constexpr uint32_t kExt4ExtentsFl = 0x00080000u;
inline constexpr uint32_t kExt4FeatureIncompat64Bit = 0x0080u;
inline constexpr uint64_t kExt4SuperblockOffset = 1024ull;

#pragma pack(push, 1)
struct Ext4SuperBlock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16];
    char s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t s_prealloc_blocks;
    uint8_t s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t s_def_hash_version;
    uint8_t s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
};

struct Ext4GroupDesc {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
};

struct Ext4ExtentHeader {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
};

struct Ext4ExtentIdx {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
};

struct Ext4Extent {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
};

struct Ext4DirEntry2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[255];
};
#pragma pack(pop)

std::vector<std::string> SplitPath(const std::string& p) {
    std::vector<std::string> out;
    std::istringstream iss(p);
    std::string seg;
    while (std::getline(iss, seg, '/')) {
        if (!seg.empty() && seg != ".") {
            out.push_back(seg);
        }
    }
    return out;
}

} // namespace

struct Ext4Reader::RawInode {
    uint16_t i_mode = 0;
    uint16_t i_uid = 0;
    uint32_t i_size_lo = 0;
    uint32_t i_atime = 0;
    uint32_t i_ctime = 0;
    uint32_t i_mtime = 0;
    uint32_t i_dtime = 0;
    uint16_t i_gid = 0;
    uint16_t i_links_count = 0;
    uint32_t i_blocks_lo = 0;
    uint32_t i_flags = 0;
    uint32_t i_osd1 = 0;
    uint32_t i_block[15]{};
    uint32_t i_generation = 0;
    uint32_t i_file_acl_lo = 0;
    uint32_t i_size_high = 0;
    uint32_t i_obso_faddr = 0;
    uint8_t i_osd2[12]{};

    uint64_t Size() const {
        return static_cast<uint64_t>(i_size_lo) | (static_cast<uint64_t>(i_size_high) << 32);
    }
    bool UsesExtents() const { return (i_flags & kExt4ExtentsFl) != 0u; }
    bool IsDirectory() const { return (i_mode & 0xF000u) == 0x4000u; }
    bool IsRegular() const { return (i_mode & 0xF000u) == 0x8000u; }
    bool IsSymlink() const { return (i_mode & 0xF000u) == 0xA000u; }
};

Ext4Reader::~Ext4Reader() {
    Close();
}

void Ext4Reader::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    image_size_ = 0;
}

bool Ext4Reader::ReadBytesAt(uint64_t offset, void* dst, size_t len) const {
    if (fd_ < 0 || offset + len > image_size_) return false;
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::pread(fd_, p + done, len - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool Ext4Reader::ReadBlock(uint64_t block_num, void* dst) const {
    if (block_num == 0ull) {
        std::memset(dst, 0, block_size_);
        return true;
    }
    return ReadBytesAt(block_num * static_cast<uint64_t>(block_size_), dst, block_size_);
}

bool Ext4Reader::Open(const std::string& image_path) {
    Close();
    int fd = ::open(image_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        last_error_ = "open failed: " + std::string(std::strerror(errno));
        return false;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 4096) {
        last_error_ = "invalid ext4 image size";
        ::close(fd);
        return false;
    }

    fd_ = fd;
    image_size_ = static_cast<uint64_t>(st.st_size);

    Ext4SuperBlock sb{};
    if (!ReadBytesAt(kExt4SuperblockOffset, &sb, sizeof(sb))) {
        last_error_ = "failed to read ext4 superblock";
        Close();
        return false;
    }
    if (sb.s_magic != kExt4SuperMagic) {
        last_error_ = "not a raw ext4 image (bad magic; convert sparse images first)";
        Close();
        return false;
    }
    if (sb.s_log_block_size > 6u) {
        last_error_ = "unsupported ext4 block size shift";
        Close();
        return false;
    }

    block_size_ = 1024u << sb.s_log_block_size;
    first_data_block_ = sb.s_first_data_block;
    blocks_per_group_ = sb.s_blocks_per_group;
    inodes_per_group_ = sb.s_inodes_per_group;
    inodes_count_ = sb.s_inodes_count;
    feature_incompat_ = sb.s_feature_incompat;
    inode_size_ = (sb.s_rev_level >= 1u && sb.s_inode_size >= 128u) ? sb.s_inode_size : 128u;
    desc_size_ = ((feature_incompat_ & kExt4FeatureIncompat64Bit) != 0u && sb.s_desc_size >= 64u)
        ? sb.s_desc_size
        : 32u;

    blocks_count_ = static_cast<uint64_t>(sb.s_blocks_count_lo);
    if ((feature_incompat_ & kExt4FeatureIncompat64Bit) != 0u) {
        blocks_count_ |= (static_cast<uint64_t>(sb.s_blocks_count_hi) << 32);
    }
    if (blocks_per_group_ == 0 || inodes_per_group_ == 0) {
        last_error_ = "invalid ext4 group geometry";
        Close();
        return false;
    }

    uint64_t data_blocks = blocks_count_ - first_data_block_;
    group_count_ = static_cast<uint32_t>((data_blocks + blocks_per_group_ - 1ull) / blocks_per_group_);
    bgdt_offset_ = static_cast<uint64_t>(first_data_block_ + 1u) * static_cast<uint64_t>(block_size_);

    char vol[17]{};
    std::memcpy(vol, sb.s_volume_name, 16);
    volume_name_ = vol;
    return true;
}

bool Ext4Reader::ReadInode(uint32_t inode_num, RawInode& out_inode) const {
    if (inode_num == 0 || inode_num > inodes_count_) return false;
    uint32_t group = (inode_num - 1u) / inodes_per_group_;
    uint32_t index = (inode_num - 1u) % inodes_per_group_;
    if (group >= group_count_) return false;

    Ext4GroupDesc gd{};
    uint64_t gd_off = bgdt_offset_ + static_cast<uint64_t>(group) * static_cast<uint64_t>(desc_size_);
    size_t read_gd_sz = std::min(sizeof(gd), static_cast<size_t>(desc_size_));
    if (!ReadBytesAt(gd_off, &gd, read_gd_sz)) return false;

    uint64_t itable_block = gd.bg_inode_table_lo;
    if (desc_size_ >= 64u) {
        itable_block |= (static_cast<uint64_t>(gd.bg_inode_table_hi) << 32);
    }

    uint64_t inode_off = itable_block * static_cast<uint64_t>(block_size_) +
                         static_cast<uint64_t>(index) * static_cast<uint64_t>(inode_size_);
    std::memset(&out_inode, 0, sizeof(out_inode));
    return ReadBytesAt(inode_off, &out_inode, std::min(sizeof(out_inode), static_cast<size_t>(inode_size_)));
}

bool Ext4Reader::SearchExtentTree(const uint8_t* node_bytes,
                                  uint32_t logical_block,
                                  uint64_t& out_phys_block) const {
    const auto* hdr = reinterpret_cast<const Ext4ExtentHeader*>(node_bytes);
    if (hdr->eh_magic != kExt4ExtentMagic) return false;

    if (hdr->eh_depth == 0u) {
        const auto* exts = reinterpret_cast<const Ext4Extent*>(node_bytes + sizeof(Ext4ExtentHeader));
        for (uint16_t i = 0; i < hdr->eh_entries; ++i) {
            uint32_t start = exts[i].ee_block;
            uint16_t raw_len = exts[i].ee_len;
            bool uninitialized = raw_len > 32768u;
            uint32_t len = uninitialized ? (raw_len - 32768u) : raw_len;
            if (logical_block >= start && logical_block < start + len) {
                if (uninitialized) {
                    out_phys_block = 0ull;
                    return true;
                }
                uint64_t phys_base = static_cast<uint64_t>(exts[i].ee_start_lo) |
                                     (static_cast<uint64_t>(exts[i].ee_start_hi) << 32);
                out_phys_block = phys_base + (logical_block - start);
                return true;
            }
        }
        out_phys_block = 0ull; // sparse hole
        return true;
    }

    const auto* idxs = reinterpret_cast<const Ext4ExtentIdx*>(node_bytes + sizeof(Ext4ExtentHeader));
    int chosen = -1;
    for (uint16_t i = 0; i < hdr->eh_entries; ++i) {
        if (idxs[i].ei_block <= logical_block) {
            chosen = static_cast<int>(i);
        } else {
            break;
        }
    }
    if (chosen < 0) {
        out_phys_block = 0ull;
        return true;
    }

    uint64_t child_block = static_cast<uint64_t>(idxs[chosen].ei_leaf_lo) |
                           (static_cast<uint64_t>(idxs[chosen].ei_leaf_hi) << 32);
    std::vector<uint8_t> child_buf(block_size_);
    if (!ReadBlock(child_block, child_buf.data())) return false;
    return SearchExtentTree(child_buf.data(), logical_block, out_phys_block);
}

bool Ext4Reader::ReadInodeBlock(const RawInode& inode,
                                uint32_t logical_block,
                                uint64_t& out_phys_block) const {
    if (inode.UsesExtents()) {
        return SearchExtentTree(reinterpret_cast<const uint8_t*>(inode.i_block), logical_block, out_phys_block);
    }

    const uint32_t ptrs_per_block = block_size_ / 4u;
    if (logical_block < 12u) {
        out_phys_block = inode.i_block[logical_block];
        return true;
    }

    logical_block -= 12u;
    if (logical_block < ptrs_per_block) {
        uint32_t ind = inode.i_block[12];
        if (ind == 0u) {
            out_phys_block = 0ull;
            return true;
        }
        std::vector<uint32_t> table(ptrs_per_block);
        if (!ReadBlock(ind, table.data())) return false;
        out_phys_block = table[logical_block];
        return true;
    }

    logical_block -= ptrs_per_block;
    uint64_t dind_cap = static_cast<uint64_t>(ptrs_per_block) * ptrs_per_block;
    if (logical_block < dind_cap) {
        uint32_t dind = inode.i_block[13];
        if (dind == 0u) {
            out_phys_block = 0ull;
            return true;
        }
        std::vector<uint32_t> l1(ptrs_per_block), l2(ptrs_per_block);
        if (!ReadBlock(dind, l1.data())) return false;
        uint32_t idx1 = logical_block / ptrs_per_block;
        uint32_t idx2 = logical_block % ptrs_per_block;
        if (l1[idx1] == 0u) {
            out_phys_block = 0ull;
            return true;
        }
        if (!ReadBlock(l1[idx1], l2.data())) return false;
        out_phys_block = l2[idx2];
        return true;
    }

    return false;
}

bool Ext4Reader::ReadInodeData(const RawInode& inode, std::vector<uint8_t>& out_data) const {
    uint64_t sz = inode.Size();
    if (sz > 512ull * 1024ull * 1024ull) {
        last_error_ = "file exceeds 512 MiB safety limit";
        return false;
    }
    out_data.resize(static_cast<size_t>(sz));
    if (sz == 0u) return true;

    std::vector<uint8_t> blk_buf(block_size_);
    uint32_t num_blocks = static_cast<uint32_t>((sz + block_size_ - 1ull) / block_size_);
    for (uint32_t lb = 0; lb < num_blocks; ++lb) {
        uint64_t phys = 0;
        if (!ReadInodeBlock(inode, lb, phys)) return false;
        if (!ReadBlock(phys, blk_buf.data())) return false;
        uint64_t offset = static_cast<uint64_t>(lb) * block_size_;
        size_t copy_len = static_cast<size_t>(std::min<uint64_t>(block_size_, sz - offset));
        std::memcpy(out_data.data() + offset, blk_buf.data(), copy_len);
    }
    return true;
}

bool Ext4Reader::ReadSymlinkFromInode(const RawInode& inode, std::string& out_target) const {
    if (!inode.IsSymlink()) return false;
    uint64_t sz = inode.Size();
    if (sz == 0u || sz > 4096u) return false;
    if (inode.i_blocks_lo == 0u && sz < 60u) {
        const char* inline_ptr = reinterpret_cast<const char*>(inode.i_block);
        out_target.assign(inline_ptr, static_cast<size_t>(sz));
        return true;
    }
    std::vector<uint8_t> buf;
    if (!ReadInodeData(inode, buf)) return false;
    out_target.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
    return true;
}

bool Ext4Reader::ReadDirFromInode(const RawInode& inode, std::vector<Ext4DirEntry>& out_entries) const {
    if (!inode.IsDirectory()) return false;
    std::vector<uint8_t> dir_bytes;
    if (!ReadInodeData(inode, dir_bytes)) return false;

    out_entries.clear();
    size_t num_blocks = dir_bytes.size() / block_size_;
    for (size_t b = 0; b < num_blocks; ++b) {
        size_t blk_start = b * block_size_;
        size_t pos = 0;
        while (pos + 8u <= block_size_) {
            const auto* de = reinterpret_cast<const Ext4DirEntry2*>(dir_bytes.data() + blk_start + pos);
            if (de->rec_len < 8u || pos + de->rec_len > block_size_) {
                break;
            }
            if (de->inode != 0u && de->name_len > 0u && pos + 8u + de->name_len <= block_size_) {
                Ext4DirEntry entry;
                entry.inode = de->inode;
                entry.file_type = de->file_type;
                entry.name.assign(de->name, de->name_len);
                out_entries.push_back(std::move(entry));
            }
            pos += de->rec_len;
        }
    }
    return true;
}

bool Ext4Reader::ResolvePathInode(const std::string& guest_path,
                                  uint32_t& out_inode_num,
                                  RawInode& out_inode,
                                  bool follow_final_symlink,
                                  int depth) const {
    if (fd_ < 0 || depth > 16) return false;
    uint32_t cur_ino = kExt4RootIno;
    RawInode cur_inode{};
    if (!ReadInode(cur_ino, cur_inode)) return false;

    std::vector<std::string> parts = SplitPath(guest_path);
    std::vector<std::string> resolved_stack;

    for (size_t idx = 0; idx < parts.size(); ++idx) {
        const std::string& part = parts[idx];
        if (part == "..") {
            if (!resolved_stack.empty()) {
                resolved_stack.pop_back();
                std::string parent_path = "/";
                for (size_t j = 0; j < resolved_stack.size(); ++j) {
                    if (j > 0) parent_path += "/";
                    parent_path += resolved_stack[j];
                }
                if (!ResolvePathInode(parent_path, cur_ino, cur_inode, true, depth + 1)) {
                    return false;
                }
            }
            continue;
        }

        if (!cur_inode.IsDirectory()) return false;
        std::vector<Ext4DirEntry> entries;
        if (!ReadDirFromInode(cur_inode, entries)) return false;

        uint32_t next_ino = 0;
        for (const auto& e : entries) {
            if (e.name == part) {
                next_ino = e.inode;
                break;
            }
        }
        if (next_ino == 0u) return false;

        RawInode next_inode{};
        if (!ReadInode(next_ino, next_inode)) return false;

        bool is_last = (idx + 1 == parts.size());
        if (next_inode.IsSymlink() && (!is_last || follow_final_symlink)) {
            std::string target;
            if (!ReadSymlinkFromInode(next_inode, target)) return false;
            std::string expanded;
            if (!target.empty() && target[0] == '/') {
                expanded = target;
            } else {
                expanded = "/";
                for (size_t j = 0; j < resolved_stack.size(); ++j) {
                    if (j > 0) expanded += "/";
                    expanded += resolved_stack[j];
                }
                if (expanded != "/") expanded += "/";
                expanded += target;
            }
            for (size_t k = idx + 1; k < parts.size(); ++k) {
                expanded += "/" + parts[k];
            }
            return ResolvePathInode(expanded, out_inode_num, out_inode, follow_final_symlink, depth + 1);
        }

        cur_ino = next_ino;
        cur_inode = next_inode;
        resolved_stack.push_back(part);
    }

    out_inode_num = cur_ino;
    out_inode = cur_inode;
    return true;
}

bool Ext4Reader::Stat(const std::string& guest_path, Ext4Stat& out_stat) const {
    uint32_t ino = 0;
    RawInode raw{};
    if (!ResolvePathInode(guest_path, ino, raw, false)) return false;
    out_stat.inode = ino;
    out_stat.mode = raw.i_mode;
    out_stat.size = raw.Size();
    out_stat.uid = raw.i_uid;
    out_stat.gid = raw.i_gid;
    out_stat.links_count = raw.i_links_count;
    return true;
}

bool Ext4Reader::Exists(const std::string& guest_path) const {
    uint32_t ino = 0;
    RawInode raw{};
    return ResolvePathInode(guest_path, ino, raw, true);
}

bool Ext4Reader::ReadSymlink(const std::string& guest_path, std::string& out_target) const {
    uint32_t ino = 0;
    RawInode raw{};
    if (!ResolvePathInode(guest_path, ino, raw, false)) return false;
    return ReadSymlinkFromInode(raw, out_target);
}

bool Ext4Reader::ReadDir(const std::string& guest_path, std::vector<Ext4DirEntry>& out_entries) const {
    uint32_t ino = 0;
    RawInode raw{};
    if (!ResolvePathInode(guest_path, ino, raw, true)) return false;
    return ReadDirFromInode(raw, out_entries);
}

bool Ext4Reader::ReadFile(const std::string& guest_path, std::vector<uint8_t>& out_data) const {
    uint32_t ino = 0;
    RawInode raw{};
    if (!ResolvePathInode(guest_path, ino, raw, true)) return false;
    if (!raw.IsRegular()) return false;
    return ReadInodeData(raw, out_data);
}

bool Ext4Reader::ReadFileString(const std::string& guest_path, std::string& out_text) const {
    std::vector<uint8_t> bytes;
    if (!ReadFile(guest_path, bytes)) return false;
    out_text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

} // namespace haseeb::loader
