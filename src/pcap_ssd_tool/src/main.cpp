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

        while (auto pkt = capture.next()) {
            if (EVP_DigestUpdate(ctx_a, pkt->data, pkt->hdr->caplen) != 1) {
                throw std::runtime_error("hash update failed");
            }

            PacketTask task;
            task.req_id = req_id++;
            task.offset = running_offset;
            task.data.assign(pkt->data, pkt->data + pkt->hdr->caplen);
            running_offset += static_cast<off_t>(pkt->hdr->caplen);

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