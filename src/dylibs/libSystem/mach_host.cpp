#include "internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>

namespace {
std::uint64_t page_size() { return static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)); }
std::uint32_t narrow(std::uint64_t value) {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(value, UINT32_MAX));
}
std::map<std::string, std::uint64_t> counters(const char *path, bool kilobytes) {
    std::ifstream file(path);
    std::map<std::string, std::uint64_t> result;
    std::string line;
    while (std::getline(file, line)) {
        char name[64]; unsigned long long value;
        if (std::sscanf(line.c_str(), "%63s %llu", name, &value) != 2) continue;
        const auto size = std::strlen(name);
        if (size && name[size - 1] == ':') name[size - 1] = 0;
        result[name] = value * (kilobytes ? 1024 : 1);
    }
    return result;
}
/* MacOSX26.1.sdk mach/vm_statistics.h: nine 64-bit counters follow the
 * four page counts; later revisions extend the tail, in 32-bit words. */
struct vm_info64 {
    std::uint32_t free, active, inactive, wired;
    std::uint64_t zero_fill, reactivations, pageins, pageouts, faults, cow_faults, lookups, hits, purges;
    std::uint32_t purgeable, speculative;
    std::uint64_t decompressions, compressions, swapins, swapouts;
    std::uint32_t compressor_pages, throttled, external_pages, internal_pages;
    std::uint64_t total_uncompressed, swapped;
};
static_assert(sizeof(vm_info64) == 160 && offsetof(vm_info64, decompressions) == 96 &&
              offsetof(vm_info64, swapped) == 152);
bool memory_info(vm_info64 &out) {
    auto memory = counters("/proc/meminfo", true);
    if (!memory.contains("MemTotal")) return false;
    auto vm = counters("/proc/vmstat", false);
    const auto page = page_size();
    out.free = narrow(memory["MemFree"] / page);
    out.active = narrow(memory["Active"] / page);
    out.inactive = narrow(memory["Inactive"] / page);
    out.wired = narrow(memory["Unevictable"] / page);
    out.pageins = vm["pgpgin"] * 1024 / page;
    out.pageouts = vm["pgpgout"] * 1024 / page;
    out.faults = vm["pgfault"];
    out.swapins = vm["pswpin"]; out.swapouts = vm["pswpout"];
    out.external_pages = narrow((memory["Cached"] + memory["Buffers"]) / page);
    out.internal_pages = narrow(memory["AnonPages"] / page);
    /* Linux has no equivalent of Mach object-cache, speculative-page or
     * compressor counters. Their fields stay zero; page categories above
     * are Linux's corresponding accounting, not a fabricated XNU snapshot. */
    return true;
}
int copy_info(void *out, unsigned *count, const void *value, unsigned words) {
    if (!out || !count || *count < words) return 4;
    std::memcpy(out, value, words * sizeof(std::uint32_t));
    *count = words;
    return 0;
}
int statistics(unsigned host, int flavor, void *out, unsigned *count, bool wide) {
    if (host != 2 || !out || !count) return 4;
    if (flavor == 1) { // HOST_LOAD_INFO, LOAD_SCALE == 1000
        double loads[3];
        if (::getloadavg(loads, 3) != 3) return 5;
        std::array<std::int32_t, 6> values{};
        const double cpus = static_cast<double>(::sysconf(_SC_NPROCESSORS_ONLN));
        for (int i = 0; i < 3; ++i) {
            values[i] = static_cast<std::int32_t>(std::min(loads[i] * 1000, static_cast<double>(INT32_MAX)));
            const auto factor = loads[i] < cpus ? cpus - loads[i] : cpus / (loads[i] + 1);
            values[i + 3] = static_cast<std::int32_t>(std::min(factor * 1000, static_cast<double>(INT32_MAX)));
        }
        return copy_info(out, count, values.data(), values.size());
    }
    if (flavor == 3) { // HOST_CPU_LOAD_INFO: user, system, idle, nice
        std::ifstream file("/proc/stat");
        std::string name;
        std::uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
        if (!(file >> name >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) || name != "cpu") return 5;
        const auto ticks = static_cast<std::uint64_t>(::sysconf(_SC_CLK_TCK));
        const std::array<std::uint64_t, 4> totals = {user, system + irq + softirq, idle + iowait + steal, nice};
        std::array<std::uint32_t, 4> values;
        for (std::size_t i = 0; i < values.size(); ++i) values[i] = static_cast<std::uint32_t>(totals[i] * 100 / ticks);
        return copy_info(out, count, values.data(), values.size());
    }
    if ((!wide && flavor == 2) || (wide && flavor == 4)) {
        vm_info64 memory{};
        if (!memory_info(memory)) return 5;
        if (wide) {
            const auto words = *count >= 40 ? 40u : *count >= 38 ? 38u : 24u;
            return copy_info(out, count, &memory, words);
        }
        const std::array<std::uint32_t, 15> values = {
            memory.free, memory.active, memory.inactive, memory.wired,
            narrow(memory.zero_fill), narrow(memory.reactivations), narrow(memory.pageins), narrow(memory.pageouts),
            narrow(memory.faults), narrow(memory.cow_faults), narrow(memory.lookups), narrow(memory.hits),
            memory.purgeable, narrow(memory.purges), memory.speculative};
        const auto words = *count >= 15 ? 15u : *count >= 14 ? 14u : 12u;
        return copy_info(out, count, values.data(), words);
    }
    return 4;
}
}

extern "C" {
int darwin_host_page_size(unsigned, std::uint64_t *) DARWIN(host_page_size);
int darwin_host_page_size(unsigned host, std::uint64_t *size) {
    if (host != 2 || !size) return 4;
    *size = page_size();
    return 0;
}
int darwin_mig_host_page_size(unsigned, std::uint64_t *) DARWIN(_host_page_size);
int darwin_mig_host_page_size(unsigned host, std::uint64_t *size) { return darwin_host_page_size(host, size); }
int darwin_host_statistics(unsigned, int, void *, unsigned *) DARWIN(host_statistics);
int darwin_host_statistics(unsigned host, int flavor, void *out, unsigned *count) {
    return statistics(host, flavor, out, count, false);
}
int darwin_host_statistics64(unsigned, int, void *, unsigned *) DARWIN(host_statistics64);
int darwin_host_statistics64(unsigned host, int flavor, void *out, unsigned *count) {
    return statistics(host, flavor, out, count, true);
}
int darwin_host_info(unsigned, int, void *, unsigned *) DARWIN(host_info);
int darwin_host_info(unsigned host, int flavor, void *out, unsigned *count) {
    if (host != 2 || flavor != 1 || !out || !count || *count < 5) return 4;
    const auto memory = counters("/proc/meminfo", true);
    const auto found = memory.find("MemTotal");
    if (found == memory.end()) return 5;
    const auto api = mm_libsystem_api();
    const auto header = api ? static_cast<const mm_mach_header_64 *>(api->main_image) : nullptr;
    std::array<std::uint32_t, 12> values{};
    values[0] = static_cast<unsigned>(::sysconf(_SC_NPROCESSORS_CONF));
    values[1] = static_cast<unsigned>(::sysconf(_SC_NPROCESSORS_ONLN));
    values[2] = static_cast<std::uint32_t>(std::min<std::uint64_t>(found->second, 2ULL * 1024 * 1024 * 1024));
    values[3] = header ? header->cputype : MM_CPU_TYPE_X86_64;
    values[4] = header ? header->cpusubtype : 3;
    // Expose the available host execution slots as guest logical/physical CPUs.
    values[6] = values[8] = values[1]; values[7] = values[9] = values[0];
    std::memcpy(values.data() + 10, &found->second, sizeof found->second);
    return copy_info(out, count, values.data(), *count >= values.size() ? values.size() : 5);
}
}
