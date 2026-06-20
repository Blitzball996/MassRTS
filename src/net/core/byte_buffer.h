#pragma once
// =============================================================================
// ByteBuffer - serialization primitive for the shared transport layer
// -----------------------------------------------------------------------------
// Equivalent of MinecraftConsoles' DataInputStream/DataOutputStream + ByteBuffer.
// Provides:
//   * fixed-width little-endian read/write (x86/ARM native; both target platforms
//     are little-endian so we memcpy directly for speed)
//   * VarInt (LEB128) variable-length integers
//   * length-prefixed UTF-8 strings
//   * bit packing (BitWriter/BitReader) for MoveEntityPacketSmall-style 4-byte
//     entity deltas (see DUAL_MODE_NETWORK.md section 1.3)
//
// All multiplayer wire formats (RTS command stream + voxel world sync) build on
// this single primitive.
// =============================================================================
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

namespace net {

class ByteBuffer {
public:
    std::vector<uint8_t> data;
    size_t read_pos = 0;

    ByteBuffer() = default;
    explicit ByteBuffer(std::vector<uint8_t> bytes) : data(std::move(bytes)) {}
    ByteBuffer(const uint8_t* src, size_t n) : data(src, src + n) {}

    size_t size() const { return data.size(); }
    size_t remaining() const { return data.size() - read_pos; }
    const uint8_t* ptr() const { return data.data(); }
    void clear() { data.clear(); read_pos = 0; }
    void reset_read() { read_pos = 0; }

    // ---- fixed-width write (little-endian) ---------------------------------
    template <typename T>
    void write(T v) {
        static_assert(std::is_trivially_copyable<T>::value, "POD only");
        size_t off = data.size();
        data.resize(off + sizeof(T));
        std::memcpy(data.data() + off, &v, sizeof(T));
    }
    void write_u8(uint8_t v)  { data.push_back(v); }
    void write_i8(int8_t v)   { data.push_back((uint8_t)v); }
    void write_u16(uint16_t v){ write<uint16_t>(v); }
    void write_i16(int16_t v) { write<int16_t>(v); }
    void write_u32(uint32_t v){ write<uint32_t>(v); }
    void write_i32(int32_t v) { write<int32_t>(v); }
    void write_u64(uint64_t v){ write<uint64_t>(v); }
    void write_i64(int64_t v) { write<int64_t>(v); }
    void write_f32(float v)   { write<float>(v); }
    void write_f64(double v)  { write<double>(v); }

    // ---- fixed-width read --------------------------------------------------
    template <typename T>
    T read() {
        static_assert(std::is_trivially_copyable<T>::value, "POD only");
        if (read_pos + sizeof(T) > data.size())
            throw std::runtime_error("ByteBuffer underflow");
        T v;
        std::memcpy(&v, data.data() + read_pos, sizeof(T));
        read_pos += sizeof(T);
        return v;
    }
    uint8_t  read_u8()  { if (read_pos >= data.size()) throw std::runtime_error("underflow"); return data[read_pos++]; }
    int8_t   read_i8()  { return (int8_t)read_u8(); }
    uint16_t read_u16() { return read<uint16_t>(); }
    int16_t  read_i16() { return read<int16_t>(); }
    uint32_t read_u32() { return read<uint32_t>(); }
    int32_t  read_i32() { return read<int32_t>(); }
    uint64_t read_u64() { return read<uint64_t>(); }
    int64_t  read_i64() { return read<int64_t>(); }
    float    read_f32() { return read<float>(); }
    double   read_f64() { return read<double>(); }

    // ---- VarInt (LEB128) - variable length integers (MC protocol standard) ---
    // Used heavily in MC for entity IDs, block counts, etc. Saves bandwidth:
    //   value < 128     → 1 byte
    //   value < 16384   → 2 bytes
    //   value < 2097152 → 3 bytes
    void write_varint(int32_t val) {
        uint32_t u = (uint32_t)val;
        while (u > 0x7F) {
            data.push_back((uint8_t)((u & 0x7F) | 0x80));
            u >>= 7;
        }
        data.push_back((uint8_t)u);
    }
    int32_t read_varint() {
        uint32_t result = 0;
        int shift = 0;
        while (true) {
            if (read_pos >= data.size()) throw std::runtime_error("varint underflow");
            uint8_t b = data[read_pos++];
            result |= (uint32_t)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
            if (shift >= 35) throw std::runtime_error("varint too long");
        }
        return (int32_t)result;
    }

    // ---- strings (length-prefixed UTF-8) -----------------------------------
    void write_string(const std::string& s) {
        write_varint((int32_t)s.size());
        data.insert(data.end(), s.begin(), s.end());
    }
    std::string read_string(size_t max_len = 32767) {
        int32_t len = read_varint();
        if (len < 0 || (size_t)len > max_len)
            throw std::runtime_error("string length invalid");
        if (read_pos + (size_t)len > data.size())
            throw std::runtime_error("string underflow");
        std::string s((const char*)data.data() + read_pos, (size_t)len);
        read_pos += (size_t)len;
        return s;
    }

    // ---- byte arrays (raw blobs) -------------------------------------------
    void write_bytes(const uint8_t* src, size_t n) {
        data.insert(data.end(), src, src + n);
    }
    void write_bytes(const std::vector<uint8_t>& v) {
        data.insert(data.end(), v.begin(), v.end());
    }
    void read_bytes(uint8_t* dst, size_t n) {
        if (read_pos + n > data.size()) throw std::runtime_error("bytes underflow");
        std::memcpy(dst, data.data() + read_pos, n);
        read_pos += n;
    }
};

// =============================================================================
// BitWriter / BitReader - for ultra-compact entity deltas (MoveEntitySmall)
// -----------------------------------------------------------------------------
// MinecraftConsoles MoveEntityPacketSmall.cpp:100-111 packs entity id + rotation
// into one short, xyz deltas into another → 4 bytes total for one entity move.
// This is the industrial-grade bandwidth optimization we replicate.
// =============================================================================
class BitWriter {
    uint32_t bits = 0;
    int count = 0;
    ByteBuffer& buf;
public:
    explicit BitWriter(ByteBuffer& b) : buf(b) {}
    void write(uint32_t value, int num_bits) {
        bits |= (value & ((1u << num_bits) - 1)) << count;
        count += num_bits;
        while (count >= 8) {
            buf.write_u8((uint8_t)bits);
            bits >>= 8;
            count -= 8;
        }
    }
    void flush() {
        if (count > 0) buf.write_u8((uint8_t)bits);
        bits = 0; count = 0;
    }
};

class BitReader {
    uint32_t bits = 0;
    int count = 0;
    ByteBuffer& buf;
public:
    explicit BitReader(ByteBuffer& b) : buf(b) {}
    uint32_t read(int num_bits) {
        while (count < num_bits) {
            bits |= (uint32_t)buf.read_u8() << count;
            count += 8;
        }
        uint32_t val = bits & ((1u << num_bits) - 1);
        bits >>= num_bits;
        count -= num_bits;
        return val;
    }
};

} // namespace net
