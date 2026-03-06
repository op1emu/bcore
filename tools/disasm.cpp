#include <elf.h>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "core.h"

// Lightweight read-only memory wrapper for per-section disassembly.
// Only raw(), base(), size() are used by Core::disassemble().
class SectionMemory : public Memory {
public:
    SectionMemory(const uint8_t* data, uint32_t base, uint32_t size)
        : data_(data), base_(base), size_(size) {}

    uint32_t base() const override { return base_; }
    uint32_t size() const override { return size_; }

    uint8_t read8(uint32_t a) const override {
        return data_[a - base_];
    }
    uint16_t read16(uint32_t a) const override {
        uint32_t o = a - base_;
        return data_[o] | (static_cast<uint16_t>(data_[o + 1]) << 8);
    }
    uint32_t read32(uint32_t a) const override {
        uint32_t o = a - base_;
        return data_[o] | (static_cast<uint32_t>(data_[o + 1]) << 8)
             | (static_cast<uint32_t>(data_[o + 2]) << 16)
             | (static_cast<uint32_t>(data_[o + 3]) << 24);
    }
    void write8(uint32_t, uint8_t) override {}
    void write16(uint32_t, uint16_t) override {}
    void write32(uint32_t, uint32_t) override {}
    const uint8_t* raw() const override { return data_; }
    uintptr_t fast_base() const override {
        return reinterpret_cast<uintptr_t>(data_) - base_;
    }

private:
    const uint8_t* data_;
    uint32_t base_;
    uint32_t size_;
};

static bool read_file(const std::string &path, std::vector<uint8_t> &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	f.seekg(0, std::ios::end);
	std::streampos len = f.tellg();
	if (len <= 0) return false;
	out.resize(static_cast<size_t>(len));
	f.seekg(0, std::ios::beg);
	f.read(reinterpret_cast<char*>(out.data()), len);
	return f.good();
}

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: disasm <elf-file>\n";
		return 1;
	}

	std::vector<uint8_t> buf;
	if (!read_file(argv[1], buf)) {
		std::cerr << "failed to read file\n";
		return 1;
	}
	if (buf.size() < sizeof(Elf32_Ehdr)) {
		std::cerr << "file too small\n";
		return 1;
	}

	auto *eh = reinterpret_cast<const Elf32_Ehdr*>(buf.data());
	if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
	    eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3 ||
	    eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_ident[EI_DATA] != ELFDATA2LSB) {
		std::cerr << "unsupported ELF\n";
		return 1;
	}

	const uint32_t shoff = eh->e_shoff;
	const uint32_t shentsize = eh->e_shentsize;
	const uint32_t shnum = eh->e_shnum;
	const uint32_t shstrndx = eh->e_shstrndx;

	if (shoff + static_cast<uint32_t>(shentsize) * shnum > buf.size()) {
		std::cerr << "bad section header table\n";
		return 1;
	}

	auto sec = [&](uint16_t idx) -> const Elf32_Shdr* {
		return reinterpret_cast<const Elf32_Shdr*>(buf.data() + shoff + static_cast<uint64_t>(idx) * shentsize);
	};

	const Elf32_Shdr *shstr = sec(shstrndx);
	if (shstr->sh_offset + shstr->sh_size > buf.size()) {
		std::cerr << "bad shstrtab\n";
		return 1;
	}
	const char *strtab = reinterpret_cast<const char*>(buf.data() + shstr->sh_offset);

	// Collect all executable sections
	std::vector<std::pair<std::string, const Elf32_Shdr*>> exec_sections;
	for (uint16_t i = 0; i < shnum; ++i) {
		const Elf32_Shdr *s = sec(i);
		if ((s->sh_flags & SHF_EXECINSTR) && s->sh_name < shstr->sh_size) {
			const char *name = strtab + s->sh_name;
			exec_sections.push_back({std::string(name), s});
		}
	}

	if (exec_sections.empty()) {
		std::cerr << "no executable sections found\n";
		return 1;
	}

	// Sort sections by (address, size) so that smaller sections come first at each address
	std::sort(exec_sections.begin(), exec_sections.end(),
		[](const auto& a, const auto& b) {
			if (a.second->sh_addr != b.second->sh_addr)
				return a.second->sh_addr < b.second->sh_addr;
			return a.second->sh_size < b.second->sh_size;  // Smaller sizes first
		});

	// Disassemble each executable section, tracking covered addresses
	std::set<uint32_t> covered_addresses;

	for (const auto& [section_name, section] : exec_sections) {
		if (section->sh_offset + section->sh_size > buf.size()) {
			std::cerr << "bad section bounds: " << section_name << "\n";
			continue;
		}

		const uint8_t *data = buf.data() + section->sh_offset;
		std::cout << section_name << " size: " << section->sh_size << " bytes\n";

		// Find the last non-zero byte to avoid outputting trailing zeros/nops
		uint64_t effective_size = section->sh_size;
		while (effective_size > 0 && data[effective_size - 1] == 0) {
			effective_size--;
		}

		SectionMemory sec_mem(data, section->sh_addr, section->sh_size);
		CpuState dummy_cpu;
		cpu_state_init(&dummy_cpu);
		Core core(&dummy_cpu, &sec_mem);

		for (uint64_t i = 0; i < effective_size; ) {
			uint32_t addr = section->sh_addr + i;

			// Skip addresses that were already covered by previous sections
			if (covered_addresses.count(addr)) {
				i += 2;  // Skip at least 2 bytes
				continue;
			}

			uint16_t instruction = data[i] | (static_cast<uint16_t>(data[i + 1]) << 8);

			std::cout << std::hex << std::setw(8) << std::setfill('0') << addr
					  << ": " << std::setw(4) << std::setfill('0') << instruction << "  ";

			auto [text, next_pc] = core.disassemble(addr);
			std::cout << text << "\n";

			uint32_t insn_len = next_pc - addr;
			// Mark all bytes of this instruction as covered
			for (uint32_t j = 0; j < insn_len; ++j) {
				covered_addresses.insert(addr + j);
			}
			i += insn_len;
		}
		std::cout << "\n";
	}
	std::cout << std::dec;
	return 0;
}
