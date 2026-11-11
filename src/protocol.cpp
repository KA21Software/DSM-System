#include "protocol.h"

#include <cstring>

uint32_t fletcher32(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    // 64-bit accumulators cannot overflow for payloads up to 64 KiB (sum2 < 2^47), so the
    // modulo is applied once at the end and the result equals the per-step definition.
    uint64_t sum1 = 0, sum2 = 0;
    for (size_t i = 0; i < len; i += 2) {
        uint16_t word = p[i];
        if (i + 1 < len) word |= static_cast<uint16_t>(p[i + 1]) << 8;  // odd tail: zero padded
        sum1 += word;
        sum2 += sum1;
    }
    return static_cast<uint32_t>(((sum2 % 65535) << 16) | (sum1 % 65535));
}

void header_encode(const Header& h, uint8_t out[KADP_HDR_LEN]) {
    memset(out, 0, KADP_HDR_LEN);  // also leaves the reserved bytes at zero
    std::vector<uint8_t> v;
    v.reserve(KADP_HDR_LEN);
    ByteWriter w(v);
    w.u32(KADP_MAGIC);                         // 0
    w.u8(KADP_VERSION);                        // 4
    w.u8(h.type);                              // 5
    w.u16(h.flags);                            // 6
    w.u16(h.src_node);                         // 8
    w.u16(h.dst_node);                         // 10
    w.u32(h.seq);                              // 12
    w.u32(h.page_id);                          // 16
    w.u32(h.page_version);                     // 20
    w.u32(h.payload_len);                      // 24
    w.u32(h.checksum);                         // 28
    w.u16(static_cast<uint16_t>(h.status));    // 32
    memcpy(out, v.data(), v.size());           // bytes 34..39 stay zero (reserved)
}

int header_decode(const uint8_t in[KADP_HDR_LEN], Header& out) {
    ByteReader r(in, KADP_HDR_LEN);
    if (r.u32() != KADP_MAGIC) return DSM_EPROTO;
    if (r.u8() != KADP_VERSION) return DSM_EPROTO;
    Header h;
    h.type = r.u8();
    h.flags = r.u16();
    h.src_node = r.u16();
    h.dst_node = r.u16();
    h.seq = r.u32();
    h.page_id = r.u32();
    h.page_version = r.u32();
    h.payload_len = r.u32();
    h.checksum = r.u32();
    h.status = static_cast<int16_t>(r.u16());
    // The reserved bytes are ignored on purpose so future extras can use them.
    if (h.payload_len > KADP_MAX_PAYLOAD) return DSM_EPROTO;
    out = h;
    return DSM_OK;
}

int frame_build(Header h, const void* payload, size_t len, std::vector<uint8_t>& out) {
    if (len > KADP_MAX_PAYLOAD || (len > 0 && !payload)) return DSM_EINVAL;
    h.payload_len = static_cast<uint32_t>(len);
    h.checksum = len ? fletcher32(payload, len) : 0;
    out.resize(KADP_HDR_LEN);
    header_encode(h, out.data());
    if (len) out.insert(out.end(), static_cast<const uint8_t*>(payload), static_cast<const uint8_t*>(payload) + len);
    return DSM_OK;
}

int payload_verify(const Header& h, const void* payload) {
    if (h.payload_len == 0) return h.checksum == 0 ? DSM_OK : DSM_EPROTO;
    if (!payload) return DSM_EPROTO;
    return fletcher32(payload, h.payload_len) == h.checksum ? DSM_OK : DSM_EPROTO;
}

bool ByteReader::bytes(void* dst, size_t n) {
    if (!take(n)) return false;
    memcpy(dst, p_ + pos_ - n, n);
    return true;
}
