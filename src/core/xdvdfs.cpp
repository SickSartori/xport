#include "core/xdvdfs.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>

#include "core/byteorder.h"

namespace xport {
namespace {

constexpr uint64_t kSectorSize = 2048;
constexpr uint64_t kVolumeDescriptorSector = 32;
constexpr char kVolumeMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kVolumeMagicLength = sizeof(kVolumeMagic) - 1;

// Start of the game partition: 0 for an XISO, otherwise the XGD layouts of full disc dumps.
constexpr uint64_t kPartitionOffsets[] = {0, 0x18300000, 0xFD90000, 0x2080000};

// Directory entry: u16 left, u16 right (in 4-byte units), u32 sector, u32 size, u8 attributes, u8 name length, name.
constexpr size_t kEntryHeaderSize = 14;
constexpr uint16_t kPaddingMarker = 0xFFFF;
constexpr uint8_t kAttributeDirectory = 0x10;

std::FILE* open_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    return _wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

bool seek_file(std::FILE* file, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

uint64_t load_le64(const uint8_t* p) { return load_le32(p) | static_cast<uint64_t>(load_le32(p + 4)) << 32; }

// CCI, the compressed image format of Repackinator and the Xbox's homebrew dashboards: a 32-byte header ("CCIM",
// header size, uncompressed size, index offset, block size 2048, version 1, index alignment 2), the sectors, then the
// index: one 32-bit entry per sector and one for the end, the position shifted right by the alignment, the top bit
// set for an LZ4-compressed sector. A compressed sector is a byte giving the zero padding at its end, then the LZ4
// block.
constexpr uint32_t kCciMagic = 0x4D494343;  // "CCIM"
constexpr uint32_t kCciHeaderSize = 32;
constexpr uint32_t kCciCompressed = 0x80000000;

// An LZ4 block (no frame): sequences of literals and copies from earlier output. Returns the bytes written, or -1
// for data that does not fit `capacity` or points before the start.
int64_t lz4_decode_block(const uint8_t* source, size_t source_size, uint8_t* destination, size_t capacity) {
    const uint8_t* in = source;
    const uint8_t* const in_end = source + source_size;
    uint8_t* out = destination;
    uint8_t* const out_end = destination + capacity;
    const auto length = [&](int64_t value) -> int64_t {
        if (value != 15) return value;
        uint8_t more;
        do {
            if (in >= in_end) return -1;
            more = *in++;
            value += more;
        } while (more == 255);
        return value;
    };
    while (in < in_end) {
        const uint8_t token = *in++;
        const int64_t literals = length(token >> 4);
        if (literals < 0 || literals > in_end - in || literals > out_end - out) return -1;
        std::memcpy(out, in, static_cast<size_t>(literals));
        in += literals;
        out += literals;
        if (in == in_end) break;  // the last sequence has literals only
        if (in_end - in < 2) return -1;
        const int64_t distance = in[0] | in[1] << 8;
        in += 2;
        const int64_t match = length(token & 15);
        if (match < 0 || distance == 0 || distance > out - destination || match + 4 > out_end - out) return -1;
        const uint8_t* from = out - distance;
        for (int64_t i = 0; i < match + 4; ++i) *out++ = *from++;  // byte by byte: the copy may overlap itself
    }
    return out - destination;
}

// The files of a split image, from any one of them: name.1.iso, name.2.iso... (name.1.cci...), else the file alone.
std::vector<std::filesystem::path> image_files(const std::filesystem::path& path) {
    const std::filesystem::path stem = path.stem();
    const std::u8string number = stem.extension().u8string();
    if (number.size() != 2 || number[1] < u8'0' || number[1] > u8'9') return {path};
    std::vector<std::filesystem::path> files;
    for (char digit = '1'; digit <= '9'; ++digit) {
        std::filesystem::path file = path.parent_path() / stem.stem();
        file += std::string{'.', digit};
        file += path.extension();
        std::error_code ignored;
        if (!std::filesystem::is_regular_file(file, ignored)) break;
        files.push_back(file);
    }
    return files.empty() ? std::vector<std::filesystem::path>{path} : files;
}

bool equals_ignore_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

namespace {
thread_local uint64_t t_read_wait_us = 0;

// Each thread reads the plain parts of the image through files of its own, so no read waits for another. With one
// file and one lock for every thread, a short read of the music (Midnight Club 3's XACT, asked by the thread that
// makes the frame) could wait behind whoever held the lock, likely a thread of low priority on the game's only core,
// preempted there for as long as Windows let the others run (not verified): 200 ms hitches at the same places of a race on a fast SSD that
// went away with the music off (mc3port issue 29). Closed when the thread ends.
struct ThreadFiles {
    struct Open {
        uint64_t image;
        size_t part;
        std::FILE* file;
    };
    std::vector<Open> open;
    ~ThreadFiles() {
        for (const Open& entry : open) std::fclose(entry.file);
    }
    std::FILE* get(uint64_t image, size_t part, const std::filesystem::path& path) {
        for (const Open& entry : open) {
            if (entry.image == image && entry.part == part) return entry.file;
        }
        std::FILE* file = open_file(path);
        if (file) open.push_back({image, part, file});
        return file;
    }
    // The calling thread's files of an image being closed (other threads close theirs when they end).
    void forget(uint64_t image) {
        std::erase_if(open, [image](const Open& entry) {
            if (entry.image != image) return false;
            std::fclose(entry.file);
            return true;
        });
    }
};
thread_local ThreadFiles t_files;
std::atomic<uint64_t> g_next_image_id{1};
}

std::unique_ptr<XdvdfsImage> XdvdfsImage::open(const std::filesystem::path& path, std::string* error) {
    std::unique_ptr<XdvdfsImage> image(new XdvdfsImage());
    image->id_ = g_next_image_id++;
    uint64_t start = 0;
    for (const std::filesystem::path& file_path : image_files(path)) {
        Part& part = image->parts_.emplace_back();  // closed with the image, whatever happens next
        part.file = open_file(file_path);
        part.path = file_path;
        if (!part.file) {
            if (error) *error = "cannot open " + path_to_utf8(file_path);
            return nullptr;
        }
        part.start = start;
        uint8_t header[kCciHeaderSize] = {};
        if (std::fread(header, 1, sizeof(header), part.file) == sizeof(header) && load_le32(header) == kCciMagic) {
            const uint64_t size = load_le64(header + 8), index = load_le64(header + 16);
            if (load_le32(header + 4) != kCciHeaderSize || load_le32(header + 24) != kSectorSize || header[28] != 1 ||
                header[29] != 2 || size % kSectorSize != 0) {
                if (error) *error = "unsupported CCI header in " + path_to_utf8(file_path);
                return nullptr;
            }
            part.sectors.resize(static_cast<size_t>(size / kSectorSize) + 1);
            if (!seek_file(part.file, index) ||
                std::fread(part.sectors.data(), 4, part.sectors.size(), part.file) != part.sectors.size()) {
                if (error) *error = "the CCI index of " + path_to_utf8(file_path) + " is cut short";
                return nullptr;
            }
            for (uint32_t& entry : part.sectors) entry = load_le32(&entry);
            part.size = size;
        } else {
            std::error_code failed;
            part.size = std::filesystem::file_size(file_path, failed);
            if (failed) part.size = 0;
        }
        start += part.size;
    }
    for (uint64_t partition : kPartitionOffsets) {
        uint8_t descriptor[kSectorSize];
        if (!image->read_raw(partition + kVolumeDescriptorSector * kSectorSize, descriptor, sizeof(descriptor))) {
            continue;
        }
        if (std::memcmp(descriptor, kVolumeMagic, kVolumeMagicLength) != 0) continue;
        image->partition_offset_ = partition;
        image->root_.sector = load_le32(descriptor + 20);
        image->root_.size = load_le32(descriptor + 24);
        image->root_.is_directory = true;
        return image;
    }
    if (error) *error = "no Xbox game partition (XDVDFS) found in " + path_to_utf8(path);
    return nullptr;
}

XdvdfsImage::~XdvdfsImage() {
    t_files.forget(id_);
    for (const Part& part : parts_) {
        if (part.file) std::fclose(part.file);
    }
}

// Sizes come from the image: checked against the image's end before anything that size is allocated.
bool XdvdfsImage::within_image(const Entry& entry) const {
    const uint64_t end = parts_.back().start + parts_.back().size;  // parts_ is never empty after open()
    const uint64_t begin = partition_offset_ + entry.sector * kSectorSize;
    return begin <= end && entry.size <= end - begin;
}

std::vector<XdvdfsImage::Entry> XdvdfsImage::list(const Entry& directory) const {
    std::vector<Entry> entries;
    if (!directory.is_directory || directory.size == 0 || !within_image(directory)) return entries;
    std::vector<uint8_t> table(directory.size);
    if (!read_raw(partition_offset_ + directory.sector * kSectorSize, table.data(), table.size())) return entries;

    // Entries form a binary tree rooted at offset 0; unused space at the end of each sector is 0xFF padding.
    std::vector<uint32_t> pending{0};
    std::vector<bool> visited(table.size() / 4 + 1, false);
    while (!pending.empty()) {
        const uint32_t offset = pending.back();
        pending.pop_back();
        if (offset + kEntryHeaderSize > table.size() || visited[offset / 4]) continue;
        visited[offset / 4] = true;

        const uint8_t* raw = table.data() + offset;
        const uint16_t left = load_le16(raw);
        const uint16_t right = load_le16(raw + 2);
        if (left == kPaddingMarker && right == kPaddingMarker) continue;
        const uint8_t name_length = raw[13];
        if (offset + kEntryHeaderSize + name_length > table.size()) continue;

        Entry entry;
        entry.sector = load_le32(raw + 4);
        entry.size = load_le32(raw + 8);
        entry.is_directory = (raw[12] & kAttributeDirectory) != 0;
        entry.name.assign(reinterpret_cast<const char*>(raw + kEntryHeaderSize), name_length);
        entries.push_back(std::move(entry));

        if (left) pending.push_back(static_cast<uint32_t>(left) * 4);
        if (right) pending.push_back(static_cast<uint32_t>(right) * 4);
    }
    return entries;
}

std::optional<XdvdfsImage::Entry> XdvdfsImage::find(std::string_view path) const {
    Entry current = root_;
    while (!path.empty()) {
        const size_t separator = path.find_first_of("\\/");
        const std::string_view component = path.substr(0, separator);
        path = separator == std::string_view::npos ? std::string_view{} : path.substr(separator + 1);
        if (component.empty()) continue;

        const std::vector<Entry> children = list(current);
        const auto match = std::find_if(children.begin(), children.end(),
                                        [&](const Entry& child) { return equals_ignore_case(child.name, component); });
        if (match == children.end()) return std::nullopt;
        current = *match;
    }
    return current;
}

size_t XdvdfsImage::read(const Entry& file, uint64_t offset, void* destination, size_t size) const {
    if (file.is_directory || offset >= file.size) return 0;
    const size_t count = static_cast<size_t>(std::min<uint64_t>(size, file.size - offset));
    const uint64_t position = partition_offset_ + file.sector * kSectorSize + offset;
    return read_raw(position, destination, count) ? count : 0;
}

std::vector<uint8_t> XdvdfsImage::read_all(const Entry& file) const {
    if (!within_image(file)) return {};
    std::vector<uint8_t> data(file.size);
    if (read(file, 0, data.data(), data.size()) != data.size()) data.clear();
    return data;
}

std::optional<std::string> XdvdfsImage::entry_past_end() const {
    std::vector<Entry> pending{root_};
    // The count bounds a damaged image whose directories point at each other (a game disc has a few thousand entries).
    for (int seen = 0; !pending.empty() && seen < 100000;) {
        const Entry directory = std::move(pending.back());
        pending.pop_back();
        for (Entry& entry : list(directory)) {
            ++seen;
            if (!within_image(entry)) return entry.name;
            if (entry.is_directory) pending.push_back(std::move(entry));
        }
    }
    return std::nullopt;
}

uint64_t XdvdfsImage::last_read_wait_us() { return t_read_wait_us; }

bool XdvdfsImage::read_raw(uint64_t offset, void* destination, size_t size) const {
    t_read_wait_us = 0;
    // Plain parts without the lock, each thread with its own files; compressed parts, and a part that cannot be
    // opened again, through the shared files in turn.
    bool plain = true;
    for (uint64_t at = offset, left = size; left;) {
        const auto part = std::find_if(parts_.begin(), parts_.end(), [&](const Part& candidate) {
            return at >= candidate.start && at - candidate.start < candidate.size;
        });
        if (part == parts_.end()) return false;
        if (!part->sectors.empty() || part->path.empty()) {
            plain = false;
            break;
        }
        const uint64_t count = std::min<uint64_t>(left, part->size - (at - part->start));
        at += count;
        left -= count;
    }
    if (plain) {
        auto* out = static_cast<uint8_t*>(destination);
        while (size) {
            const auto part = std::find_if(parts_.begin(), parts_.end(), [&](const Part& candidate) {
                return offset >= candidate.start && offset - candidate.start < candidate.size;
            });
            std::FILE* file = t_files.get(id_, static_cast<size_t>(part - parts_.begin()), part->path);
            if (!file) {
                plain = false;
                break;
            }
            const uint64_t inside = offset - part->start;
            const size_t count = static_cast<size_t>(std::min<uint64_t>(size, part->size - inside));
            if (!seek_file(file, inside) || std::fread(out, 1, count, file) != count) return false;
            out += count;
            offset += count;
            size -= count;
        }
        if (plain) return true;
        destination = out;
    }
    const auto asked = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    t_read_wait_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - asked).count());
    auto* out = static_cast<uint8_t*>(destination);
    while (size) {
        const auto part = std::find_if(parts_.begin(), parts_.end(), [&](const Part& candidate) {
            return offset >= candidate.start && offset - candidate.start < candidate.size;
        });
        if (part == parts_.end()) return false;
        uint64_t inside = offset - part->start;
        const size_t count = static_cast<size_t>(std::min<uint64_t>(size, part->size - inside));
        if (part->sectors.empty()) {
            if (!seek_file(part->file, inside) || std::fread(out, 1, count, part->file) != count) return false;
        } else {
            for (size_t done = 0; done < count;) {
                const size_t within = static_cast<size_t>(inside % kSectorSize);
                const size_t piece = std::min<size_t>(count - done, kSectorSize - within);
                if (piece == kSectorSize) {
                    if (!read_cci_sector(*part, inside / kSectorSize, out + done)) return false;
                } else {
                    uint8_t whole[kSectorSize];
                    if (!read_cci_sector(*part, inside / kSectorSize, whole)) return false;
                    std::memcpy(out + done, whole + within, piece);
                }
                done += piece;
                inside += piece;
            }
        }
        out += count;
        offset += count;
        size -= count;
    }
    return true;
}

bool XdvdfsImage::read_cci_sector(const Part& part, uint64_t sector, uint8_t* destination) const {
    if (sector + 1 >= part.sectors.size()) return false;
    const uint32_t entry = part.sectors[sector];
    const uint64_t position = static_cast<uint64_t>(entry & ~kCciCompressed) << 2;
    const uint64_t end = static_cast<uint64_t>(part.sectors[sector + 1] & ~kCciCompressed) << 2;
    if (end <= position || end - position > 2 * kSectorSize) return false;
    const size_t stored = static_cast<size_t>(end - position);
    if (!(entry & kCciCompressed) && stored == kSectorSize) {
        return seek_file(part.file, position) && std::fread(destination, 1, kSectorSize, part.file) == kSectorSize;
    }
    uint8_t block[2 * kSectorSize];
    if (!seek_file(part.file, position) || std::fread(block, 1, stored, part.file) != stored) return false;
    const size_t padding = block[0];
    if (padding + 1 > stored) return false;
    return lz4_decode_block(block + 1, stored - padding - 1, destination, kSectorSize) == kSectorSize;
}

}  // namespace xport
