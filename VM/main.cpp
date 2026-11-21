#include "bus.h"
#include "cpu.h"

#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstring>

static void usage()
{
    std::fputs("Usage: rs-vm --firmware FILE --disk FILE [--max-steps N]\n", stderr);
}

struct Options
{
    const char *firmware, *disk;
    uint64_t max_steps;
};

static bool parse_options(int argc, char **argv, Options *options)
{
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) {
            return false;
        }
        const char *name = argv[i];
        const char *value = argv[i + 1];
        if (std::strcmp(name, "--firmware") == 0 && !options->firmware) {
            options->firmware = value;
        } else if (std::strcmp(name, "--disk") == 0 && !options->disk) {
            options->disk = value;
        } else if (std::strcmp(name, "--max-steps") == 0 && !options->max_steps) {
            const char *end = value + std::strlen(value);
            auto result = std::from_chars(value, end, options->max_steps);
            if (result.ec != std::errc{} || result.ptr != end || !options->max_steps) {
                return false;
            }
        } else {
            return false;
        }
    }
    return options->firmware && options->disk;
}

static bool mount_images(Bus *bus, const Options *options)
{
    std::ifstream firmware(options->firmware, std::ios::binary | std::ios::ate);
    std::streamoff length = firmware.tellg();
    if (length <= 0 || length > static_cast<std::streamoff>(bus->ram.size())) {
        std::fprintf(stderr, "Cannot load firmware (missing, empty, or too large): %s\n", options->firmware);
        return false;
    }
    firmware.seekg(0);
    if (!firmware.read(reinterpret_cast<char *>(bus->ram.data()), length)) {
        std::fprintf(stderr, "Cannot read firmware: %s\n", options->firmware);
        return false;
    }
    bus->disk.open(options->disk, std::ios::binary | std::ios::ate);
    length = bus->disk.tellg();
    if (length < 512 || length % 512 != 0) {
        std::fprintf(stderr, "Disk must be a readable image of whole 512-byte sectors: %s\n", options->disk);
        return false;
    }
    bus->sectors = static_cast<uint64_t>(length) / 512;
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }
    Options options{};
    if (!parse_options(argc, argv, &options)) {
        usage();
        return 2;
    }
    Bus bus{};
    bus.ram.resize(RAM_SIZE);
    if (!mount_images(&bus, &options)) {
        return 1;
    }
    if (!console_start(&bus.console)) {
        std::fputs("Cannot initialize serial console.\n", stderr);
        return 1;
    }
    Cpu cpu{};
    cpu.pc = RESET_PC;
    uint64_t steps = 0;
    while (!bus.stopped && !console_interrupted()) {
        if (options.max_steps && steps == options.max_steps) {
            std::fprintf(stderr, "Instruction budget exhausted at pc=0x%" PRIx64 "\n", cpu.pc);
            bus.exit_code = 2;
            break;
        }
        cpu_step(&cpu, &bus);
        ++steps;
        // M 态取指使用物理地址。板上地址 0 无存储器，继续取指只会覆盖首次故障的 CSR。
        if (cpu.trapped && cpu.privilege == MACHINE && cpu.pc == 0) {
            std::fprintf(stderr, "Unhandled machine trap: mcause=0x%" PRIx64
                         " mepc=0x%" PRIx64 " mtval=0x%" PRIx64 "\n",
                         cpu.mcause, cpu.mepc, cpu.mtval);
            bus.exit_code = 1;
            break;
        }
    }
    console_stop(&bus.console);
    if (console_interrupted()) {
        return 130;
    }
    if (bus.stopped && bus.exit_code == 2) {
        std::fputs("Serial input ended while the guest was waiting for a command.\n", stderr);
    }
    return bus.exit_code;
}
