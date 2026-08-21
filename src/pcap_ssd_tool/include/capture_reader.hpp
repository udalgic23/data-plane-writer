#pragma once

#include <optional>
#include <pcap.h>
#include <string>

struct PcapView {
    const struct pcap_pkthdr *hdr;
    const u_char *data;
};

class CaptureReader {
public:
    explicit CaptureReader(const std::string &pcap_path);
    ~CaptureReader();

    std::optional<PcapView> next();

    uint32_t snaplen() const;
    uint32_t linktype() const;

private:
    pcap_t *pcap_ = nullptr;
};