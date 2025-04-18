#include <sstream>
#include <string>
#include <vector>

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "elf.h"
#include "patchelf.h"
#include "stdlib.hpp"

#ifndef PACKAGE_STRING
#define PACKAGE_STRING "patchelf-stripped"
#endif

static std::vector<std::string> fileNames;
static std::string outputFileName;
static bool alwaysWrite = true;

#ifdef DEFAULT_PAGESIZE
static int forcedPageSize = DEFAULT_PAGESIZE;
#else
static int forcedPageSize = -1;
#endif

static FileContents readFile(
	const std::string & fileName,
    size_t cutOff = std::numeric_limits<size_t>::max()
) {
    struct stat st;
    if (stat(fileName.c_str(), &st) != 0)
		return FileContents();

    if (static_cast<uint64_t>(st.st_size) > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        return FileContents();

    size_t size = std::min(cutOff, static_cast<size_t>(st.st_size));

    FileContents contents = std::make_shared<std::vector<unsigned char>>(size);

    int fd = open(fileName.c_str(), O_RDONLY);
    if (fd == -1) return FileContents();

    size_t bytesRead = 0;
    ssize_t portion;
    while ((portion = read(fd, contents->data() + bytesRead, size - bytesRead)) > 0)
        bytesRead += portion;

    close(fd);

    if (bytesRead != size) return FileContents();

    return contents;
}


typedef struct {
    bool is32Bit;
    int machine; // one of EM_*
} ElfType;

[[nodiscard]] static ElfType getElfType(const FileContents & fileContents) {
    /* Check the ELF header for basic validity. */
    if (fileContents->size() < sizeof(Elf32_Ehdr))
        return ElfType { };

    auto contents = fileContents->data();

    if (memcmp(contents, ELFMAG, SELFMAG) != 0) return ElfType { };

    if (contents[EI_VERSION] != EV_CURRENT) return ElfType { };

    if (contents[EI_CLASS] != ELFCLASS32 && contents[EI_CLASS] != ELFCLASS64)
    	return ElfType { };

    bool is32Bit = contents[EI_CLASS] == ELFCLASS32;

    // FIXME: endianness
    return ElfType { is32Bit, is32Bit ? (reinterpret_cast<Elf32_Ehdr *>(contents))->e_machine : (reinterpret_cast<Elf64_Ehdr *>(contents))->e_machine };
}


static std::string extractString(const FileContents & contents, size_t offset, size_t size) {
    return { reinterpret_cast<const char *>(contents->data()) + offset, size };
}


template<ElfFileParams>
ElfFile<ElfFileParamNames>::ElfFile(
	FileContents fContents
) : fileContents(fContents) {
    /* Check the ELF header for basic validity. */
    if (fileContents->size() < (off_t) sizeof(Elf_Ehdr)) error("missing ELF header");


    if (memcmp(hdr()->e_ident, ELFMAG, SELFMAG) != 0)
        error("not an ELF executable");

    littleEndian = hdr()->e_ident[EI_DATA] == ELFDATA2LSB;

    if (rdi(hdr()->e_type) != ET_EXEC && rdi(hdr()->e_type) != ET_DYN)
        error("wrong ELF type");

    {
        auto ph_offset = rdi(hdr()->e_phoff);
        auto ph_num = rdi(hdr()->e_phnum);
        auto ph_entsize = rdi(hdr()->e_phentsize);
        size_t ph_size, ph_end;

        if (__builtin_mul_overflow(ph_num, ph_entsize, &ph_size)
            || __builtin_add_overflow(ph_offset, ph_size, &ph_end)
            || ph_end > fileContents->size()) {
            error("program header table out of bounds");
        }
    }

    if (rdi(hdr()->e_shnum) == 0)
        error("no section headers. The input file is probably a statically linked, self-decompressing binary");

    {
        auto sh_offset = rdi(hdr()->e_shoff);
        auto sh_num = rdi(hdr()->e_shnum);
        auto sh_entsize = rdi(hdr()->e_shentsize);
        size_t sh_size, sh_end;

        if (__builtin_mul_overflow(sh_num, sh_entsize, &sh_size)
            || __builtin_add_overflow(sh_offset, sh_size, &sh_end)
            || sh_end > fileContents->size()) {
            error("section header table out of bounds");
        }
    }

    if (rdi(hdr()->e_phentsize) != sizeof(Elf_Phdr))
        error("program headers have wrong size");

    /* Copy the program and section headers. */
    for (int i = 0; i < rdi(hdr()->e_phnum); ++i) {
        Elf_Phdr *phdr = (Elf_Phdr *) (fileContents->data() + rdi(hdr()->e_phoff)) + i;

        checkPointer(fileContents, phdr, sizeof(*phdr));
        phdrs.push_back(*phdr);
        if (rdi(phdrs[i].p_type) == PT_INTERP) isExecutable = true;
    }

    for (int i = 0; i < rdi(hdr()->e_shnum); ++i) {
        Elf_Shdr *shdr = (Elf_Shdr *) (fileContents->data() + rdi(hdr()->e_shoff)) + i;

        checkPointer(fileContents, shdr, sizeof(*shdr));
        shdrs.push_back(*shdr);
    }

    /* Get the section header string table section (".shstrtab").  Its
       index in the section header table is given by e_shstrndx field
       of the ELF header. */
    auto shstrtabIndex = rdi(hdr()->e_shstrndx);
    if (shstrtabIndex >= shdrs.size())
        error("string table index out of bounds");

    auto shstrtabSize = rdi(shdrs[shstrtabIndex].sh_size);
    size_t shstrtabptr;
    if (__builtin_add_overflow(reinterpret_cast<size_t>(fileContents->data()), rdi(shdrs[shstrtabIndex].sh_offset), &shstrtabptr))
        error("string table overflow");
    const char *shstrtab = reinterpret_cast<const char *>(shstrtabptr);
    checkPointer(fileContents, shstrtab, shstrtabSize);

    if (shstrtabSize == 0)
        error("string table size is zero");

    if (shstrtab[shstrtabSize - 1] != 0)
        error("string table is not zero terminated");

    sectionNames = std::string(shstrtab, shstrtabSize);

    sectionsByOldIndex.resize(shdrs.size());
    for (size_t i = 1; i < shdrs.size(); ++i)
        sectionsByOldIndex.at(i) = getSectionName(shdrs.at(i));
}

template<ElfFileParams>
unsigned int ElfFile<ElfFileParamNames>::getPageSize() const noexcept {
    if (forcedPageSize > 0)
        return forcedPageSize;

    // Architectures (and ABIs) can have different minimum section alignment
    // requirements. There is no authoritative list of these values. The
    // current list is extracted from GNU gold's source code (abi_pagesize).
    switch (rdi(hdr()->e_machine)) {
      case EM_ALPHA:
      case EM_IA_64:
      case EM_MIPS:
      case EM_PPC:
      case EM_PPC64:
      case EM_AARCH64:
      case EM_TILEGX:
      case EM_LOONGARCH:
        return 0x10000;
      case EM_SPARC: // This should be sparc 32-bit. According to the linux
                     // kernel 4KB should be also fine, but it seems that solaris is doing 8KB
      case EM_SPARCV9: /* SPARC64 support */
        return 0x2000;
      default:
        return 0x1000;
    }
}

template<ElfFileParams>
void ElfFile<ElfFileParamNames>::sortPhdrs() {
    /* Sort the segments by offset. */
    CompPhdr comp;
    comp.elfFile = this;
    stable_sort(phdrs.begin(), phdrs.end(), comp);
}

template<ElfFileParams>
void ElfFile<ElfFileParamNames>::sortShdrs() {
    /* Translate sh_link mappings to section names, since sorting the
       sections will invalidate the sh_link fields. */
    std::map<SectionName, SectionName> linkage;
    for (unsigned int i = 1; i < rdi(hdr()->e_shnum); ++i)
        if (rdi(shdrs.at(i).sh_link) != 0)
            linkage[getSectionName(shdrs.at(i))] = getSectionName(shdrs.at(rdi(shdrs.at(i).sh_link)));

    /* Idem for sh_info on certain sections. */
    std::map<SectionName, SectionName> info;
    for (unsigned int i = 1; i < rdi(hdr()->e_shnum); ++i)
        if (rdi(shdrs.at(i).sh_info) != 0 &&
            (rdi(shdrs.at(i).sh_type) == SHT_REL || rdi(shdrs.at(i).sh_type) == SHT_RELA))
            info[getSectionName(shdrs.at(i))] = getSectionName(shdrs.at(rdi(shdrs.at(i).sh_info)));

    /* Idem for the index of the .shstrtab section in the ELF header. */
    Elf_Shdr shstrtab = shdrs.at(rdi(hdr()->e_shstrndx));

    /* Sort the sections by offset. */
    CompShdr comp;
    comp.elfFile = this;
    stable_sort(shdrs.begin() + 1, shdrs.end(), comp);

    /* Restore the sh_link mappings. */
    for (unsigned int i = 1; i < rdi(hdr()->e_shnum); ++i)
        if (rdi(shdrs[i].sh_link) != 0)
            wri(shdrs[i].sh_link,
                getSectionIndex(linkage[getSectionName(shdrs[i])]));

    /* And the st_info mappings. */
    for (unsigned int i = 1; i < rdi(hdr()->e_shnum); ++i)
        if (rdi(shdrs.at(i).sh_info) != 0 &&
            (rdi(shdrs.at(i).sh_type) == SHT_REL || rdi(shdrs.at(i).sh_type) == SHT_RELA))
            wri(shdrs.at(i).sh_info,
                getSectionIndex(info.at(getSectionName(shdrs.at(i)))));

    /* And the .shstrtab index. Note: the match here is done by checking the offset as searching
     * by name can yield incorrect results in case there are multiple sections with the same
     * name as the one initially pointed by hdr()->e_shstrndx */
    for (unsigned int i = 1; i < rdi(hdr()->e_shnum); ++i) {
        if (shdrs.at(i).sh_offset == shstrtab.sh_offset) {
            wri(hdr()->e_shstrndx, i);
        }
    }
}

static void writeFile(const std::string & fileName, const FileContents & contents) {
    debug("writing %s\n", fileName.c_str());

    int fd = open(fileName.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0777);
    if (fd == -1)
        error("open");

    size_t bytesWritten = 0;
    ssize_t portion;
    while (bytesWritten < contents->size()) {
        if ((portion = write(fd, contents->data() + bytesWritten, contents->size() - bytesWritten)) < 0) {
            if (errno == EINTR)
                continue;
            error("write");
        }
        bytesWritten += portion;
    }

    if (close(fd) >= 0)
        return;
    /*
     * Just ignore EINTR; a retry loop is the wrong thing to do.
     *
     * http://lkml.indiana.edu/hypermail/linux/kernel/0509.1/0877.html
     * https://bugzilla.gnome.org/show_bug.cgi?id=682819
     * http://utcc.utoronto.ca/~cks/space/blog/unix/CloseEINTR
     * https://sites.google.com/site/michaelsafyan/software-engineering/checkforeintrwheninvokingclosethinkagain
     */
    if (errno == EINTR)
        return;
    error("close");
}

static uint64_t roundUp(uint64_t n, uint64_t m) {
    if (n == 0)
        return m;
    return ((n - 1) / m + 1) * m;
}

template<ElfFileParams>
void ElfFile<ElfFileParamNames>::shiftFile(unsigned int extraPages, size_t startOffset, size_t extraBytes) {
    assert(startOffset >= sizeof(Elf_Ehdr));

    auto oldSize = fileContents->size();
    assert(oldSize > startOffset);

    /* Move the entire contents of the file after 'startOffset' by 'extraPages' pages further. */
    unsigned int shift = extraPages * getPageSize();
    fileContents->resize(oldSize + shift, 0);
    memmove(fileContents->data() + startOffset + shift, fileContents->data() + startOffset, oldSize - startOffset);
    memset(fileContents->data() + startOffset, 0, shift);

    /* Adjust the ELF header. */
    wri(hdr()->e_phoff, sizeof(Elf_Ehdr));
    if (rdi(hdr()->e_shoff) >= startOffset)
        wri(hdr()->e_shoff, rdi(hdr()->e_shoff) + shift);

    /* Update the offsets in the section headers. */
    for (int i = 1; i < rdi(hdr()->e_shnum); ++i) {
        size_t sh_offset = rdi(shdrs.at(i).sh_offset);
        if (sh_offset >= startOffset)
            wri(shdrs.at(i).sh_offset, sh_offset + shift);
    }

    int splitIndex = -1;
    size_t splitShift = 0;

    /* Update the offsets in the program headers. */
    for (int i = 0; i < rdi(hdr()->e_phnum); ++i) {
        Elf_Off p_start = rdi(phdrs.at(i).p_offset);

        if (p_start <= startOffset && p_start + rdi(phdrs.at(i).p_filesz) > startOffset && rdi(phdrs.at(i).p_type) == PT_LOAD) {
            assert(splitIndex == -1);

            splitIndex = i;
            splitShift = startOffset - p_start;

            /* This is the load segment we're currently extending within, so we split it. */
            wri(phdrs.at(i).p_offset, startOffset);
            wri(phdrs.at(i).p_memsz, rdi(phdrs.at(i).p_memsz) - splitShift);
            wri(phdrs.at(i).p_filesz, rdi(phdrs.at(i).p_filesz) - splitShift);
            wri(phdrs.at(i).p_paddr, rdi(phdrs.at(i).p_paddr) + splitShift);
            wri(phdrs.at(i).p_vaddr, rdi(phdrs.at(i).p_vaddr) + splitShift);

            p_start = startOffset;
        }

        if (p_start >= startOffset) {
            wri(phdrs.at(i).p_offset, p_start + shift);
            if (rdi(phdrs.at(i).p_align) != 0 &&
                (rdi(phdrs.at(i).p_vaddr) - rdi(phdrs.at(i).p_offset)) % rdi(phdrs.at(i).p_align) != 0) {
                debug("changing alignment of program header %d from %d to %d\n", i,
                    rdi(phdrs.at(i).p_align), getPageSize());
                wri(phdrs.at(i).p_align, getPageSize());
            }
        } else {
            /* If we're not physically shifting, shift back virtual memory. */
            if (rdi(phdrs.at(i).p_paddr) >= shift)
                wri(phdrs.at(i).p_paddr, rdi(phdrs.at(i).p_paddr) - shift);
            if (rdi(phdrs.at(i).p_vaddr) >= shift)
                wri(phdrs.at(i).p_vaddr, rdi(phdrs.at(i).p_vaddr) - shift);
        }
    }

    assert(splitIndex != -1);

    /* Add another PT_LOAD segment loading the data we've split above. */
    phdrs.resize(rdi(hdr()->e_phnum) + 1);
    wri(hdr()->e_phnum, rdi(hdr()->e_phnum) + 1);
    Elf_Phdr & phdr = phdrs.at(rdi(hdr()->e_phnum) - 1);
    wri(phdr.p_type, PT_LOAD);
    wri(phdr.p_offset, phdrs.at(splitIndex).p_offset - splitShift - shift);
    wri(phdr.p_paddr, phdrs.at(splitIndex).p_paddr - splitShift - shift);
    wri(phdr.p_vaddr, phdrs.at(splitIndex).p_vaddr - splitShift - shift);
    wri(phdr.p_filesz, wri(phdr.p_memsz, splitShift + extraBytes));
    wri(phdr.p_flags, PF_R | PF_W);
    wri(phdr.p_align, getPageSize());
}

template<ElfFileParams>
std::string ElfFile<ElfFileParamNames>::getSectionName(const Elf_Shdr & shdr) const {
    const size_t name_off = rdi(shdr.sh_name);

    if (name_off >= sectionNames.size())
        error("section name offset out of bounds");

    return std::string(sectionNames.c_str() + name_off);
}

template<ElfFileParams>
const Elf_Shdr & ElfFile<ElfFileParamNames>::findSectionHeader(const SectionName & sectionName) const {
    auto shdr = tryFindSectionHeader(sectionName);
    if (!shdr) {
        std::string extraMsg;
        if (sectionName == ".interp" || sectionName == ".dynamic" || sectionName == ".dynstr")
            extraMsg = ". The input file is most likely statically linked";
        error("cannot find section '" + sectionName + "'" + extraMsg);
    }
    return *shdr;
}


template<ElfFileParams>
std::optional<std::reference_wrapper<const Elf_Shdr>> ElfFile<ElfFileParamNames>::tryFindSectionHeader(const SectionName & sectionName) const {
    auto i = getSectionIndex(sectionName);
    if (i)
        return shdrs.at(i);
    return {};
}

template<ElfFileParams>
template<class T>
span<T> ElfFile<ElfFileParamNames>::getSectionSpan(const Elf_Shdr & shdr) const {
    return span((T*)(fileContents->data() + rdi(shdr.sh_offset)), rdi(shdr.sh_size)/sizeof(T));
}

template<ElfFileParams>
template<class T>
span<T> ElfFile<ElfFileParamNames>::getSectionSpan(const SectionName & sectionName) {
    return getSectionSpan<T>(findSectionHeader(sectionName));
}

template<ElfFileParams>
template<class T>
span<T> ElfFile<ElfFileParamNames>::tryGetSectionSpan(const SectionName & sectionName) {
    auto shdrOpt = tryFindSectionHeader(sectionName);
    return shdrOpt ? getSectionSpan<T>(*shdrOpt) : span<T>();
}

template<ElfFileParams>
unsigned int ElfFile<ElfFileParamNames>::getSectionIndex(const SectionName & sectionName) const {
    for (unsigned int i = 1; i < rdi(hdr()->e_shnum); ++i)
        if (getSectionName(shdrs.at(i)) == sectionName) return i;
    return 0;
}

template<ElfFileParams>
bool ElfFile<ElfFileParamNames>::hasReplacedSection(const SectionName & sectionName) const {
    return replacedSections.count(sectionName);
}

template<ElfFileParams>
bool ElfFile<ElfFileParamNames>::canReplaceSection(const SectionName & sectionName) const {
    auto shdr = findSectionHeader(sectionName);

    return sectionName == ".interp" || rdi(shdr.sh_type) != SHT_PROGBITS;
}

template<ElfFileParams>
std::string & ElfFile<ElfFileParamNames>::replaceSection(
	const SectionName & sectionName,
    unsigned int size
) {
    auto i = replacedSections.find(sectionName);
    std::string s;

    if (i != replacedSections.end()) {
        s = std::string(i->second);
    } else {
        auto shdr = findSectionHeader(sectionName);
        s = extractString(fileContents, rdi(shdr.sh_offset), rdi(shdr.sh_size));
    }

    s.resize(size);
    replacedSections[sectionName] = s;

    return replacedSections[sectionName];
}

template<ElfFileParams>
void ElfFile<ElfFileParamNames>::writeReplacedSections(
	Elf_Off & curOff,
    Elf_Addr startAddr,
    Elf_Off startOffset
) {
    if (clobberOldSections) {
        /* Overwrite the old section contents with 'Z's.  Do this
           *before* writing the new section contents (below) to prevent
           clobbering previously written new section contents. */
        for (auto & i : replacedSections) {
            const std::string & sectionName = i.first;
            const Elf_Shdr & shdr = findSectionHeader(sectionName);
            if (rdi(shdr.sh_type) != SHT_NOBITS)
                memset(fileContents->data() + rdi(shdr.sh_offset), 'Z', rdi(shdr.sh_size));
        }
    }

    std::set<unsigned int> noted_phdrs = {};

    /* We iterate over the sorted section headers here, so that the relative
       position between replaced sections stays the same.  */
    for (auto & shdr : shdrs) {
        std::string sectionName = getSectionName(shdr);
        auto i = replacedSections.find(sectionName);
        if (i == replacedSections.end())
            continue;

        Elf_Shdr orig_shdr = shdr;
        debug("rewriting section '%s' from offset 0x%x (size %d) to offset 0x%x (size %d)\n",
            sectionName.c_str(), rdi(shdr.sh_offset), rdi(shdr.sh_size), curOff, i->second.size());

        memcpy(fileContents->data() + curOff, i->second.c_str(),
            i->second.size());

        /* Update the section header for this section. */
        wri(shdr.sh_offset, curOff);
        wri(shdr.sh_addr, startAddr + (curOff - startOffset));
        wri(shdr.sh_size, i->second.size());
        wri(shdr.sh_addralign, sectionAlignment);

        /* If this is the .interp section, then the PT_INTERP segment
           must be sync'ed with it. */
        if (sectionName == ".interp") {
            for (auto & phdr : phdrs) {
                if (rdi(phdr.p_type) == PT_INTERP) {
                    phdr.p_offset = shdr.sh_offset;
                    phdr.p_vaddr = phdr.p_paddr = shdr.sh_addr;
                    phdr.p_filesz = phdr.p_memsz = shdr.sh_size;
                }
            }
        }

        /* If this is the .dynamic section, then the PT_DYNAMIC segment
           must be sync'ed with it. */
        else if (sectionName == ".dynamic") {
            for (auto & phdr : phdrs) {
                if (rdi(phdr.p_type) == PT_DYNAMIC) {
                    phdr.p_offset = shdr.sh_offset;
                    phdr.p_vaddr = phdr.p_paddr = shdr.sh_addr;
                    phdr.p_filesz = phdr.p_memsz = shdr.sh_size;
                }
            }
        }

        /* If this is a note section, there might be a PT_NOTE segment that
           must be sync'ed with it. Note that normalizeNoteSegments() will have
           already taken care of PT_NOTE segments containing multiple note
           sections. At this point, we can assume that the segment will map to
           exactly one section.

           Note sections also have particular alignment constraints: the
           data inside the section is formatted differently depending on the
           section alignment. Keep the original alignment if possible. */
        if (rdi(shdr.sh_type) == SHT_NOTE) {
            if (orig_shdr.sh_addralign < sectionAlignment)
                shdr.sh_addralign = orig_shdr.sh_addralign;

            for (unsigned int j = 0; j < phdrs.size(); ++j) {
                auto &phdr = phdrs.at(j);
                if (rdi(phdr.p_type) == PT_NOTE && !noted_phdrs.count(j)) {
                    Elf_Off p_start = rdi(phdr.p_offset);
                    Elf_Off p_end = p_start + rdi(phdr.p_filesz);
                    Elf_Off s_start = rdi(orig_shdr.sh_offset);
                    Elf_Off s_end = s_start + rdi(orig_shdr.sh_size);

                    /* Skip if no overlap. */
                    if (!(s_start >= p_start && s_start < p_end) &&
                        !(s_end > p_start && s_end <= p_end))
                        continue;

                    /* We only support exact matches. */
                    if (p_start != s_start || p_end != s_end)
                        error("unsupported overlap of SHT_NOTE and PT_NOTE");

                    phdr.p_offset = shdr.sh_offset;
                    phdr.p_vaddr = phdr.p_paddr = shdr.sh_addr;
                    phdr.p_filesz = phdr.p_memsz = shdr.sh_size;

                    noted_phdrs.insert(j);
                }
            }
        }

        /* If there is .MIPS.abiflags section, then the PT_MIPS_ABIFLAGS
           segment must be sync'ed with it. */
        if (sectionName == ".MIPS.abiflags") {
            for (auto & phdr : phdrs) {
                if (rdi(phdr.p_type) == PT_MIPS_ABIFLAGS) {
                    phdr.p_offset = shdr.sh_offset;
                    phdr.p_vaddr = phdr.p_paddr = shdr.sh_addr;
                    phdr.p_filesz = phdr.p_memsz = shdr.sh_size;
                }
            }
        }

        /* If there is .note.gnu.property section, then the PT_GNU_PROPERTY
           segment must be sync'ed with it. */
        if (sectionName == ".note.gnu.property") {
            for (auto & phdr : phdrs) {
                if (rdi(phdr.p_type) == PT_GNU_PROPERTY) {
                    phdr.p_offset = shdr.sh_offset;
                    phdr.p_vaddr = phdr.p_paddr = shdr.sh_addr;
                    phdr.p_filesz = phdr.p_memsz = shdr.sh_size;
                }
            }
        }

        curOff += roundUp(i->second.size(), sectionAlignment);
    }

    replacedSections.clear();
}


template<ElfFileParams>
void ElfFile<ElfFileParamNames>::rewriteSectionsLibrary() {
    /* For dynamic libraries, we just place the replacement sections
       at the end of the file.  They're mapped into memory by a
       PT_LOAD segment located directly after the last virtual address
       page of other segments. */
    Elf_Addr startPage = 0;
    Elf_Addr firstPage = 0;
    unsigned alignStartPage = getPageSize();
    for (auto & phdr : phdrs) {
        Elf_Addr thisPage = rdi(phdr.p_vaddr) + rdi(phdr.p_memsz);
        if (thisPage > startPage) startPage = thisPage;
        if (rdi(phdr.p_type) == PT_PHDR) firstPage = rdi(phdr.p_vaddr) - rdi(phdr.p_offset);
        unsigned thisAlign = rdi(phdr.p_align);
        alignStartPage = std::max(alignStartPage, thisAlign);
    }

    startPage = roundUp(startPage, alignStartPage);

    debug("last page is 0x%llx\n", (unsigned long long) startPage);
    debug("first page is 0x%llx\n", (unsigned long long) firstPage);

    /* When normalizing note segments we will in the worst case be adding
       1 program header for each SHT_NOTE section. */
    unsigned int num_notes = std::count_if(shdrs.begin(), shdrs.end(),
        [this](Elf_Shdr shdr) { return rdi(shdr.sh_type) == SHT_NOTE; });

    /* Compute the total space needed for the replaced sections, pessimistically
       assuming we're going to need one more to account for new PT_LOAD covering
       relocated PHDR */
    off_t phtSize = roundUp((phdrs.size() + num_notes + 1) * sizeof(Elf_Phdr) + sizeof(Elf_Ehdr), sectionAlignment);
    off_t shtSize = roundUp(rdi(hdr()->e_shnum) * rdi(hdr()->e_shentsize), sectionAlignment);

    /* Check if we can keep PHT at the beginning of the file.

       We'd like to do that, because it preverves compatibility with older
       kernels¹ - but if the PHT has grown too much, we have no other option but
       to move it at the end of the file.

       ¹ older kernels had a bug that prevented them from loading ELFs with
         PHDRs not located at the beginning of the file; it was fixed over
         0da1d5002745cdc721bc018b582a8a9704d56c42 (2022-03-02) */
    bool relocatePht = false;
    unsigned int i = 1;

    while (i < rdi(hdr()->e_shnum) && ((off_t) rdi(shdrs.at(i).sh_offset)) <= phtSize) {
        const auto & sectionName = getSectionName(shdrs.at(i));

        if (!hasReplacedSection(sectionName) && !canReplaceSection(sectionName)) {
            relocatePht = true;
            break;
        }

        i++;
    }

    if (!relocatePht) {
        unsigned int i = 1;

        while (i < rdi(hdr()->e_shnum) && ((off_t) rdi(shdrs.at(i).sh_offset)) <= phtSize) {
            const auto & sectionName = getSectionName(shdrs.at(i));
            const auto sectionSize = rdi(shdrs.at(i).sh_size);

            if (!hasReplacedSection(sectionName)) {
                replaceSection(sectionName, sectionSize);
            }

            i++;
        }
    }

    /* Calculate how much space we'll need. */
    off_t neededSpace = shtSize;

    if (relocatePht) {
        neededSpace += phtSize;
    }

    for (auto & s : replacedSections)
        neededSpace += roundUp(s.second.size(), sectionAlignment);

    debug("needed space is %d\n", neededSpace);

    Elf_Off startOffset = roundUp(fileContents->size(), alignStartPage);

    // In older version of binutils (2.30), readelf would check if the dynamic
    // section segment is strictly smaller than the file (and not same size).
    // By making it one byte larger, we don't break readelf.
    off_t binutilsQuirkPadding = 1;

    fileContents->resize(startOffset + neededSpace + binutilsQuirkPadding, 0);

    auto& lastSeg = phdrs.back();
    Elf_Addr lastSegAddr = 0;

    /* As an optimization, instead of allocating a new PT_LOAD segment, try
       expanding the last one */
    if (!phdrs.empty() &&
        rdi(lastSeg.p_type) == PT_LOAD &&
        rdi(lastSeg.p_flags) == (PF_R | PF_W) &&
        rdi(lastSeg.p_align) == alignStartPage) {
        auto segEnd = roundUp(rdi(lastSeg.p_offset) + rdi(lastSeg.p_memsz), alignStartPage);

        if (segEnd == startOffset) {
            auto newSz = startOffset + neededSpace - rdi(lastSeg.p_offset);

            wri(lastSeg.p_filesz, wri(lastSeg.p_memsz, newSz));

            lastSegAddr = rdi(lastSeg.p_vaddr) + newSz - neededSpace;
        }
    }

    if (lastSegAddr == 0) {
        debug("allocating new PT_LOAD segment\n");

        /* Add a segment that maps the replaced sections into memory. */
        phdrs.resize(rdi(hdr()->e_phnum) + 1);
        wri(hdr()->e_phnum, rdi(hdr()->e_phnum) + 1);
        Elf_Phdr & phdr = phdrs.at(rdi(hdr()->e_phnum) - 1);
        wri(phdr.p_type, PT_LOAD);
        wri(phdr.p_offset, startOffset);
        wri(phdr.p_vaddr, wri(phdr.p_paddr, startPage));
        wri(phdr.p_filesz, wri(phdr.p_memsz, neededSpace));
        wri(phdr.p_flags, PF_R | PF_W);
        wri(phdr.p_align, alignStartPage);
        assert(startPage % alignStartPage == startOffset % alignStartPage);

        lastSegAddr = startPage;
    }

    normalizeNoteSegments();

    /* Write out the replaced sections. */
    Elf_Off curOff = startOffset;

    if (relocatePht) {
        debug("rewriting pht from offset 0x%x to offset 0x%x (size %d)\n",
            rdi(hdr()->e_phoff), curOff, phtSize);

        wri(hdr()->e_phoff, curOff);
        curOff += phtSize;
    }

    // ---

    debug("rewriting sht from offset 0x%x to offset 0x%x (size %d)\n",
        rdi(hdr()->e_shoff), curOff, shtSize);

    wri(hdr()->e_shoff, curOff);
    curOff += shtSize;

    // ---

    /* Write out the replaced sections. */
    writeReplacedSections(curOff, startPage, startOffset);
    assert(curOff == startOffset + neededSpace);

    /* Write out the updated program and section headers */
    if (relocatePht) {
        rewriteHeaders(lastSegAddr);
    } else {
        rewriteHeaders(firstPage + rdi(hdr()->e_phoff));
    }
}

static bool noSort = false;

template<ElfFileParams>
void ElfFile<ElfFileParamNames>::rewriteSectionsExecutable() {
    if (!noSort) {
        /* Sort the sections by offset, otherwise we won't correctly find
           all the sections before the last replaced section. */
        sortShdrs();
    }

    /* What is the index of the last replaced section? */
    unsigned int lastReplaced = 0;
    for (unsigned int i = 1; i < rdi(hdr()->e_shnum); ++i) {
        std::string sectionName = getSectionName(shdrs.at(i));
        if (replacedSections.count(sectionName)) {
            debug("using replaced section '%s'\n", sectionName.c_str());
            lastReplaced = i;
        }
    }

    assert(lastReplaced != 0);

    debug("last replaced is %d\n", lastReplaced);

    /* Try to replace all sections before that, as far as possible.
       Stop when we reach an irreplacable section (such as one of type
       SHT_PROGBITS).  These cannot be moved in virtual address space
       since that would invalidate absolute references to them. */
    assert(lastReplaced + 1 < shdrs.size()); /* !!! I'm lazy. */
    size_t startOffset = rdi(shdrs.at(lastReplaced + 1).sh_offset);
    Elf_Addr startAddr = rdi(shdrs.at(lastReplaced + 1).sh_addr);
    std::string prevSection;
    for (unsigned int i = 1; i <= lastReplaced; ++i) {
        Elf_Shdr & shdr(shdrs.at(i));
        std::string sectionName = getSectionName(shdr);
        debug("looking at section '%s'\n", sectionName.c_str());
        /* !!! Why do we stop after a .dynstr section? I can't
           remember! */
        if ((rdi(shdr.sh_type) == SHT_PROGBITS && sectionName != ".interp")
            || prevSection == ".dynstr")
        {
            startOffset = rdi(shdr.sh_offset);
            startAddr = rdi(shdr.sh_addr);
            lastReplaced = i - 1;
            break;
        }
        if (!replacedSections.count(sectionName)) {
            debug("replacing section '%s' which is in the way\n", sectionName.c_str());
            replaceSection(sectionName, rdi(shdr.sh_size));
        }
        prevSection = std::move(sectionName);
    }

    debug("first reserved offset/addr is 0x%x/0x%llx\n",
        startOffset, (unsigned long long) startAddr);

    assert(startAddr % getPageSize() == startOffset % getPageSize());
    Elf_Addr firstPage = startAddr - startOffset;
    debug("first page is 0x%llx\n", (unsigned long long) firstPage);

    if (rdi(hdr()->e_shoff) < startOffset) {
        /* The section headers occur too early in the file and would be
           overwritten by the replaced sections. Move them to the end of the file
           before proceeding. */
        off_t shoffNew = fileContents->size();
        off_t shSize = rdi(hdr()->e_shoff) + rdi(hdr()->e_shnum) * rdi(hdr()->e_shentsize);
        fileContents->resize(fileContents->size() + shSize, 0);
        wri(hdr()->e_shoff, shoffNew);

        /* Rewrite the section header table.  For neatness, keep the
           sections sorted. */
        assert(rdi(hdr()->e_shnum) == shdrs.size());
        sortShdrs();
        for (unsigned int i = 1; i < rdi(hdr()->e_shnum); ++i)
            * ((Elf_Shdr *) (fileContents->data() + rdi(hdr()->e_shoff)) + i) = shdrs.at(i);
    }


    normalizeNoteSegments();


    /* Compute the total space needed for the replaced sections, the
       ELF header, and the program headers. */
    size_t neededSpace = sizeof(Elf_Ehdr) + phdrs.size() * sizeof(Elf_Phdr);
    for (auto & i : replacedSections)
        neededSpace += roundUp(i.second.size(), sectionAlignment);

    debug("needed space is %d\n", neededSpace);

    /* If we need more space at the start of the file, then grow the
       file by the minimum number of pages and adjust internal
       offsets. */
    if (neededSpace > startOffset) {
        /* We also need an additional program header, so adjust for that. */
        neededSpace += sizeof(Elf_Phdr);
        debug("needed space is %d\n", neededSpace);

        /* Calculate how many bytes are needed out of the additional pages. */
        size_t extraSpace = neededSpace - startOffset; 
        // Always give one extra page to avoid colliding with segments that start at
        // unaligned addresses and will be rounded down when loaded
        unsigned int neededPages = 1 + roundUp(extraSpace, getPageSize()) / getPageSize();
        debug("needed pages is %d\n", neededPages);
        if (neededPages * getPageSize() > firstPage)
            error("virtual address space underrun!");

        shiftFile(neededPages, startOffset, extraSpace);

        firstPage -= neededPages * getPageSize();
        startOffset += neededPages * getPageSize();
    }

    Elf_Off curOff = sizeof(Elf_Ehdr) + phdrs.size() * sizeof(Elf_Phdr);

    /* Ensure PHDR is covered by a LOAD segment.

       Because PHDR is supposed to have been covered by such section before, in
       here we assume that we don't have to create any new section, but rather
       extend the existing one. */
    for (auto& phdr : phdrs)
        if (rdi(phdr.p_type) == PT_LOAD &&
            rdi(phdr.p_offset) <= curOff &&
            rdi(phdr.p_offset) + rdi(phdr.p_filesz) > curOff &&
            rdi(phdr.p_filesz) < neededSpace)
        {
            wri(phdr.p_filesz, neededSpace);
            wri(phdr.p_memsz, neededSpace);
            break;
        }

    /* Clear out the free space. */
    debug("clearing first %d bytes\n", startOffset - curOff);
    memset(fileContents->data() + curOff, 0, startOffset - curOff);

    /* Write out the replaced sections. */
    writeReplacedSections(curOff, firstPage, 0);
    assert(curOff == neededSpace);

    /* Write out the updated program and section headers */
    rewriteHeaders(firstPage + rdi(hdr()->e_phoff));
}

template<ElfFileParams>
void ElfFile<ElfFileParamNames>::normalizeNoteSegments() {
    /* Break up PT_NOTE segments containing multiple SHT_NOTE sections. This
       is to avoid having to deal with moving multiple sections together if
       one of them has to be replaced. */

    /* We don't need to do anything if no note segments were replaced. */
    bool replaced_note = std::any_of(replacedSections.begin(), replacedSections.end(),
        [this](std::pair<const std::string, std::string> & i) { return rdi(findSectionHeader(i.first).sh_type) == SHT_NOTE; });
    if (!replaced_note) return;

    std::vector<Elf_Phdr> newPhdrs;
    for (auto & phdr : phdrs) {
        if (rdi(phdr.p_type) != PT_NOTE) continue;

        size_t start_off = rdi(phdr.p_offset);
        size_t curr_off = start_off;
        size_t end_off = start_off + rdi(phdr.p_filesz);

        /* Binaries produced by older patchelf versions may contain empty PT_NOTE segments.
           For backwards compatibility, if we find one we should ignore it. */
        bool empty = std::none_of(shdrs.begin(), shdrs.end(),
            [&](auto & shdr) { return rdi(shdr.sh_offset) >= start_off && rdi(shdr.sh_offset) < end_off; });
        if (empty)
            continue;

        while (curr_off < end_off) {
            /* Find a section that starts at the current offset. If we can't
               find one, it means the SHT_NOTE sections weren't contiguous
               within the segment. */
            size_t size = 0;
            for (const auto & shdr : shdrs) {
                if (rdi(shdr.sh_type) != SHT_NOTE) continue;
                if (rdi(shdr.sh_offset) != roundUp(curr_off, rdi(shdr.sh_addralign))) continue;
                size = rdi(shdr.sh_size);
                curr_off = roundUp(curr_off, rdi(shdr.sh_addralign));
                break;
            }
            if (size == 0)
                error("cannot normalize PT_NOTE segment: non-contiguous SHT_NOTE sections");
            if (curr_off + size > end_off)
                error("cannot normalize PT_NOTE segment: partially mapped SHT_NOTE section");

            /* Build a new phdr for this note section. */
            Elf_Phdr new_phdr = phdr;
            wri(new_phdr.p_offset, curr_off);
            wri(new_phdr.p_vaddr, rdi(phdr.p_vaddr) + (curr_off - start_off));
            wri(new_phdr.p_paddr, rdi(phdr.p_paddr) + (curr_off - start_off));
            wri(new_phdr.p_filesz, size);
            wri(new_phdr.p_memsz, size);

            /* If we haven't yet, reuse the existing phdr entry. Otherwise add
               a new phdr to the table. */
            if (curr_off == start_off)
                phdr = new_phdr;
            else
                newPhdrs.push_back(new_phdr);

            curr_off += size;
        }
    }
    phdrs.insert(phdrs.end(), newPhdrs.begin(), newPhdrs.end());

    wri(hdr()->e_phnum, phdrs.size());
}

template<ElfFileParams>
void ElfFile<ElfFileParamNames>::rewriteSections(bool force) {
    if (!force && replacedSections.empty()) return;

    for (auto & i : replacedSections)
        debug("replacing section '%s' with size %d\n",
            i.first.c_str(), i.second.size());

    if (rdi(hdr()->e_type) == ET_DYN) {
        debug("this is a dynamic library\n");
        rewriteSectionsLibrary();
    } else if (rdi(hdr()->e_type) == ET_EXEC) {
        debug("this is an executable\n");
        rewriteSectionsExecutable();
    } else error("unknown ELF type");
}

template<ElfFileParams>
void ElfFile<ElfFileParamNames>::rewriteHeaders(Elf_Addr phdrAddress) {
    /* Rewrite the program header table. */

    /* If there is a segment for the program header table, update it.
       (According to the ELF spec, there can only be one.) */
    for (auto & phdr : phdrs) {
        if (rdi(phdr.p_type) == PT_PHDR) {
            phdr.p_offset = hdr()->e_phoff;
            wri(phdr.p_vaddr, wri(phdr.p_paddr, phdrAddress));
            wri(phdr.p_filesz, wri(phdr.p_memsz, phdrs.size() * sizeof(Elf_Phdr)));
            break;
        }
    }

    if (!noSort) {
        sortPhdrs();
    }

    for (unsigned int i = 0; i < phdrs.size(); ++i)
        * ((Elf_Phdr *) (fileContents->data() + rdi(hdr()->e_phoff)) + i) = phdrs.at(i);


    /* Rewrite the section header table.  For neatness, keep the
       sections sorted. */
    assert(rdi(hdr()->e_shnum) == shdrs.size());
    if (!noSort) {
        sortShdrs();
    }
    for (unsigned int i = 1; i < rdi(hdr()->e_shnum); ++i)
        * ((Elf_Shdr *) (fileContents->data() + rdi(hdr()->e_shoff)) + i) = shdrs.at(i);


    /* Update all those nasty virtual addresses in the .dynamic
       section.  Note that not all executables have .dynamic sections
       (e.g., those produced by klibc's klcc). */
    auto shdrDynamic = tryFindSectionHeader(".dynamic");
    if (shdrDynamic) {
        auto dyn_table = (Elf_Dyn *) (fileContents->data() + rdi((*shdrDynamic).get().sh_offset));
        unsigned int d_tag;
        for (auto dyn = dyn_table; (d_tag = rdi(dyn->d_tag)) != DT_NULL; dyn++)
            if (d_tag == DT_STRTAB)
                dyn->d_un.d_ptr = findSectionHeader(".dynstr").sh_addr;
            else if (d_tag == DT_STRSZ)
                dyn->d_un.d_val = findSectionHeader(".dynstr").sh_size;
            else if (d_tag == DT_SYMTAB)
                dyn->d_un.d_ptr = findSectionHeader(".dynsym").sh_addr;
            else if (d_tag == DT_HASH)
                dyn->d_un.d_ptr = findSectionHeader(".hash").sh_addr;
            else if (d_tag == DT_GNU_HASH) {
                auto shdr = tryFindSectionHeader(".gnu.hash");
                // some binaries might this section stripped
                // in which case we just ignore the value.
                if (shdr) dyn->d_un.d_ptr = (*shdr).get().sh_addr;
            } else if (d_tag == DT_MIPS_XHASH) {
                // the .MIPS.xhash section was added to the glibc-ABI
                // in commit 23c1c256ae7b0f010d0fcaff60682b620887b164
                dyn->d_un.d_ptr = findSectionHeader(".MIPS.xhash").sh_addr;
            } else if (d_tag == DT_JMPREL) {
                auto shdr = tryFindSectionHeader(".rel.plt");
                if (!shdr) shdr = tryFindSectionHeader(".rela.plt");
                /* 64-bit Linux, x86-64 */
                if (!shdr) shdr = tryFindSectionHeader(".rela.IA_64.pltoff"); /* 64-bit Linux, IA-64 */
                if (!shdr) error("cannot find section corresponding to DT_JMPREL");
                dyn->d_un.d_ptr = (*shdr).get().sh_addr;
            }
            else if (d_tag == DT_REL) { /* !!! hack! */
                auto shdr = tryFindSectionHeader(".rel.dyn");
                /* no idea if this makes sense, but it was needed for some
                   program */
                if (!shdr) shdr = tryFindSectionHeader(".rel.got");
                /* some programs have neither section, but this doesn't seem
                   to be a problem */
                if (!shdr) continue;
                dyn->d_un.d_ptr = (*shdr).get().sh_addr;
            }
            else if (d_tag == DT_RELA) {
                auto shdr = tryFindSectionHeader(".rela.dyn");
                /* some programs lack this section, but it doesn't seem to
                   be a problem */
                if (!shdr) continue;
                dyn->d_un.d_ptr = (*shdr).get().sh_addr;
            }
            else if (d_tag == DT_VERNEED)
                dyn->d_un.d_ptr = findSectionHeader(".gnu.version_r").sh_addr;
            else if (d_tag == DT_VERSYM)
                dyn->d_un.d_ptr = findSectionHeader(".gnu.version").sh_addr;
            else if (d_tag == DT_MIPS_RLD_MAP_REL) {
                /* the MIPS_RLD_MAP_REL tag stores the offset to the debug
                   pointer, relative to the address of the tag */
                auto shdr = tryFindSectionHeader(".rld_map");
                if (shdr) {
                    /*
                     * "When correct, (DT_MIPS_RLD_MAP_REL + tag offset + executable base address) equals DT_MIPS_RLD_MAP"
                     * -- https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=820334#5
                     *
                     * Equivalently,
                     *
                     *   DT_MIPS_RLD_MAP_REL + tag offset + executable base address == DT_MIPS_RLD_MAP
                     *   DT_MIPS_RLD_MAP_REL              + executable base address == DT_MIPS_RLD_MAP - tag_offset
                     *   DT_MIPS_RLD_MAP_REL                                        == DT_MIPS_RLD_MAP - tag_offset - executable base address
                     */
                    auto rld_map_addr = findSectionHeader(".rld_map").sh_addr;
                    auto dyn_offset = ((char*)dyn) - ((char*)dyn_table);
                    dyn->d_un.d_ptr = rld_map_addr - dyn_offset - (*shdrDynamic).get().sh_addr;
                } else {
                    /* ELF file with DT_MIPS_RLD_MAP_REL but without .rld_map
                       is broken, and it's not our job to fix it; yet, we have
                       to find some location for dynamic loader to write the
                       debug pointer to; well, let's write it right here */
                    fprintf(stderr, "warning: DT_MIPS_RLD_MAP_REL entry is present, but .rld_map section is not\n");
                    dyn->d_un.d_ptr = 0;
                }
            }
    }


    /* Rewrite the .dynsym section.  It contains the indices of the
       sections in which symbols appear, so these need to be
       remapped. */
    for (unsigned int i = 1; i < rdi(hdr()->e_shnum); ++i) {
        auto &shdr = shdrs.at(i);
        if (rdi(shdr.sh_type) != SHT_SYMTAB && rdi(shdr.sh_type) != SHT_DYNSYM) continue;
        debug("rewriting symbol table section %d\n", i);
        for (size_t entry = 0; (entry + 1) * sizeof(Elf_Sym) <= rdi(shdr.sh_size); entry++) {
            auto sym = (Elf_Sym *)(fileContents->data() + rdi(shdr.sh_offset) + entry * sizeof(Elf_Sym));
            unsigned int shndx = rdi(sym->st_shndx);
            if (shndx != SHN_UNDEF && shndx < SHN_LORESERVE) {
                if (shndx >= sectionsByOldIndex.size()) {
                    fprintf(stderr, "warning: entry %d in symbol table refers to a non-existent section, skipping\n", shndx);
                    continue;
                }
                const std::string & section = sectionsByOldIndex.at(shndx);
                assert(!section.empty());
                auto newIndex = getSectionIndex(section); // inefficient
                //debug("rewriting symbol %d: index = %d (%s) -> %d\n", entry, shndx, section.c_str(), newIndex);
                wri(sym->st_shndx, newIndex);
                /* Rewrite st_value.  FIXME: we should do this for all
                   types, but most don't actually change. */
                if (ELF32_ST_TYPE(rdi(sym->st_info)) == STT_SECTION)
                    wri(sym->st_value, rdi(shdrs.at(newIndex).sh_addr));
            }
        }
    }
}

static void setSubstr(std::string & s, unsigned int pos, const std::string & t) {
    assert(pos + t.size() <= s.size());
    copy(t.begin(), t.end(), s.begin() + pos);
}

template<ElfFileParams>
void ElfFile<ElfFileParamNames>::replaceNeeded(const std::map<std::string, std::string> & libs) {
    if (libs.empty()) return;

    auto shdrDynamic = findSectionHeader(".dynamic");
    auto shdrDynStr = findSectionHeader(".dynstr");
    char * strTab = (char *) fileContents->data() + rdi(shdrDynStr.sh_offset);

    auto dyn = (Elf_Dyn *)(fileContents->data() + rdi(shdrDynamic.sh_offset));

    unsigned int verNeedNum = 0;

    unsigned int dynStrAddedBytes = 0;
    std::unordered_map<std::string, Elf_Off> addedStrings;

    for ( ; rdi(dyn->d_tag) != DT_NULL; dyn++) {
        if (rdi(dyn->d_tag) == DT_NEEDED) {
            char * name = strTab + rdi(dyn->d_un.d_val);
            auto i = libs.find(name);
            if (i != libs.end() && name != i->second) {
                auto replacement = i->second;

                debug("replacing DT_NEEDED entry '%s' with '%s'\n", name, replacement.c_str());

                auto a = addedStrings.find(replacement);
                // the same replacement string has already been added, reuse it
                if (a != addedStrings.end()) {
                    wri(dyn->d_un.d_val, a->second);
                    continue;
                }

                // technically, the string referred by d_val could be used otherwise, too (although unlikely)
                // we'll therefore add a new string
                debug("resizing .dynstr ...\n");

                // relative location of the new string
                Elf_Off strOffset = rdi(shdrDynStr.sh_size) + dynStrAddedBytes;
                std::string & newDynStr = replaceSection(".dynstr",
                    strOffset + replacement.size() + 1);
                setSubstr(newDynStr, strOffset, replacement + '\0');

                wri(dyn->d_un.d_val, strOffset);
                addedStrings[replacement] = strOffset;

                dynStrAddedBytes += replacement.size() + 1;

                changed = true;
            } else {
                debug("keeping DT_NEEDED entry '%s'\n", name);
            }
        }
        if (rdi(dyn->d_tag) == DT_VERNEEDNUM) {
            verNeedNum = rdi(dyn->d_un.d_val);
        }
    }

    // If a replaced library uses symbol versions, then there will also be
    // references to it in the "version needed" table, and these also need to
    // be replaced.

    if (verNeedNum) {
        auto shdrVersionR = findSectionHeader(".gnu.version_r");
        // The filename strings in the .gnu.version_r are different from the
        // ones in .dynamic: instead of being in .dynstr, they're in some
        // arbitrary section and we have to look in ->sh_link to figure out
        // which one.
        Elf_Shdr & shdrVersionRStrings = shdrs.at(rdi(shdrVersionR.sh_link));
        // this is where we find the actual filename strings
        char * verStrTab = (char *) fileContents->data() + rdi(shdrVersionRStrings.sh_offset);
        // and we also need the name of the section containing the strings, so
        // that we can pass it to replaceSection
        std::string versionRStringsSName = getSectionName(shdrVersionRStrings);

        debug("found .gnu.version_r with %i entries, strings in %s\n", verNeedNum, versionRStringsSName.c_str());

        unsigned int verStrAddedBytes = 0;
        // It may be that it is .dynstr again, in which case we must take the already
        // added bytes into account.
        if (versionRStringsSName == ".dynstr")
            verStrAddedBytes += dynStrAddedBytes;
        else
            // otherwise the already added strings can't be reused
            addedStrings.clear();

        auto need = (Elf_Verneed *)(fileContents->data() + rdi(shdrVersionR.sh_offset));
        while (verNeedNum > 0) {
            char * file = verStrTab + rdi(need->vn_file);
            auto i = libs.find(file);
            if (i != libs.end() && file != i->second) {
                auto replacement = i->second;

                debug("replacing .gnu.version_r entry '%s' with '%s'\n", file, replacement.c_str());

                auto a = addedStrings.find(replacement);
                // the same replacement string has already been added, reuse it
                if (a != addedStrings.end()) {
                    wri(need->vn_file, a->second);
                } else {
                    debug("resizing string section %s ...\n", versionRStringsSName.c_str());

                    Elf_Off strOffset = rdi(shdrVersionRStrings.sh_size) + verStrAddedBytes;
                    std::string & newVerDynStr = replaceSection(versionRStringsSName,
                        strOffset + replacement.size() + 1);
                    setSubstr(newVerDynStr, strOffset, replacement + '\0');

                    wri(need->vn_file, strOffset);
                    addedStrings[replacement] = strOffset;

                    verStrAddedBytes += replacement.size() + 1;
                }

                changed = true;
            } else {
                debug("keeping .gnu.version_r entry '%s'\n", file);
            }
            // the Elf_Verneed structures form a linked list, so jump to next entry
            need = (Elf_Verneed *) (((char *) need) + rdi(need->vn_next));
            --verNeedNum;
        }
    }

    this->rewriteSections();
}

static std::map<std::string, std::string> neededLibsToReplace;

template<class ElfFile>
static void patchElf2(
	ElfFile && elfFile,
	const FileContents & fileContents,
	const std::string & fileName
) {  
    elfFile.replaceNeeded(neededLibsToReplace);

    if (elfFile.isChanged()){
        writeFile(fileName, elfFile.fileContents);
    } else if (alwaysWrite) {
        debug("not modified, but alwaysWrite=true\n");
        writeFile(fileName, fileContents);
    }
}


static void patchElf() {
    for (const auto & fileName : fileNames) { 
        debug("patching ELF file '%s'\n", fileName.c_str());

        auto fileContents = readFile(fileName);
        const std::string & outputFileName2 = outputFileName.empty() ? fileName : outputFileName;

        if (getElfType(fileContents).is32Bit)
            patchElf2(ElfFile<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Addr, Elf32_Off, Elf32_Dyn, Elf32_Sym, Elf32_Versym, Elf32_Verdef, Elf32_Verdaux, Elf32_Verneed, Elf32_Vernaux, Elf32_Rel, Elf32_Rela, 32>(fileContents), fileContents, outputFileName2);
        else
            patchElf2(ElfFile<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Addr, Elf64_Off, Elf64_Dyn, Elf64_Sym, Elf64_Versym, Elf64_Verdef, Elf64_Verdaux, Elf64_Verneed, Elf64_Vernaux, Elf64_Rel, Elf64_Rela, 64>(fileContents), fileContents, outputFileName2);
    }
}

[[nodiscard]] static std::string resolveArgument(const char *arg) {
	if (strlen(arg) > 0 && arg[0] == '@') {
		FileContents cnts = readFile(arg + 1);
		return std::string((char *)cnts->data(), cnts->size());
	}

	return std::string(arg);
}


static void showHelp(const std::string & progName) {
	fprintf(stderr, "syntax: %s\n\
  [--replace-needed LIBRARY NEW_LIBRARY]\n\
  [--output FILE]\n\
  [--debug]\n\
  FILENAME...\n", progName.c_str());
}


static int mainWrapped(int argc, char * * argv) {
    if (argc <= 1) {
        showHelp(argv[0]);
        return 1;
    }

    if (getenv("PATCHELF_DEBUG") != nullptr)
        debugMode = true;

    int i;
    for (i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--replace-needed") {
            if (i+2 >= argc) error("missing argument(s)");
            neededLibsToReplace[ argv[i+1] ] = argv[i+2];
            i += 2;
        }
        else if (arg == "--output") {
            if (++i == argc) error("missing argument");
            outputFileName = resolveArgument(argv[i]);
            alwaysWrite = true;
        }
        else if (arg == "--debug") {
            debugMode = true;
        }
        else {
            fileNames.push_back(arg);
        }
    }

    if (fileNames.empty()) error("missing filename");

    if (!outputFileName.empty() && fileNames.size() != 1)
        error("--output option only allowed with single input file");
    
    patchElf();

    return 0;
}

int main(int argc, char * * argv) {
    try {
        return mainWrapped(argc, argv);
    } catch (std::exception & e) {
        fprintf(stderr, "patchelf: %s\n", e.what());
        return 1;
    }
}
