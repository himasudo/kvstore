#include "persistence_format.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

static void append_u32_le(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

static uint32_t read_u32_le(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint32_t crc32c(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xffffffffu;
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0x82f63b78u & mask);
        }
    }
    return ~crc;
}

std::array<uint8_t, PERSISTENCE_FILE_HEADER_SIZE> make_file_header(
    const std::array<uint8_t, 4>& magic) {
    std::array<uint8_t, PERSISTENCE_FILE_HEADER_SIZE> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    header[4] = static_cast<uint8_t>(PERSISTENCE_FORMAT_VERSION);
    header[5] = static_cast<uint8_t>(PERSISTENCE_FORMAT_VERSION >> 8);
    header[6] = static_cast<uint8_t>(PERSISTENCE_FORMAT_VERSION >> 16);
    header[7] = static_cast<uint8_t>(PERSISTENCE_FORMAT_VERSION >> 24);
    return header;
}

void validate_file_header(std::span<const uint8_t> header,
                          const std::array<uint8_t, 4>& expected_magic) {
    if (header.size() != PERSISTENCE_FILE_HEADER_SIZE) {
        throw std::runtime_error("Persistence file: invalid header size");
    }
    if (!std::equal(expected_magic.begin(), expected_magic.end(), header.begin())) {
        throw std::runtime_error("Persistence file: invalid magic");
    }

    uint32_t version = read_u32_le(header.data() + 4);
    if (version != PERSISTENCE_FORMAT_VERSION) {
        throw std::runtime_error("Persistence file: unsupported format version");
    }
}

PersistenceRecordHeader decode_record_header(std::span<const uint8_t> header) {
    if (header.size() != PERSISTENCE_RECORD_HEADER_SIZE) {
        throw std::runtime_error("Persistence record: invalid header size");
    }

    uint32_t payload_size = read_u32_le(header.data());
    if (payload_size < PERSISTENCE_MIN_PAYLOAD_SIZE ||
        payload_size > PERSISTENCE_MAX_PAYLOAD_SIZE) {
        throw std::runtime_error("Persistence record: invalid payload size");
    }

    return {payload_size, read_u32_le(header.data() + 4)};
}

std::vector<uint8_t> encode_record(uint8_t opcode,
                                   const std::string& key,
                                   const std::string& value) {
    if (key.size() > std::numeric_limits<uint32_t>::max() ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("Persistence record key or value is too large");
    }

    uint64_t payload_size = sizeof(opcode) + sizeof(uint32_t) + key.size() +
                            sizeof(uint32_t) + value.size();
    if (payload_size > PERSISTENCE_MAX_PAYLOAD_SIZE) {
        throw std::length_error("Persistence record exceeds maximum size");
    }

    std::vector<uint8_t> payload;
    payload.reserve(static_cast<size_t>(payload_size));
    payload.push_back(opcode);
    append_u32_le(payload, static_cast<uint32_t>(key.size()));
    payload.insert(payload.end(), key.begin(), key.end());
    append_u32_le(payload, static_cast<uint32_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());

    std::vector<uint8_t> record;
    record.reserve(PERSISTENCE_RECORD_HEADER_SIZE + payload.size());
    append_u32_le(record, static_cast<uint32_t>(payload.size()));
    append_u32_le(record, crc32c(payload));
    record.insert(record.end(), payload.begin(), payload.end());
    return record;
}

PersistenceRecord decode_record_payload(std::span<const uint8_t> payload,
                                        uint32_t expected_checksum) {
    if (payload.size() < PERSISTENCE_MIN_PAYLOAD_SIZE ||
        payload.size() > PERSISTENCE_MAX_PAYLOAD_SIZE) {
        throw std::runtime_error("Persistence record: invalid payload size");
    }
    if (crc32c(payload) != expected_checksum) {
        throw std::runtime_error("Persistence record: checksum mismatch");
    }

    size_t offset = 0;
    auto require = [&](size_t count) {
        if (count > payload.size() - offset) {
            throw std::runtime_error("Persistence record: field exceeds record boundary");
        }
    };
    auto read_u32 = [&]() {
        require(sizeof(uint32_t));
        uint32_t value = read_u32_le(payload.data() + offset);
        offset += sizeof(uint32_t);
        return value;
    };

    require(sizeof(uint8_t));
    uint8_t opcode = payload[offset++];

    uint32_t key_len = read_u32();
    require(key_len);
    std::string key(reinterpret_cast<const char*>(payload.data() + offset), key_len);
    offset += key_len;

    uint32_t value_len = read_u32();
    require(value_len);
    std::string value(reinterpret_cast<const char*>(payload.data() + offset), value_len);
    offset += value_len;

    if (offset != payload.size()) {
        throw std::runtime_error("Persistence record: trailing bytes");
    }

    return {opcode, std::move(key), std::move(value)};
}
