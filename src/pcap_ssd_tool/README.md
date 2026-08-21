# pcap_ssd_tool

Reads packets from a `.pcap` file and writes them to a target file (typically
an SSD) using a pool of writer threads that issue offset-addressed `pwrite()`
calls in parallel, rather than a single sequential stream.

This directory has two binaries:

- **`pcap_ssd_tool`** — the actual conversion tool: pcap in, pcap out, with an
  integrity check.
- **`bench_ssd_writer`** — a standalone benchmark of the write path only
  (`SsdWriterPool`), used to measure raw storage performance independent of
  pcap parsing.

---

## Build

```bash
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
```

Requires `libpcap-dev`, `libssl-dev` (OpenSSL), and a C++17 compiler. Both
binaries are produced in `build/`.

---

## `pcap_ssd_tool`

Converts a pcap file by reading it with libpcap and re-writing it — global
header, per-packet headers, and payloads — to a new location using the
parallel writer pool.

### Usage

```bash
./pcap_ssd_tool <input.pcap> <output_path> <num_writers>
```

| Argument      | Meaning                                                        |
|---------------|------------------------------------------------------------------|
| `input.pcap`  | Source pcap file to read                                       |
| `output_path` | Destination file (created/truncated)                           |
| `num_writers` | Number of writer threads in the `SsdWriterPool`                |

### Example

```bash
./pcap_ssd_tool ../fixtures/sample.pcap /tmp/output.pcap 4
```

```
OK: 460237 bytes written across 1001 packets
```

### What it does

1. Opens `input.pcap` with `pcap_open_offline()` and reads packets one at a
   time via `pcap_next_ex()`.
2. For each packet, serializes a 16-byte pcap record header (timestamp,
   caplen, len) followed by the packet's payload bytes, and computes the
   offset that block should land at in the output file. A 24-byte pcap
   global header is written first, at offset 0.
3. Dispatches each (header + payload) block to `SsdWriterPool`, which
   round-robins work across `num_writers` threads. Each thread pops tasks
   from its own lock-free SPSC queue and writes them with `pwrite(fd, data,
   size, offset)` — writes land at their correct position regardless of
   which thread or in what order they actually execute, so output ordering
   doesn't depend on thread scheduling.
4. While reading, it also incrementally hashes (SHA-256) the exact byte
   stream it intends to produce (global header, then each per-packet header
   + payload, in logical order).
5. After all writes complete and the file is `fsync`'d and closed, it hashes
   the file back from disk in one pass.
6. Compares the two digests. A match means every byte the parallel writer
   pool was asked to write actually landed at the right offset — no
   dropped, corrupted, or misplaced writes. A mismatch prints both digests
   and exits non-zero.

The output is a standard pcap file — readable by Wireshark, `tcpdump -r`,
or `pcap_open_offline()` — not just a raw dump of packet bytes.

---

## `bench_ssd_writer`

Benchmarks `SsdWriterPool`'s write path **in isolation**: synthetic
fixed-content blocks, no pcap parsing or hashing involved. It exists to
answer questions like "how does write throughput scale with core count?" or
"how much does random access cost us vs. sequential?" without pcap-reading
overhead muddying the numbers.

### Usage

```bash
./bench_ssd_writer <output_path> <core_count> <block_size_bytes> <io_count> <queue_depth> <seq|rand>
```

| Argument           | Meaning                                                                 |
|--------------------|--------------------------------------------------------------------------|
| `output_path`      | File to write to (created/truncated, pre-sized with `posix_fallocate`) |
| `core_count`       | Number of writer threads (maps to `SsdWriterPool`'s `num_writers`)     |
| `block_size_bytes` | Size of each write, in bytes                                           |
| `io_count`         | Total number of write operations to issue                              |
| `queue_depth`      | Max outstanding (in-flight, unacknowledged) writes at any moment       |
| `seq` \| `rand`    | Access pattern — see below                                             |

### Example

```bash
./bench_ssd_writer /tmp/bench.dat 4 4096 50000 32 rand
```

```
core_count=4 block_size=4096B io_count=50000 queue_depth=32 pattern=rand
elapsed:     0.24 s
IOPS:        208333.33
throughput:  813.80 MB/s
latency avg: 71.40 us
latency p50: 45.10 us
latency p95: 131.20 us
latency p99: 448.90 us
latency max: 19855.44 us
```

### What it measures

- **IOPS** — `io_count / elapsed_time`. Total operations completed per
  second across all writer threads combined.
- **Throughput** — `(io_count * block_size) / elapsed_time`, in MB/s.
- **Latency** — per-operation time from *submission* (just before
  `dispatch()` is called) to *completion* (the moment its `pwrite()`
  returns inside the writer thread), reported as avg / p50 / p95 / p99 /
  max, matching how tools like `fio` define per-op latency. This is not
  just "how fast pwrite runs" — it includes any time an op spends waiting
  in queue because `queue_depth` was full.

### How each parameter is implemented

- **`core_count`** is passed straight through as `SsdWriterPool`'s
  `num_writers` — one OS thread per "core," each with its own queue,
  issuing `pwrite()` independently.
- **`block_size_bytes`** sets the size of the synthetic buffer (filled with
  a fixed byte pattern) used for every write task.
- **`io_count`** determines how many block-aligned offsets exist:
  `0, block_size, 2*block_size, ..., (io_count-1)*block_size`.
- **`seq` vs `rand`** controls the *order* those same offsets are submitted
  in — sequential submits them in increasing order; random shuffles them
  first. Either way, every block in the file is written exactly once and
  the total bytes written is identical, so the two modes are directly
  comparable — only access order differs, not the workload's size or
  shape.
- **`queue_depth`** is enforced by the benchmark driver itself:
  `SsdWriterPool` doesn't natively cap in-flight work, so the benchmark
  tracks an atomic in-flight counter (incremented on submit, decremented in
  a completion callback fired by the writer thread right after each
  `pwrite()`) and spins the submitting thread whenever `in_flight >=
  queue_depth`, before handing off the next task. This bounds how many
  writes can be outstanding at once, the same concept as I/O queue depth in
  `fio` or `iostat`.
- The output file is pre-sized with `posix_fallocate()` before the run
  starts, so on-demand extent allocation by the filesystem doesn't get
  counted as part of write latency.

### Sweeping parameters

The binary runs a single configuration per invocation; sweep by looping
over it, e.g.:

```bash
# Core count scaling, sequential 4KB writes
for cores in 1 2 4 8 16; do
  ./bench_ssd_writer /tmp/bench.dat $cores 4096 50000 32 seq
done

# Block size sweep
for bs in 512 4096 65536 1048576; do
  ./bench_ssd_writer /tmp/bench.dat 4 $bs 20000 32 rand
done

# Queue depth sweep
for qd in 1 4 16 64 256; do
  ./bench_ssd_writer /tmp/bench.dat 4 4096 50000 $qd rand
done

# Sequential vs random, held otherwise constant
./bench_ssd_writer /tmp/bench.dat 4 4096 50000 32 seq
./bench_ssd_writer /tmp/bench.dat 4 4096 50000 32 rand
```

### Caveats

- Results are only meaningful when `output_path` points at the actual
  storage device you care about (a real SSD/NVMe drive, not a tmpfs or a
  container overlay filesystem) — the underlying medium dominates these
  numbers far more than any of the tool's own logic.
- The per-task buffer copy (`task.data.assign(...)`) mirrors real
  `dispatch()` usage but does add a small CPU-side cost on top of pure
  storage I/O; at very small block sizes and very high core counts this can
  become a non-trivial fraction of the measured time.
- `queue_depth` bounding uses a busy-spin (`yield()`), which burns CPU on
  the submitting thread while waiting for room — fine for benchmarking, not
  representative of an application that would otherwise block or do useful
  work while waiting.
