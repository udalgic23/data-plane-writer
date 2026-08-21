#pragma once

#include <cstdint>
#include <sys/types.h>
#include <vector>

struct PacketTask {
    uint64_t req_id;
    off_t offset;
    std::vector<uint8_t> data;
};