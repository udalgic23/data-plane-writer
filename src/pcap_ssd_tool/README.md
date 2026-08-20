rm -rf build && mkdir build && cd build
cmake ..
make
./pcap_ssd_tool ../fixtures/sample.pcap ../fixtures/output.pcap 4
