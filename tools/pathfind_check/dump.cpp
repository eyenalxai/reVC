// Path-graph dumper for the VICE-130 pathfinding checker.
//
// Reads the path graph out of a live reVC process, read-only, through
// /proc/<pid>/mem and /proc/<pid>/maps. Nothing is written to the game and no
// signal is sent to it. The result is a blob the checker loads into a real
// CPathFind instance, so the path searches in the checker run on the same
// graph the running game uses.
//
// Usage: dump <pid> <out.bin>
//
// The graph is immutable after CPathFind::PreparePathData: every path search
// is a pure function of these arrays. The dumper therefore takes a single
// snapshot and the checker can replay it as often as it likes.

#include "common.h"
#include "PathFind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------- ELF

static std::vector<uint8_t> ReadFile(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		exit(1);
	}
	std::vector<uint8_t> data;
	uint8_t buf[65536];
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		data.insert(data.end(), buf, buf + n);
	close(fd);
	return data;
}

// Offset of a global object symbol inside the executable's ELF image.
static uint64_t
FindSymbol(const std::vector<uint8_t> &elf, const char *name)
{
	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf.data();
	if (elf.size() < sizeof(Elf64_Ehdr) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
	    eh->e_ident[EI_CLASS] != ELFCLASS64) {
		fprintf(stderr, "not an ELF64 file\n");
		exit(1);
	}
	const Elf64_Shdr *sh = (const Elf64_Shdr *)(elf.data() + eh->e_shoff);
	for (int i = 0; i < eh->e_shnum; i++) {
		if (sh[i].sh_type != SHT_SYMTAB)
			continue;
		const Elf64_Sym *sym = (const Elf64_Sym *)(elf.data() + sh[i].sh_offset);
		size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
		const char *strtab = (const char *)(elf.data() + sh[sh[i].sh_link].sh_offset);
		for (size_t j = 0; j < count; j++) {
			if (ELF64_ST_TYPE(sym[j].st_info) != STT_OBJECT)
				continue;
			if (strcmp(strtab + sym[j].st_name, name) == 0)
				return sym[j].st_value;
		}
	}
	fprintf(stderr, "symbol %s not found\n", name);
	exit(1);
}

// Base address of the executable's first mapping (the PIE load address).
static uint64_t
FindBase(pid_t pid, const std::string &exe)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		perror(path);
		exit(1);
	}
	char line[4096];
	uint64_t base = 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long start, end, off;
		char perms[8], dev[16];
		unsigned long inode;
		char name[2048];
		name[0] = 0;
		int n = sscanf(line, "%llx-%llx %7s %llx %15s %lu %2047[^\n]", &start, &end, perms, &off, dev, &inode, name);
		if (n < 6)
			continue;
		const char *p = name;
		while (*p == ' ')
			p++;
		if (strcmp(p, exe.c_str()) != 0)
			continue;
		if (off == 0 && strcmp(perms, "r--p") == 0) {
			base = start;
			break;
		}
	}
	fclose(f);
	if (base == 0) {
		fprintf(stderr, "no executable mapping found\n");
		exit(1);
	}
	return base;
}

// ---------------------------------------------------------------- memory

static int g_memfd = -1;

static void
ReadMem(uint64_t addr, void *dst, size_t size)
{
	ssize_t n = pread(g_memfd, dst, size, addr);
	if (n != (ssize_t)size) {
		fprintf(stderr, "short read at 0x%llx: %zd of %zu\n",
			(unsigned long long)addr, n, size);
		exit(1);
	}
}

// ---------------------------------------------------------------- blob

struct DumpHeader
{
	char magic[8];
	uint32_t version;
	uint32_t numPathNodes;
	int32_t numCarPathNodes;
	int32_t numPedPathNodes;
	int32_t numConnections;
	int32_t numCarPathLinks;
	uint32_t sizes[4];	// CPathNode, CCarPathLink, connection table, path nodes
};

int
main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: dump <pid> <out.bin>\n");
		return 1;
	}
	pid_t pid = atoi(argv[1]);

	char exe[1024];
	char link[64];
	snprintf(link, sizeof(link), "/proc/%d/exe", pid);
	ssize_t n = readlink(link, exe, sizeof(exe) - 1);
	if (n < 0) {
		perror(link);
		return 1;
	}
	exe[n] = 0;

	std::vector<uint8_t> elf = ReadFile(exe);
	uint64_t base = FindBase(pid, exe);
	uint64_t thePaths = base + FindSymbol(elf, "ThePaths");

	char mem[64];
	snprintf(mem, sizeof(mem), "/proc/%d/mem", pid);
	g_memfd = open(mem, O_RDONLY);
	if (g_memfd < 0) {
		perror(mem);
		return 1;
	}

	// Read the small scalars first: they say how much of the arrays is live.
	int32_t numPathNodes, numCarPathNodes, numPedPathNodes, numCarPathLinks;
	int16_t numConnections;
	ReadMem(thePaths + offsetof(CPathFind, m_numPathNodes), &numPathNodes, sizeof(numPathNodes));
	ReadMem(thePaths + offsetof(CPathFind, m_numCarPathNodes), &numCarPathNodes, sizeof(numCarPathNodes));
	ReadMem(thePaths + offsetof(CPathFind, m_numPedPathNodes), &numPedPathNodes, sizeof(numPedPathNodes));
	ReadMem(thePaths + offsetof(CPathFind, m_numConnections), &numConnections, sizeof(numConnections));
	ReadMem(thePaths + offsetof(CPathFind, m_numCarPathLinks), &numCarPathLinks, sizeof(numCarPathLinks));

	if (numPathNodes <= 0 || numPathNodes > NUM_PATHNODES ||
	    numConnections < 0 || numConnections > NUM_PATHCONNECTIONS ||
	    numCarPathLinks < 0 || numCarPathLinks > NUM_CARPATHLINKS) {
		fprintf(stderr, "implausible counts: nodes %d connections %d carlinks %d\n",
			numPathNodes, numConnections, numCarPathLinks);
		return 1;
	}

	// The whole arrays, so the checker sees exactly the layout the game has.
	std::vector<CPathNode> nodes(NUM_PATHNODES);
	std::vector<uint16_t> connections(NUM_PATHCONNECTIONS);
	std::vector<uint8_t> distances(NUM_PATHCONNECTIONS);
	std::vector<int16_t> carPathConnections(NUM_PATHCONNECTIONS);
	std::vector<CCarPathLink> carPathLinks(NUM_CARPATHLINKS);
	ReadMem(thePaths + offsetof(CPathFind, m_pathNodes), nodes.data(), nodes.size() * sizeof(CPathNode));
	ReadMem(thePaths + offsetof(CPathFind, m_connections), connections.data(), connections.size() * sizeof(uint16_t));
	ReadMem(thePaths + offsetof(CPathFind, m_distances), distances.data(), distances.size());
	ReadMem(thePaths + offsetof(CPathFind, m_carPathConnections), carPathConnections.data(), carPathConnections.size() * sizeof(int16_t));
	ReadMem(thePaths + offsetof(CPathFind, m_carPathLinks), carPathLinks.data(), carPathLinks.size() * sizeof(CCarPathLink));

	FILE *f = fopen(argv[2], "wb");
	if (!f) {
		perror(argv[2]);
		return 1;
	}
	DumpHeader h;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, "RVCPATH1", 8);
	h.version = 1;
	h.numPathNodes = numPathNodes;
	h.numCarPathNodes = numCarPathNodes;
	h.numPedPathNodes = numPedPathNodes;
	h.numConnections = numConnections;
	h.numCarPathLinks = numCarPathLinks;
	h.sizes[0] = sizeof(CPathNode);
	h.sizes[1] = sizeof(CCarPathLink);
	h.sizes[2] = NUM_PATHCONNECTIONS;
	h.sizes[3] = NUM_PATHNODES;
	fwrite(&h, sizeof(h), 1, f);
	fwrite(nodes.data(), sizeof(CPathNode), nodes.size(), f);
	fwrite(connections.data(), sizeof(uint16_t), connections.size(), f);
	fwrite(distances.data(), 1, distances.size(), f);
	fwrite(carPathConnections.data(), sizeof(int16_t), carPathConnections.size(), f);
	fwrite(carPathLinks.data(), sizeof(CCarPathLink), carPathLinks.size(), f);
	fclose(f);

	printf("dumped ThePaths from pid %d (%s, base 0x%llx): %d nodes (%d car, %d ped), %d connections, %d car path links\n",
		pid, exe, (unsigned long long)base, numPathNodes, numCarPathNodes, numPedPathNodes,
		numConnections, numCarPathLinks);
	printf("wrote %s\n", argv[2]);
	return 0;
}
