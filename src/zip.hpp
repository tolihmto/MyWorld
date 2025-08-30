#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <chrono>
#include <unordered_map>

// Minimal ZIP writer supporting only "store" (no compression)
// Usage:
//   ZipWriter zw(path);
//   if (!zw.ok()) { /* handle error */ }
//   zw.addFile("world.json", data);
//   zw.addFile("maps/seed_123/c0_0.csv", bytes);
//   bool ok = zw.close();
class ZipWriter {
public:
    explicit ZipWriter(const std::string& outPath) : _out(outPath, std::ios::binary), _ok(!!_out) {}
    bool ok() const { return _ok; }

    // Add a file from memory. mtime is Unix time (seconds since epoch). If 0, uses now.
    bool addFile(const std::string& name, const std::vector<uint8_t>& data, uint32_t mtime = 0);
    bool addFile(const std::string& name, const std::string& text, uint32_t mtime = 0) {
        return addFile(name, std::vector<uint8_t>(text.begin(), text.end()), mtime);
    }

    bool close();

private:
    struct CDRec {
        uint32_t crc32;
        uint32_t compSize;
        uint32_t uncompSize;
        uint32_t localHeaderOffset;
        uint16_t modTime;
        uint16_t modDate;
        std::string name;
    };

    static uint32_t crc32(const uint8_t* data, size_t len);
    static void write32(std::ofstream& os, uint32_t v) { os.write(reinterpret_cast<const char*>(&v), 4); }
    static void write16(std::ofstream& os, uint16_t v) { os.write(reinterpret_cast<const char*>(&v), 2); }
    static void msdosTimeDate(uint32_t unixTime, uint16_t& dosTime, uint16_t& dosDate);

    std::ofstream _out;
    bool _ok = false;
    std::vector<CDRec> _cd;
};

// Minimal ZIP reader supporting only files stored with method 0 (no compression)
class ZipReader {
public:
    explicit ZipReader(const std::string& path) { open(path); }
    ZipReader() = default;
    bool open(const std::string& path);
    bool ok() const { return _ok; }
    std::vector<std::string> listFiles() const;
    bool readFile(const std::string& name, std::vector<uint8_t>& out);

private:
    struct Entry {
        uint32_t crc32;
        uint32_t compSize;
        uint32_t uncompSize;
        uint32_t localHeaderOffset;
        uint16_t modTime;
        uint16_t modDate;
        uint16_t method; // must be 0
    };
    static uint32_t read32(std::ifstream& is) { uint32_t v; is.read(reinterpret_cast<char*>(&v), 4); return v; }
    static uint16_t read16(std::ifstream& is) { uint16_t v; is.read(reinterpret_cast<char*>(&v), 2); return v; }
    bool _ok = false;
    std::ifstream _in;
    std::vector<std::string> _names;
    std::unordered_map<std::string, Entry> _map;
};
