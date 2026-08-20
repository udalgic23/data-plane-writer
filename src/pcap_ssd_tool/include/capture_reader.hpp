#pragma once

#include <pcap.h>
#include <string>
#include <optional>

struct PcapView {
    const struct pcap_pkthdr* hdr;
    const u_char* data;
};

class CaptureReader {
public:
    explicit CaptureReader(const std::string& pcap_path);
    ~CaptureReader();

    std::optional<PcapView> next();

private:
    pcap_t* pcap_ = nullptr;
};