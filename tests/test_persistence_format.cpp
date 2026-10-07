#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "persistence_format.h"

static bool throws_runtime(auto&& fn) {
    try {
        fn();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

int main() {
    const std::string check = "123456789";
    assert(crc32c(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(check.data()), check.size())) == 0xe3069283u);

    const std::array<uint8_t, 4> magic{'K', 'V', 'W', 'L'};
    auto file_header = make_file_header(magic);
    assert(file_header[0] == 'K');
    assert(file_header[4] == 1);
    assert(file_header[5] == 0);
    validate_file_header(file_header, magic);

    auto bad_magic = file_header;
    bad_magic[0] = 'X';
    assert(throws_runtime([&] { validate_file_header(bad_magic, magic); }));

    auto bad_version = file_header;
    bad_version[4] = 2;
    assert(throws_runtime([&] { validate_file_header(bad_version, magic); }));

    auto bytes = encode_record(0, "alpha", "one");
    auto header = decode_record_header(std::span<const uint8_t>(bytes.data(), PERSISTENCE_RECORD_HEADER_SIZE));
    auto payload = std::span<const uint8_t>(bytes.data() + PERSISTENCE_RECORD_HEADER_SIZE, header.payload_size);
    auto record = decode_record_payload(payload, header.checksum);
    assert(record.opcode == 0);
    assert(record.key == "alpha");
    assert(record.value == "one");

    std::vector<uint8_t> corrupted = bytes;
    corrupted.back() ^= 0x01;
    auto corrupted_header = decode_record_header(
        std::span<const uint8_t>(corrupted.data(), PERSISTENCE_RECORD_HEADER_SIZE));
    auto corrupted_payload = std::span<const uint8_t>(
        corrupted.data() + PERSISTENCE_RECORD_HEADER_SIZE, corrupted_header.payload_size);
    assert(throws_runtime([&] {
        (void)decode_record_payload(corrupted_payload, corrupted_header.checksum);
    }));

    return 0;
}
