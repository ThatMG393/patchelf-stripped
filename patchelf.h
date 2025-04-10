#pragma once

#include <memory>
#include <vector>

using FileContents = std::shared_ptr<std::vector<unsigned char>>;

#define ElfFileParams class Elf_Ehdr, class Elf_Phdr, class Elf_Shdr, class Elf_Addr, class Elf_Off, class Elf_Dyn, class Elf_Sym, class Elf_Versym, class Elf_Verdef, class Elf_Verdaux, class Elf_Verneed, class Elf_Vernaux, class Elf_Rel, class Elf_Rela, unsigned ElfClass
#define ElfFileParamNames Elf_Ehdr, Elf_Phdr, Elf_Shdr, Elf_Addr, Elf_Off, Elf_Dyn, Elf_Sym, Elf_Versym, Elf_Verdef, Elf_Verdaux, Elf_Verneed, Elf_Vernaux, Elf_Rel, Elf_Rela, ElfClass

static bool debugMode = false;

template<class I>
constexpr I rdi(I i, bool littleEndian) noexcept {
    I r = 0;
    if (littleEndian) {
        for (unsigned int n = 0; n < sizeof(I); ++n) {
            r |= ((I) *(((unsigned char *) &i) + n)) << (n * 8);
        }
    } else {
        for (unsigned int n = 0; n < sizeof(I); ++n) {
            r |= ((I) *(((unsigned char *) &i) + n)) << ((sizeof(I) - n - 1) * 8);
        }
    }
    return r;
}

static std::vector<std::string> splitColonDelimitedString(std::string_view s) {
    std::vector<std::string> parts;

    size_t pos;
    while ((pos = s.find(':')) != std::string_view::npos) {
        parts.emplace_back(s.substr(0, pos));
        s = s.substr(pos + 1);
    }

    if (!s.empty())
        parts.emplace_back(s);

    return parts;
}

static std::string trim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());

    return s;
}

static std::string downcase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

static void debug(const char * format, ...) {
    if (debugMode) {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}

template<ElfFileParams>
class ElfFile {
	private:
		FileContents fileContents;
};
