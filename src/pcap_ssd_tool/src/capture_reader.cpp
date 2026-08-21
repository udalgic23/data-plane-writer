#include "capture_reader.hpp"

#include <stdexcept>

CaptureReader::CaptureReader(const std::string &pcap_path) {
    char errbuf[PCAP_ERRBUF_SIZE];
    errbuf[0] = '\0';

    pcap_ = pcap_open_offline(pcap_path.c_str(), errbuf);
    if (pcap_ == nullptr) {
        throw std::runtime_error("failed to open pcap file '" + pcap_path + "': " + errbuf);
    }
}

CaptureReader::~CaptureReader() {
    if (pcap_ != nullptr) {
        pcap_close(pcap_);
        pcap_ = nullptr;
    }
}

std::optional<PcapView> CaptureReader::next() {
    struct pcap_pkthdr *hdr = nullptr;
    const u_char *data = nullptr;

    int rc = pcap_next_ex(pcap_, &hdr, &data);

    switch (rc) {
    case 1:
        return PcapView {hdr, data};
    case 0:
        return next();
    case -2:
        return std::nullopt;
    default:
        throw std::runtime_error(std::string("pcap_next_ex error: ") + pcap_geterr(pcap_));
    }
}

uint32_t CaptureReader::snaplen() const {
    return static_cast<uint32_t>(pcap_snapshot(pcap_));
}

uint32_t CaptureReader::linktype() const {
    return static_cast<uint32_t>(pcap_datalink(pcap_));
}