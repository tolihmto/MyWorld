#include "zip.hpp"
#include <ctime>
#include <vector>
#include <string>
#include <limits>

// CRC32 (polynomial 0xEDB88320), tableless
static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint32_t ZipWriter::crc32(const uint8_t* data, size_t len) {
    return crc32_update(0, data, len);
}

void ZipWriter::msdosTimeDate(uint32_t unixTime, uint16_t& dosTime, uint16_t& dosDate) {
    std::time_t t = unixTime ? (std::time_t)unixTime : std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    // DOS time: bits: hh(5) mm(6) ss/2(5)
    dosTime = (uint16_t)(((tm.tm_hour) << 11) | ((tm.tm_min) << 5) | ((tm.tm_sec / 2)));
    // DOS date: bits: year-1980(7) month(4) day(5)
    int year = tm.tm_year + 1900;
    dosDate = (uint16_t)(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | (tm.tm_mday));
}

bool ZipWriter::addFile(const std::string& name, const std::vector<uint8_t>& data, uint32_t mtime) {
    if (!_ok) return false;
    // Local file header
    uint32_t sig = 0x04034b50u;
    write32(_out, sig);
    write16(_out, 20);         // version needed to extract
    write16(_out, 0);          // general purpose bit flag
    write16(_out, 0);          // compression method: 0 = store
    uint16_t dt=0, dd=0; msdosTimeDate(mtime, dt, dd);
    write16(_out, dt);         // last mod file time
    write16(_out, dd);         // last mod file date
    uint32_t c = crc32(data.data(), data.size());
    write32(_out, c);          // CRC-32
    write32(_out, (uint32_t)data.size()); // compressed size (store)
    write32(_out, (uint32_t)data.size()); // uncompressed size
    write16(_out, (uint16_t)name.size()); // file name length
    write16(_out, 0);          // extra field length
    _out.write(name.data(), name.size()); // file name
    uint32_t offset = (uint32_t)_out.tellp();
    offset -= (4 + 2 + 2 + 2 + 2 + 2 + 4 + 4 + 4 + 2 + 2 + (uint32_t)name.size());
    // File data
    if (!data.empty()) _out.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());

    // Record for central directory
    CDRec rec{};
    rec.crc32 = c;
    rec.compSize = (uint32_t)data.size();
    rec.uncompSize = (uint32_t)data.size();
    rec.localHeaderOffset = offset;
    rec.modTime = dt; rec.modDate = dd;
    rec.name = name;
    _cd.push_back(std::move(rec));
    return true;
}

bool ZipWriter::close() {
    if (!_ok) return false;
    uint32_t cdStart = (uint32_t)_out.tellp();
    // Central directory entries
    for (const auto& r : _cd) {
        write32(_out, 0x02014b50u); // central file header signature
        write16(_out, 20);          // version made by
        write16(_out, 20);          // version needed to extract
        write16(_out, 0);           // general purpose bit flag
        write16(_out, 0);           // compression method
        write16(_out, r.modTime);   // last mod time
        write16(_out, r.modDate);   // last mod date
        write32(_out, r.crc32);
        write32(_out, r.compSize);
        write32(_out, r.uncompSize);
        write16(_out, (uint16_t)r.name.size()); // file name length
        write16(_out, 0);           // extra field length
        write16(_out, 0);           // file comment length
        write16(_out, 0);           // disk number start
        write16(_out, 0);           // internal file attributes
        write32(_out, 0);           // external file attributes
        write32(_out, r.localHeaderOffset);
        _out.write(r.name.data(), r.name.size());
    }
    uint32_t cdEnd = (uint32_t)_out.tellp();
    uint32_t cdSize = cdEnd - cdStart;
    // End of central directory
    write32(_out, 0x06054b50u);
    write16(_out, 0); // number of this disk
    write16(_out, 0); // number of the disk with the start of the central directory
    write16(_out, (uint16_t)_cd.size()); // total number of entries on this disk
    write16(_out, (uint16_t)_cd.size()); // total number of entries
    write32(_out, cdSize);               // size of the central directory
    write32(_out, cdStart);              // offset of start of central directory
    write16(_out, 0); // ZIP file comment length

    _out.flush();
    _out.close();
    _ok = false;
    return true;
}

// ---- ZipReader (store-only) ----
bool ZipReader::open(const std::string& path) {
    _ok = false; _names.clear(); _map.clear();
    _in = std::ifstream(path, std::ios::binary);
    if (!_in) return false;
    // Find EOCD by scanning last 64KB
    _in.seekg(0, std::ios::end);
    std::streamoff fileSize = _in.tellg();
    const std::streamoff maxBack = std::min<std::streamoff>(fileSize, 65557);
    std::vector<char> tail((size_t)maxBack);
    _in.seekg(fileSize - maxBack, std::ios::beg);
    _in.read(tail.data(), (std::streamsize)tail.size());
    int eocdIndex = -1;
    for (int i = (int)tail.size() - 22; i >= 0; --i) { // EOCD min size is 22
        const unsigned char* p = reinterpret_cast<const unsigned char*>(tail.data() + i);
        if (p[0]==0x50 && p[1]==0x4b && p[2]==0x05 && p[3]==0x06) { eocdIndex = i; break; }
    }
    if (eocdIndex < 0) return false;
    // Parse EOCD
    const char* e = tail.data() + eocdIndex;
    // skip signature (4), disk nums (2+2), entries on disk(2), total entries(2)
    uint16_t totalEntries = *reinterpret_cast<const uint16_t*>(e + 10);
    uint32_t cdSize       = *reinterpret_cast<const uint32_t*>(e + 12);
    uint32_t cdOffset     = *reinterpret_cast<const uint32_t*>(e + 16);
    // Read central directory
    _in.seekg(cdOffset, std::ios::beg);
    for (uint16_t n = 0; n < totalEntries; ++n) {
        uint32_t sig = read32(_in); if (sig != 0x02014b50u) return false;
        (void)read16(_in); // version made by
        (void)read16(_in); // version needed
        (void)read16(_in); // flags
        uint16_t method = read16(_in);
        uint16_t mtime  = read16(_in);
        uint16_t mdate  = read16(_in);
        uint32_t crc    = read32(_in);
        uint32_t csize  = read32(_in);
        uint32_t usize  = read32(_in);
        uint16_t nlen   = read16(_in);
        uint16_t xlen   = read16(_in);
        uint16_t clen   = read16(_in);
        (void)read16(_in); // disk start
        (void)read16(_in); // internal attrs
        (void)read32(_in); // external attrs
        uint32_t lho    = read32(_in);
        std::string name(nlen, '\0');
        if (nlen) _in.read(name.data(), nlen);
        if (xlen) _in.seekg(xlen, std::ios::cur);
        if (clen) _in.seekg(clen, std::ios::cur);
        // Directories have trailing '/'; still record them but skip reading later
        Entry ent{}; ent.crc32 = crc; ent.compSize = csize; ent.uncompSize = usize; ent.localHeaderOffset = lho; ent.modTime = mtime; ent.modDate = mdate; ent.method = method;
        _names.push_back(name);
        _map.emplace(name, ent);
    }
    _ok = true; return true;
}

std::vector<std::string> ZipReader::listFiles() const { return _names; }

bool ZipReader::readFile(const std::string& name, std::vector<uint8_t>& out) {
    auto it = _map.find(name);
    if (it == _map.end()) return false;
    const Entry& ent = it->second;
    if (ent.method != 0) return false; // only store
    // Seek to local header
    _in.seekg(ent.localHeaderOffset, std::ios::beg);
    uint32_t sig = read32(_in); if (sig != 0x04034b50u) return false;
    (void)read16(_in); // version needed
    (void)read16(_in); // flags
    uint16_t method = read16(_in); if (method != 0) return false;
    (void)read16(_in); // time
    (void)read16(_in); // date
    (void)read32(_in); // crc32
    uint32_t csize = read32(_in);
    (void)read32(_in); // usize (can trust from central dir)
    uint16_t nlen = read16(_in);
    uint16_t xlen = read16(_in);
    if (nlen) _in.seekg(nlen, std::ios::cur);
    if (xlen) _in.seekg(xlen, std::ios::cur);
    out.clear(); out.resize(csize);
    if (csize) _in.read(reinterpret_cast<char*>(out.data()), csize);
    return true;
}
