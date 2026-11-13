#include <mach/mach.h>
#include <mach/host_info.h>
#include <mach/vm_statistics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *what) {
    if (!condition) { fprintf(stderr, "Mach host failed: %s\n", what); exit(1); }
}
static void check_vm(host_t host, int wide, mach_msg_type_number_t capacity, mach_msg_type_number_t expected) {
    unsigned char bytes[sizeof(vm_statistics64_data_t) + 16];
    memset(bytes, 0xa5, sizeof bytes);
    mach_msg_type_number_t count = capacity;
    kern_return_t result = wide
        ? host_statistics64(host, HOST_VM_INFO64, (host_info64_t)bytes, &count)
        : host_statistics(host, HOST_VM_INFO, (host_info_t)bytes, &count);
    require(result == KERN_SUCCESS && count == expected, "VM structure revision count");
    const unsigned *pages = (const unsigned *)bytes;
    require((unsigned long long)pages[0] + pages[1] + pages[2] + pages[3] > 0, "real memory page counts");
    for (size_t i = count * sizeof(integer_t); i < sizeof bytes; ++i)
        require(bytes[i] == 0xa5, "VM write stays within returned revision");
}
int main(void) {
    const host_t host = mach_host_self();
    struct { vm_size_t size; unsigned long guard; } page = {0, 0xabcdef9876543210UL};
    require(host_page_size(host, &page.size) == KERN_SUCCESS, "host page size");
    require(page.size == vm_page_size && page.size > 0 && !(page.size & (page.size - 1)), "page size matches VM exports");
    require(page.guard == 0xabcdef9876543210UL, "page size output bounds");
    require(_host_page_size(host, &page.size) == KERN_SUCCESS, "MIG page size alias");
    require(host_page_size(MACH_PORT_NULL, &page.size) == KERN_INVALID_ARGUMENT, "invalid host");
    host_basic_info_data_t basic = {0};
    mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
    require(host_info(host, HOST_BASIC_INFO, (host_info_t)&basic, &count) == KERN_SUCCESS && count == HOST_BASIC_INFO_COUNT,
            "host basic SDK layout");
    require(basic.avail_cpus > 0 && basic.max_cpus >= basic.avail_cpus && basic.max_mem > 0 &&
            basic.logical_cpu == basic.avail_cpus && basic.memory_size <= 0x80000000U, "host hardware values");
#if defined(__aarch64__)
    require(basic.cpu_type == CPU_TYPE_ARM64, "guest ARM64 host identity");
#else
    require(basic.cpu_type == CPU_TYPE_X86_64, "guest x86_64 host identity");
#endif
    check_vm(host, 0, HOST_VM_INFO_COUNT, HOST_VM_INFO_COUNT);
    check_vm(host, 0, HOST_VM_INFO_REV1_COUNT, HOST_VM_INFO_REV1_COUNT);
    check_vm(host, 0, HOST_VM_INFO_REV0_COUNT, HOST_VM_INFO_REV0_COUNT);
    check_vm(host, 1, HOST_VM_INFO64_COUNT, HOST_VM_INFO64_COUNT);
    check_vm(host, 1, HOST_VM_INFO64_REV1_COUNT, HOST_VM_INFO64_REV1_COUNT);
    check_vm(host, 1, HOST_VM_INFO64_REV0_COUNT, HOST_VM_INFO64_REV0_COUNT);
    host_cpu_load_info_data_t cpu = {0};
    count = HOST_CPU_LOAD_INFO_COUNT;
    require(host_statistics(host, HOST_CPU_LOAD_INFO, (host_info_t)&cpu, &count) == KERN_SUCCESS &&
            count == HOST_CPU_LOAD_INFO_COUNT, "CPU tick SDK layout");
    require((unsigned long long)cpu.cpu_ticks[CPU_STATE_USER] + cpu.cpu_ticks[CPU_STATE_SYSTEM] +
            cpu.cpu_ticks[CPU_STATE_IDLE] + cpu.cpu_ticks[CPU_STATE_NICE] > 0, "actual CPU tick counters");
    host_load_info_data_t load;
    count = HOST_LOAD_INFO_COUNT;
    require(host_statistics(host, HOST_LOAD_INFO, (host_info_t)&load, &count) == KERN_SUCCESS &&
            count == HOST_LOAD_INFO_COUNT && load.avenrun[0] >= 0, "system load averages");
    unsigned words[48];
    memset(words, 0xa5, sizeof words);
    count = HOST_VM_INFO64_REV0_COUNT - 1;
    require(host_statistics64(host, HOST_VM_INFO64, (host_info64_t)words, &count) == KERN_INVALID_ARGUMENT,
            "reject short VM buffer");
    require(words[0] == 0xa5a5a5a5U && count == HOST_VM_INFO64_REV0_COUNT - 1, "failed VM query preserves outputs");
    count = 48;
    require(host_statistics(host, -1, (host_info_t)words, &count) == KERN_INVALID_ARGUMENT, "unsupported statistics flavor");
    puts("SDK Mach host page size, hardware, VM revisions, CPU/load counters and bounds passed");
    return 0;
}
