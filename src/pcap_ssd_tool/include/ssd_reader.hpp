#pragma once
#include <string>
#include <sys/types.h>

class SsdReader {
public:
    explicit SsdReader(const std::string &path);
    ~SsdReader();
    ssize_t read(void *buf, size_t len, off_t offset);

private:
    int fd_ = -1;
};