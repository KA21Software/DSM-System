#include <cstdio>
#include <cstring>
#include <vector>

#include "protocol.h"

static int g_failed = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

static uint32_t fl(const char* s) { return fletcher32(s, strlen(s)); }

int main() {
    // Fletcher-32 against the published reference vectors (not computed by our own code).
    CHECK(fl("abcde") == 0xF04FC729);
    CHECK(fl("abcdef") == 0x56502D2A);
    CHECK(fl("abcdefgh") == 0xEBE19591);
    CHECK(fletcher32(nullptr, 0) == 0);
    // It must notice a single flipped bit and a swapped pair of words.
    std::vector<uint8_t> page(4096, 0x5A);
    uint32_t base = fletcher32(page.data(), page.size());
    page[1234] ^= 0x01;
    CHECK(fletcher32(page.data(), page.size()) != base);
    page[1234] ^= 0x01;
    page[0] = 1; page[2] = 2;
    uint32_t a = fletcher32(page.data(), page.size());
    page[0] = 2; page[2] = 1;
    CHECK(fletcher32(page.data(), page.size()) != a);

    // Header: every field survives a round trip, including a negative status.
    Header h;
    h.type = PAGE_DATA;
    h.flags = F_IS_REPLY | F_GRANT_M;
    h.src_node = 3; h.dst_node = 7;
    h.seq = 0xDEADBEEF; h.page_id = 0x01020304; h.page_version = 99;
    h.payload_len = 4096; h.checksum = 0xCAFEBABE;
    h.status = DSM_ENOTALLOC;  // -12
    uint8_t raw[KADP_HDR_LEN];
    header_encode(h, raw);

    // The wire layout is fixed by the design table 6.1: check bytes at their offsets.
    CHECK(memcmp(raw, "KADP", 4) == 0);
    CHECK(raw[4] == KADP_VERSION && raw[5] == PAGE_DATA);
    CHECK(raw[6] == 0x00 && raw[7] == (F_IS_REPLY | F_GRANT_M));
    CHECK(raw[8] == 0 && raw[9] == 3 && raw[10] == 0 && raw[11] == 7);
    CHECK(raw[12] == 0xDE && raw[13] == 0xAD && raw[14] == 0xBE && raw[15] == 0xEF);  // big-endian
    CHECK(raw[16] == 0x01 && raw[19] == 0x04);
    CHECK(raw[24] == 0x00 && raw[25] == 0x00 && raw[26] == 0x10 && raw[27] == 0x00);  // 4096
    CHECK(raw[32] == 0xFF && raw[33] == 0xF4);  // -12 as int16
    for (int i = 34; i < 40; i++) CHECK(raw[i] == 0);  // reserved, sent as zero

    Header d;
    CHECK(header_decode(raw, d) == DSM_OK);
    CHECK(d.type == h.type && d.flags == h.flags && d.src_node == 3 && d.dst_node == 7);
    CHECK(d.seq == h.seq && d.page_id == h.page_id && d.page_version == 99);
    CHECK(d.payload_len == 4096 && d.checksum == h.checksum && d.status == DSM_ENOTALLOC);

    // Defaults: a node without ID sends src 0xFFFF, destination "all".
    Header def;
    CHECK(def.src_node == NODE_NONE && def.dst_node == NODE_ALL);
    CHECK(msg_class(PAGE_REQUEST) == 1 && msg_class(JOIN) == 3 && msg_class(KADP_ERROR) == 0xF);

    // Rejections: wrong magic, wrong version, oversized payload (exactly 64 KiB is fine).
    uint8_t bad[KADP_HDR_LEN];
    memcpy(bad, raw, sizeof bad); bad[0] = 'X';
    CHECK(header_decode(bad, d) == DSM_EPROTO);
    memcpy(bad, raw, sizeof bad); bad[4] = KADP_VERSION + 1;
    CHECK(header_decode(bad, d) == DSM_EPROTO);
    Header big; big.payload_len = KADP_MAX_PAYLOAD;
    header_encode(big, bad);
    CHECK(header_decode(bad, d) == DSM_OK);
    big.payload_len = KADP_MAX_PAYLOAD + 1;
    header_encode(big, bad);
    CHECK(header_decode(bad, d) == DSM_EPROTO);

    // Whole message: build, split as a receiver would (40 B, then payload_len B), verify.
    std::vector<uint8_t> payload(4096);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = static_cast<uint8_t>(i * 7);
    Header req; req.type = PAGE_DATA; req.seq = 5; req.page_id = 600;
    std::vector<uint8_t> msg;
    CHECK(frame_build(req, payload.data(), payload.size(), msg) == DSM_OK);
    CHECK(msg.size() == KADP_HDR_LEN + 4096);
    Header got;
    CHECK(header_decode(msg.data(), got) == DSM_OK);
    CHECK(got.payload_len == 4096 && got.checksum == fletcher32(payload.data(), 4096));
    CHECK(payload_verify(got, msg.data() + KADP_HDR_LEN) == DSM_OK);
    CHECK(memcmp(msg.data() + KADP_HDR_LEN, payload.data(), 4096) == 0);
    msg[KADP_HDR_LEN + 100] ^= 0x10;  // corrupted in transit
    CHECK(payload_verify(got, msg.data() + KADP_HDR_LEN) == DSM_EPROTO);

    // No payload: checksum is 0, and a header that claims otherwise is rejected.
    CHECK(frame_build(req, nullptr, 0, msg) == DSM_OK && msg.size() == KADP_HDR_LEN);
    CHECK(header_decode(msg.data(), got) == DSM_OK && got.payload_len == 0 && got.checksum == 0);
    CHECK(payload_verify(got, nullptr) == DSM_OK);
    got.checksum = 1;
    CHECK(payload_verify(got, nullptr) == DSM_EPROTO);

    // frame_build argument checks.
    std::vector<uint8_t> tmp(KADP_MAX_PAYLOAD + 1);
    CHECK(frame_build(req, tmp.data(), tmp.size(), msg) == DSM_EINVAL);
    CHECK(frame_build(req, nullptr, 10, msg) == DSM_EINVAL);
    CHECK(frame_build(req, tmp.data(), KADP_MAX_PAYLOAD, msg) == DSM_OK);

    // Payload fields: big-endian writer/reader round trip and overrun protection.
    std::vector<uint8_t> buf;
    ByteWriter w(buf);
    w.u8(0xAB); w.u16(0x1234); w.u32(0xA1B2C3D4); w.u64(0x0102030405060708ULL);
    w.bytes("hi", 2);
    CHECK(buf.size() == 1 + 2 + 4 + 8 + 2);
    CHECK(buf[1] == 0x12 && buf[2] == 0x34 && buf[3] == 0xA1 && buf[6] == 0xD4 && buf[7] == 0x01 && buf[14] == 0x08);
    ByteReader r(buf.data(), buf.size());
    char two[2];
    CHECK(r.u8() == 0xAB && r.u16() == 0x1234 && r.u32() == 0xA1B2C3D4);
    CHECK(r.u64() == 0x0102030405060708ULL && r.bytes(two, 2) && memcmp(two, "hi", 2) == 0);
    CHECK(r.ok() && r.remaining() == 0);
    CHECK(r.u32() == 0 && !r.ok());  // overrun: zero and the flag turns false
    ByteReader shortr(buf.data(), 3);
    shortr.u16();
    CHECK(shortr.ok());
    CHECK(shortr.u16() == 0 && !shortr.ok());

    if (g_failed) { fprintf(stderr, "%d check(s) failed\n", g_failed); return 1; }
    puts("test_protocol OK");
    return 0;
}
