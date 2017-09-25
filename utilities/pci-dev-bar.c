/*
 * This program is used to dump, read or write on the specified PCI
 * device memory BAR. Note that it's not figured out to access IO
 * BAR so far, which is something to be sorted out in future.
 *
 * Copyright Gavin Shan, Alibaba Inc 2017.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

struct pci_dev_bar_op {
	unsigned int	op;
#define PCI_DEV_BAR_OP_MIN		0
#define PCI_DEV_BAR_OP_DUMP		1
#define PCI_DEV_BAR_OP_READ		2
#define PCI_DEV_BAR_OP_WRITE		3
#define PCI_DEV_BAR_OP_MAX		4

	unsigned int	bar_index;
	unsigned long	bar_size;
	void		*bar_mmap;
	unsigned long	offset;
	unsigned long	length;
	unsigned long	val;
	unsigned int	verbose;
};

static const char *sys_path = "/sys/bus/pci/devices";
struct pci_dev_bar_op op;

#define PCI_DEV_BAR_DBG(fmt...)		\
	if (op.verbose)			\
		fprintf(stdout, fmt)

static void usage(void)
{
	fprintf(stdout, "\n");
	fprintf(stdout, "pci-dev-bar - Dump, read or write on the specified PCI device memory BAR\n\n");
	fprintf(stdout, "   -d   PCI device in generic format (dddd:bb:ss.f). dddd is four digits for\n");
	fprintf(stdout, "        PCI domain. bb is two digits for PCI bus number. ss is two digits for\n");
	fprintf(stdout, "        slot number and f is one digit for function number. All values are all\n");
	fprintf(stdout, "        in hexadecimal numbers\n\n");
	fprintf(stdout, "   -b   PCI device BAR index in range of 0 to 6. It is either IO or memory BAR.\n");
	fprintf(stdout, "        We are able to access memory BAR only\n\n");
	fprintf(stdout, "   -o   BAR offset where the specified options to be issued. It is 0 by default.\n\n");
	fprintf(stdout, "   -l   The length of bytes to be dumped, read or written. It is the BAR size\n");
	fprintf(stdout, "        for dumping, 4 bytes for reading and 1 byte for writing. It must even\n");
	fprintf(stdout, "        number less or equal than 8 on writing value to the BAR.\n\n");
	fprintf(stdout, "   -t   Value to be written to the BAR offset.\n\n");
	fprintf(stdout, "   -v   Verbose option to output debugging messages.\n\n");
	fprintf(stdout, "   -h   Show usage information.\n\n");
}

/* Check if PCI device file exists or not */
static int check_pci_dev(const char *devname)
{
	char path[256];
	struct stat stats;
	int ret;

	snprintf(path, 256, "%s/%s", sys_path, devname);
	ret = stat(path, &stats);
	if (ret) {
		PCI_DEV_BAR_DBG("PCI device file <%s> doesn't exist\n", path);
		return -ENOENT;
	}

	return 0;
}

/*
 * Check if PCI device BAR file exists or not. Also, the BAR's index and
 * its size are returned.
 */
static int check_pci_dev_bar(const char *devname, const char *barname)
{
	FILE *f = NULL;
	char path[256];
	struct stat stats;
	char *buf = NULL;
	size_t buf_size;
	ssize_t read_size;
	unsigned long start, end, flags;
	int index, i, ret = 0;

	/* Get BAR index */
	if (sscanf(barname, "%d", &index) != 1) {
		PCI_DEV_BAR_DBG("No valid BAR index found in <%s>\n", barname);
		return -EINVAL;
	} else if (index >= 6) {
		PCI_DEV_BAR_DBG("BAR index %d should be less than 6\n", index);
		return -EINVAL;
	}

	/* Validate BAR device file */
	snprintf(path, 256, "%s/%s/resource%d", sys_path, devname, index);
	if (stat(path, &stats)) {
		PCI_DEV_BAR_DBG("BAR file <%s> doesn't exist\n", path);
		return -ENOENT;
	}

	/* Check BAR flags and get its size */
	buf = (char *)malloc(256);
	if (!buf) {
		PCI_DEV_BAR_DBG("Can't allocate buffer to read BAR file\n");
		return -ENOMEM;
	}

	snprintf(path, 256, "%s/%s/resource", sys_path, devname);
	f = fopen(path, "r");
	if (!f) {
		PCI_DEV_BAR_DBG("Can't open <%s>\n", path);
		ret = -EIO;
		goto out;
	}

	for (i = 0; i < 6; i++) {
		read_size = getline(&buf, &buf_size, f);
		if (i == index)
			break;
	}
	if (!read_size || !buf_size) {
		PCI_DEV_BAR_DBG("Can't read file <%s>\n", path);
		ret = -EIO;
		goto out;
	}

	if (sscanf(buf, "0x%lx 0x%lx 0x%lx", &start, &end, &flags) != 3) {
		PCI_DEV_BAR_DBG("Can't identify BAR#%d's range from <%s>\n", index, buf);
		ret = -EINVAL;
		goto out;
	} else if (start >= end) {
		PCI_DEV_BAR_DBG("Invalid BAR#%d range <%s>\n", index, buf);
		ret = -ERANGE;
		goto out;
	} else if (!(flags & 0x200)) {
		PCI_DEV_BAR_DBG("BAR#%d <%s> isn't memory BAR\n", index, buf);
		ret = -EINVAL;
		goto out;
	}

	op.bar_index = index;
	op.bar_size = end - start + 1;

out:
	free(buf);
	if (f)
		fclose(f);
	return ret;
}

static int to_hex(const char *id, char *str, unsigned long *pval)
{
	char *p;

	*pval = strtol(str, &p, 16);
	if (p == str) {
		PCI_DEV_BAR_DBG("Unable to parse %s\n", id);
		return -EINVAL;
	}

	return 0;
}

static void do_read_bar(void)
{
	unsigned long i;

	for (i = 0; i < (op.offset & 0xful); i++) {
		if (i == 0ul)
			fprintf(stdout, "%08lx: ", op.offset + 1);

		fprintf(stdout, ".. ");
	}

	for (i = op.offset; i < (op.offset + op.length); i++) {
		if (i % 16 == 0)
			fprintf(stdout, "%08lx: ", i);

		fprintf(stdout, "%02x ", *((unsigned char *)(op.bar_mmap + i)));

		if ((i & 0xful) == 0xful ||
		    (i == (op.offset + op.length - 1)))
			fprintf(stdout, "\n");
	}
}

static void do_write_bar(void)
{
	unsigned long i;
	unsigned char *base = (unsigned char *)(op.bar_mmap + op.offset);

	for (i = 0; i < op.length; i++)
		*(base + i) = (unsigned char)((op.val >> (8 * i)) & 0xfful);
}

int main(int argc, char **argv)
{
	char path[256];
	char *devname = NULL, *barname = NULL;
	int opt, fd, ret = 0;

	while ((opt = getopt(argc, argv, "hvdrws:b:o:l:t")) != -1) {
		switch (opt) {
		case 'd':
			op.op = PCI_DEV_BAR_OP_DUMP;
			break;
		case 'r':
			op.op = PCI_DEV_BAR_OP_READ;
			break;
		case 'w':
			op.op = PCI_DEV_BAR_OP_WRITE;
			break;
		case 's':
			devname = optarg;
			break;
		case 'b':
			barname = optarg;
			break;
		case 'o':
			ret = to_hex("offset", optarg, &op.offset);
			if (ret)
				return ret;
			break;
		case 'l':
			ret = to_hex("length", optarg, &op.length);
			if (ret)
				return ret;

			break;
		case 't':
			ret = to_hex("value", optarg, &op.val);
			if (ret)
				return ret;
			break;
		case 'v':
			op.verbose = 1;
			break;
		case 'h':
		default:
			usage();
			return 0;
		}
	}

	/* Check operation */
	if (op.op <= PCI_DEV_BAR_OP_MIN ||
	    op.op >= PCI_DEV_BAR_OP_MAX) {
		PCI_DEV_BAR_DBG("Operation must be specified\n");
		return -EINVAL;
	}

	/* Check device name */
	if (!devname) {
		PCI_DEV_BAR_DBG("PCI device should be given\n");
		return -EINVAL;
	}

	ret = check_pci_dev(devname);
	if (ret)
		return ret;

	/* Check BAR name */
	if (!barname) {
		PCI_DEV_BAR_DBG("BAR name should be given\n");
		return -EINVAL;
	}

	ret = check_pci_dev_bar(devname, barname);
	if (ret)
		return ret;

	/* Set default length if needed */
	if (op.op == PCI_DEV_BAR_OP_DUMP ) {
		op.offset = 0;
		op.length = op.bar_size;
	} else if (op.op == PCI_DEV_BAR_OP_READ) {
		op.length = op.length ? op.length : 4;
	} else if (op.op == PCI_DEV_BAR_OP_WRITE) {
		op.length = op.length ? op.length : 1;
	}

	/* Check offset */
	if ((op.op == PCI_DEV_BAR_OP_DUMP  ||
	     op.op == PCI_DEV_BAR_OP_READ) &&
	    (op.offset + op.length) > op.bar_size) {
		PCI_DEV_BAR_DBG("Out of range on dumping or reading (0x%lx + 0x%lx) > 0x%lx\n",
				op.offset, op.length, op.bar_size);
		return -ERANGE;
	} else if (op.op == PCI_DEV_BAR_OP_WRITE	&&
		   !(op.length  >= 1 && op.length <= 8)	&&
		   (op.length & (op.length - 1))) {
		PCI_DEV_BAR_DBG("Illegal length (0x%lx) specified for writing\n", op.length);
		return -EINVAL;
	}

	/* Open PCI device BAR file and mmap it */
	snprintf(path, 256, "%s/%s/resource%d", sys_path, devname, op.bar_index);
	fd = open(path, O_RDWR);
	if (fd <= 0) {
		PCI_DEV_BAR_DBG("Unable to open file <%s>\n", path);
		return fd;
	}

	op.bar_mmap = mmap(NULL, op.bar_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0ul);
	if (op.bar_mmap == MAP_FAILED) {
		PCI_DEV_BAR_DBG("Unable to map file <%s>\n", path);
		ret = -EIO;
		goto out;
	}

	/* Issue the operation */
	switch (op.op) {
	case PCI_DEV_BAR_OP_WRITE:
		do_write_bar();
		break;
	case PCI_DEV_BAR_OP_READ:
	case PCI_DEV_BAR_OP_DUMP:
		do_read_bar();
		break;
	}

out:
	if (op.bar_mmap != MAP_FAILED)
		munmap(op.bar_mmap, op.bar_size);

	close(fd);
	return ret;
}
