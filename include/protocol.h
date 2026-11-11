// KADP wire format (design document, section 6.2): 40-byte header, big-endian integers,
// Fletcher-32 payload checksum. Everything is serialised field by field; a struct is never
// copied onto the network, because compiler padding could change the layout.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "kadp_types.h"

constexpr size_t KADP_MAX_PAYLOAD = 65536;
constexpr uint16_t NODE_NONE = 0xFFFF;  // src_node of a node that has no ID yet
constexpr uint16_t NODE_ALL = 0xFFFF;   // dst_node meaning "every node"

// Decoded form of the header. This is NOT the wire layout; use the functions below.
struct Header {
    uint8_t type = 0;  // kadp_type: class in the high nibble, code in the low one
    uint16_t flags = 0;  // kadp_flags bits
    uint16_t src_node = NODE_NONE;
    uint16_t dst_node = NODE_ALL;
    uint32_t seq = 0;  // a reply repeats the seq of its request
    uint32_t page_id = 0;
    uint32_t page_version = 0;
    uint32_t payload_len = 0;
    uint32_t checksum = 0;  // Fletcher-32 of the payload, 0 when there is none
    int16_t status = 0;  // 0 or a DSM_E* code in replies
    // magic and version are not stored: encode always writes the current ones and decode
    // rejects anything else.
};

inline uint8_t msg_class(uint8_t type) { return type >> 4; }  // what the listener routes on

// Fletcher-32 over 16-bit words (a trailing odd byte is padded with zero). Empty input gives 0.
uint32_t fletcher32(const void* data, size_t len);

// Writes exactly KADP_HDR_LEN bytes. The 6 reserved bytes are sent as zero.
void header_encode(const Header& h, uint8_t out[KADP_HDR_LEN]);

// DSM_OK, or DSM_EPROTO if magic or version are wrong or payload_len > KADP_MAX_PAYLOAD.
// The caller should close the connection on DSM_EPROTO (framing can no longer be trusted).
int header_decode(const uint8_t in[KADP_HDR_LEN], Header& out);

// Builds a whole message: sets payload_len and checksum from the payload, then appends it.
// DSM_EINVAL if the payload is too large or null with a non-zero length.
int frame_build(Header h, const void* payload, size_t len, std::vector<uint8_t>& out);

// Checks a received payload against h.payload_len and h.checksum. DSM_EPROTO on mismatch
// (the receiver answers KADP_ERROR(DSM_EPROTO) and the sender retries).
int payload_verify(const Header& h, const void* payload);

// Big-endian field writer/reader for payloads. The reader never reads past the end: on
// overrun it returns 0 and ok() turns false, so check ok() once after a group of reads.
class ByteWriter {
public:
    explicit ByteWriter(std::vector<uint8_t>& buf) : buf_(buf) {}
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { u8(v >> 8); u8(v & 0xFF); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
    void u64(uint64_t v) { u32(static_cast<uint32_t>(v >> 32)); u32(static_cast<uint32_t>(v)); }
    void bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }

private:
    std::vector<uint8_t>& buf_;
};

class ByteReader {
public:
    ByteReader(const void* p, size_t n) : p_(static_cast<const uint8_t*>(p)), n_(n) {}
    uint8_t u8() { return static_cast<uint8_t>(be(1)); }
    uint16_t u16() { return static_cast<uint16_t>(be(2)); }
    uint32_t u32() { return static_cast<uint32_t>(be(4)); }
    uint64_t u64() { return be(8); }
    bool bytes(void* dst, size_t n);
    bool ok() const { return ok_; }
    size_t remaining() const { return n_ - pos_; }

private:
    bool take(size_t n) {
        if (!ok_ || n > n_ - pos_) { ok_ = false; return false; }
        pos_ += n;
        return true;
    }
    // All-or-nothing: a read that does not fully fit consumes nothing and returns 0.
    uint64_t be(size_t n) {
        if (!take(n)) return 0;
        uint64_t v = 0;
        for (size_t i = n; i > 0; i--) v = v << 8 | p_[pos_ - i];
        return v;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};
