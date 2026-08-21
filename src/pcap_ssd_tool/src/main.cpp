#include "capture_reader.hpp"
#include "ssd_writer_pool.hpp"

#include <openssl/evp.h>
#include <fstream>
#include <vector>
#include <array>
#include <stdexcept>
#include <iostream>
#include <cstdint>
#include <string>

constexpr size_t DIGEST_SIZE = 32;
using Digest = std::array<uint8_t, DIGEST_SIZE>;

static bool digests_equal(const Digest& a, const Digest& b) { return a == b; }

static std::string digest_to_hex(const Digest& d) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(DIGEST_SIZE * 2);
    for (uint8_t b : d) { out.push_back(hex[b >> 4]); out.push_back(hex[b & 0x0F]); }
    return out;
}

static void append_u32(std::vector<uint8_t>& buf, uint32_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(v));
}
static void append_u16(std::vector<uint8_t>& buf, uint16_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(v));
}
static void append_i32(std::vector<uint8_t>& buf, int32_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(v));
}

static std::vector<uint8_t> make_global_header(uint32_t snaplen, uint32_t linktype) {
    std::vector<uint8_t> buf;
    buf.reserve(24);
    append_u32(buf, 0xa1b2c3d4u); // magic (microsecond-resolution timestamps)
    append_u16(buf, 2);           // version_major
    append_u16(buf, 4);           // version_minor
    append_i32(buf, 0);           // thiszone (GMT offset, always 0 in practice)
    append_u32(buf, 0);           // sigfigs  (accuracy of timestamps, always 0)
    append_u32(buf, snaplen);
    append_u32(buf, linktype);
    return buf;
}

static std::vector<uint8_t> make_packet_header(const struct pcap_pkthdr* hdr) {
    std::vector<uint8_t> buf;
    buf.reserve(16);
    append_u32(buf, static_cast<uint32_t>(hdr->ts.tv_sec));
    append_u32(buf, static_cast<uint32_t>(hdr->ts.tv_usec));
    append_u32(buf, hdr->caplen);
    append_u32(buf, hdr->len);
    return buf;
}

static Digest hash_whole_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("failed to open " + path);

    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);

    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(buf.data()), size)) {
        throw std::runtime_error("failed to read " + path);
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    Digest digest{};
    unsigned int out_len = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, buf.data(), buf.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest.data(), &out_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA-256 hashing failed for " + path);
    }
    EVP_MD_CTX_free(ctx);

    if (out_len != DIGEST_SIZE) throw std::runtime_error("unexpected digest length");
    return digest;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " <input.pcap> <ssd_output_path> <num_writers>\n";
        return 1;
    }
    const std::string pcap_path = argv[1];
    const std::string ssd_path  = argv[2];
    const size_t num_writers    = std::stoul(argv[3]);

    try {
        CaptureReader capture(pcap_path);
        SsdWriterPool pool(ssd_path, num_writers);

        EVP_MD_CTX* ctx_a = EVP_MD_CTX_new();
        if (!ctx_a || EVP_DigestInit_ex(ctx_a, EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("failed to init hash for source payloads");
        }

        off_t running_offset = 0;
        uint64_t req_id = 0;

        {
            std::vector<uint8_t> ghdr = make_global_header(capture.snaplen(), capture.linktype());
            if (EVP_DigestUpdate(ctx_a, ghdr.data(), ghdr.size()) != 1) {
                throw std::runtime_error("hash update failed (global header)");
            }

            PacketTask task;
            task.req_id = req_id++;
            task.offset = running_offset;
            running_offset += static_cast<off_t>(ghdr.size());
            task.data = std::move(ghdr);

            pool.dispatch(std::move(task));
        }

        while (auto pkt = capture.next()) {
            std::vector<uint8_t> phdr = make_packet_header(pkt->hdr);

            if (EVP_DigestUpdate(ctx_a, phdr.data(), phdr.size()) != 1 ||
                EVP_DigestUpdate(ctx_a, pkt->data, pkt->hdr->caplen) != 1) {
                throw std::runtime_error("hash update failed");
            }

            PacketTask task;
            task.req_id = req_id++;
            task.offset = running_offset;

            task.data.reserve(phdr.size() + pkt->hdr->caplen);
            task.data.insert(task.data.end(), phdr.begin(), phdr.end());
            task.data.insert(task.data.end(), pkt->data, pkt->data + pkt->hdr->caplen);

            running_offset += static_cast<off_t>(task.data.size());

            pool.dispatch(std::move(task));
        }

        pool.finish();
        pool.join();
        if (pool.has_error()) {
            throw std::runtime_error("one or more writes to SSD failed");
        }
        pool.sync_and_close();

        Digest digest_a{};
        unsigned int out_len = 0;
        if (EVP_DigestFinal_ex(ctx_a, digest_a.data(), &out_len) != 1 || out_len != DIGEST_SIZE) {
            EVP_MD_CTX_free(ctx_a);
            throw std::runtime_error("failed to finalize source hash");
        }
        EVP_MD_CTX_free(ctx_a);

        Digest digest_b = hash_whole_file(ssd_path);

        if (digests_equal(digest_a, digest_b)) {
            std::cout << "OK: " << running_offset << " bytes written across "
                      << req_id << " packets\n";
            return 0;
        } else {
            std::cerr << "MISMATCH: hash(A)=" << digest_to_hex(digest_a)
                      << " hash(B)=" << digest_to_hex(digest_b) << "\n";
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}