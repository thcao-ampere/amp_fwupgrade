/*
 * libefiboot - library for the manipulation of EFI boot variables
 * Copyright 2012-2015 Red Hat, Inc.
 * Copyright (C) 2001 Dell Computer Corporation <Matt_Domsch@dell.com>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/nvme.h>
#include <scsi/scsi.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include <efivar.h>
#include <efiboot.h>

#include "dp.h"
#include "linux.h"
#include "util.h"

#ifndef SCSI_IOCTL_GET_IDLUN
#define SCSI_IOCTL_GET_IDLUN 0x5382
#endif

int
__attribute__((__visibility__ ("hidden")))
eb_nvme_ns_id(int fd, uint32_t *ns_id)
{
	uint64_t ret = ioctl(fd, NVME_IOCTL_ID, NULL);
	if ((int)ret < 0)
		return ret;
	*ns_id = (uint32_t)ret;
	return 0;
}

typedef struct scsi_idlun_s {
	uint32_t dev_id;
	uint32_t host_unique_id;
} scsi_idlun;

int
__attribute__((__visibility__ ("hidden")))
eb_scsi_idlun(int fd, uint8_t *host, uint8_t *channel, uint8_t *id,
		   uint8_t *lun)
{
	int rc;
	scsi_idlun idlun = {0, 0};

	if (fd < 0 || !host || !channel || !id || !lun) {
		errno = EINVAL;
		return -1;
	}

	rc = ioctl(fd, SCSI_IOCTL_GET_IDLUN, &idlun);
	if (rc < 0)
		return rc;

	*host =	(idlun.dev_id >> 24) & 0xff;
	*channel = (idlun.dev_id >> 16) & 0xff;
	*lun = (idlun.dev_id >> 8) & 0xff;
	*id = idlun.dev_id & 0xff;
	return 0;
}

/*
 * Look up dynamic major device node numbers.
 */
static int
get_dynamic_major(char *name, int block)
{
	static int cached;
	static char cached_name[1024] = "";
	static int cached_block;

	FILE *f;
	char line[256];
	int seen_block = 0;
	char namenl[strlen(name)+2];

	if (cached != 0 && block==cached_block &&
	    !strncmp(cached_name, name, 1023)) {
		return cached;
	}
	strcpy(namenl, name);
	strcat(namenl, "\n");

	cached = -1;
	f = fopen("/proc/devices", "r");
	if (f == NULL)
		return cached;

	while (fgets(line, sizeof line, f) != NULL) {
		size_t len = strlen(line);
		int major, scanned = 0;

		if (!strcmp(line, "Block devices:\n"))
			seen_block = 1;
		if (len == 0 || line[len - 1] != '\n') {
			break;
		}
		if (seen_block == block &&
		    sscanf(line, "%d %n", &major, &scanned) == 1 &&
		    strcmp(line + scanned, namenl) == 0) {
			cached = major;
			strncpy(cached_name, name, 1023);
			cached_block = block;
			break;
		}
	}
	fclose(f);
	return cached;
}

int
__attribute__((__visibility__ ("hidden")))
eb_ide_pci(int fd, const struct disk_info *info, uint8_t *bus, uint8_t *device,
	   uint8_t *function)
{
	return -1;
}

#ifndef SCSI_IOCTL_GET_PCI
#define SCSI_IOCTL_GET_PCI 0x5387
#endif

/* see scsi_ioctl_get_pci() in linux/drivers/scsi/scsi_ioctl.c */
#define SLOT_NAME_SIZE ((size_t)21)

/* TODO: move this to get it from sysfs? */
int
__attribute__((__visibility__ ("hidden")))
eb_scsi_pci(int fd, const struct disk_info *info, uint8_t *bus,
	    uint8_t *device, uint8_t *function)
{
	char buf[SLOT_NAME_SIZE] = "";
	int rc;
	unsigned int b=0,d=0,f=0;

	/*
	 * Maybe if we're on an old enough kernel,
	 * SCSI_IOCTL_GET_PCI gives b:d.f ...
	 */
	rc = ioctl(fd, SCSI_IOCTL_GET_PCI, buf);
	if (rc < 0)
		return rc;

	rc = sscanf(buf, "%x:%x:%x", &b, &d, &f);
	if (rc != 3) {
		errno = EINVAL;
		return -1;
	}

	*bus = b & 0xff;
	*device = d & 0xff;
	*function = f & 0xff;
	return 0;
}

int
__attribute__((__visibility__ ("hidden")))
get_disk_name(uint64_t major, unsigned char minor,
	      char *pathname, size_t max)
{
	char *linkbuf;
	ssize_t rc;

	rc = sysfs_readlink(&linkbuf, "/sys/dev/block/%"PRIu64":%hhd",
		      major, minor);
	if (rc < 0)
		return -1;

	char *dev;
	dev = strrchr(linkbuf, '/');
	if (!dev) {
		errno = EINVAL;
		return -1;
	}
	*dev = '\0';

	dev = strrchr(linkbuf, '/');
	if (!dev) {
		errno = EINVAL;
		return -1;
	}

	strncpy(pathname, dev+1, max);
	return 0;
}

static int
sysfs_test_sata(const char *buf, ssize_t size)
{
	if (!strncmp(buf, "ata", MIN(size,3)))
		return 1;
	return 0;
}

static int
sysfs_test_sas(const char *buf, ssize_t size, struct disk_info *info)
{
	int rc;
	char *path;
	struct stat statbuf = { 0, };

	int host;
	int sz;

	errno = 0;
	rc = sscanf(buf, "host%d/%n", &host, &sz);
	if (rc < 1)
		return (errno == 0) ? 0 : -1;

	rc = asprintfa(&path, "/sys/class/scsi_host/host%d/host_sas_address",
			host);
	if (rc < 0)
		return -1;

	rc = stat(path, &statbuf);
	if (rc >= 0)
		return 1;
	return 0;
}

static ssize_t
sysfs_sata_get_port_info(uint32_t print_id, struct disk_info *info)
{
	DIR *d;
	struct dirent *de;
	int saved_errno;
	uint8_t *buf = NULL;
	int rc;

	d = opendir("/sys/class/ata_device/");
	if (!d)
		return -1;

	while ((de = readdir(d)) != NULL) {
		uint32_t found_print_id;
		uint32_t found_pmp;
		uint32_t found_devno = 0;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		int rc;
		rc = sscanf(de->d_name, "dev%d.%d.%d", &found_print_id,
			    &found_pmp, &found_devno);
		if (rc == 2) {
			found_devno = found_pmp;
			found_pmp=0;
		} else if (rc != 3) {
			saved_errno = errno;
			closedir(d);
			errno = saved_errno;
			return -1;
		}
		if (found_print_id == print_id) {
			info->sata_info.ata_devno = found_devno;
			info->sata_info.ata_pmp = found_pmp;
			break;
		}
	}
	closedir(d);

	rc = read_sysfs_file(&buf, "/sys/class/ata_port/ata%d/port_no",
			     print_id);
	if (rc <= 0)
		return -1;

	rc = sscanf(
		    (char *)buf,
		    "%d",
		    &info->sata_info.ata_port);
	if (rc != 1)
		return -1;

	info->sata_info.ata_port -= 1;
	return 0;
}

static ssize_t
sysfs_parse_sata(const char *buf, ssize_t size, struct disk_info *info)
{
	ssize_t off = 0;
	int sz = 0;
	int rc;

	uint32_t print_id;

	uint32_t scsi_bus;
	uint32_t scsi_device;
	uint32_t scsi_target;
	uint32_t scsi_lun;

	/* find the ata info:
	 * ata1/host0/target0:0:0/
	 *    ^dev  ^host   x y z
	 */
	rc = sscanf(buf, "ata%d/host%d/target%d:%d:%d/%n",
		    &print_id, &scsi_bus, &scsi_device, &scsi_target, &scsi_lun,
		    &sz);
	if (rc != 5)
		return -1;
	off += sz;

	/* find the emulated scsi bits (and ignore them)
	 * 0:0:0:0/
	 */
	uint32_t dummy0, dummy1, dummy2;
	uint64_t dummy3;
	rc = sscanf(buf+off, "%d:%d:%d:%"PRIu64"/%n", &dummy0, &dummy1,
		    &dummy2, &dummy3, &sz);
	if (rc != 4)
		return -1;
	off += sz;

	/* what's left is:
	 * block/sda/sda4
	 */
	char *disk_name = NULL;
	char *part_name = NULL;
	rc = sscanf(buf+off, "block/%m[^/]/%m[^/]%n", &disk_name, &part_name,
		    &sz);
	if (rc != 2)
		return -1;
	off += sz;

	info->sata_info.scsi_bus = scsi_bus;
	info->sata_info.scsi_device = scsi_device;
	info->sata_info.scsi_target = scsi_target;
	info->sata_info.scsi_lun = scsi_lun;

	rc = sysfs_sata_get_port_info(print_id, info);
	if (rc < 0) {
		free(disk_name);
		free(part_name);
		return -1;
	}

	if (buf[off] != '\0') {
		free(disk_name);
		free(part_name);
		errno = EINVAL;
		return -1;
	}

	info->disk_name = disk_name;
	info->part_name = part_name;
	info->interface_type = sata;

	return off;
}

static ssize_t
sysfs_parse_sas(const char *buf, ssize_t size, struct disk_info *info)
{
	int rc;
	int sz = 0;
	ssize_t off = 0;
	char *filebuf = NULL;
	uint64_t sas_address;

	/* buf is:
	 * host4/port-4:0/end_device-4:0/target4:0:0/4:0:0:0/block/sdc/sdc1
	 */
	uint32_t tosser0, tosser1, tosser2;

	/* ignore a bunch of stuff
	 *    host4/port-4:0
	 * or host4/port-4:0:0
	 */
	rc = sscanf(buf+off, "host%d/port-%d:%d%n", &tosser0, &tosser1,
		    &tosser2, &sz);
	if (rc != 3)
		return -1;
	off += sz;

	sz = 0;
	rc = sscanf(buf+off, ":%d%n", &tosser0, &sz);
	if (rc != 0 && rc != 1)
		return -1;
	off += sz;

	/* next:
	 *    /end_device-4:0
	 * or /end_device-4:0:0
	 * awesomely these are the exact same fields that go into port-blah,
	 * but we don't care for now about any of them anyway.
	 */
	rc = sscanf(buf+off, "/end_device-%d:%d%n", &tosser0, &tosser1, &sz);
	if (rc != 2)
		return -1;
	off += sz;

	sz = 0;
	rc = sscanf(buf+off, ":%d%n", &tosser0, &sz);
	if (rc != 0 && rc != 1)
		return -1;
	off += sz;

	/* now:
	 * /target4:0:0/
	 */
	uint64_t tosser3;
	rc = sscanf(buf+off, "/target%d:%d:%"PRIu64"/%n", &tosser0, &tosser1,
		    &tosser3, &sz);
	if (rc != 3)
		return -1;
	off += sz;

	/* now:
	 * %d:%d:%d:%llu/
	 */
	rc = sscanf(buf+off, "%d:%d:%d:%"PRIu64"/%n", &info->sas_info.scsi_bus,
			    &info->sas_info.scsi_device,
			    &info->sas_info.scsi_target,
			    &info->sas_info.scsi_lun, &sz);
	if (rc != 4)
		return -1;
	off += sz;

	/* what's left is:
	 * block/sdc/sdc1
	 */
	char *disk_name = NULL;
	char *part_name = NULL;
	rc = sscanf(buf+off, "block/%m[^/]/%m[^/]%n", &disk_name, &part_name,
		    &sz);
	if (rc != 2)
		return -1;
	off += sz;

	if (buf[off] != '\0') {
		free(disk_name);
		free(part_name);
		errno = EINVAL;
		return -1;
	}

	/*
	 * we also need to get the actual sas_address from someplace...
	 */
	rc = read_sysfs_file(&filebuf,
			     "/sys/class/block/%s/device/sas_address",
			     disk_name);
	if (rc < 0)
		return -1;

	rc = sscanf(filebuf, "%"PRIx64, &sas_address);
	if (rc != 1)
		return -1;

	info->sas_info.sas_address = sas_address;
	info->disk_name = disk_name;
	info->part_name = part_name;
	info->interface_type = sas;
	return off;
}

int
__attribute__((__visibility__ ("hidden")))
eb_blockdev_pci_fill(struct disk_info *info)
{
	char *linkbuf = NULL;
	int off = 0;
	int sz = 0;
	int rc;

	rc = sysfs_readlink(&linkbuf, "/sys/dev/block/%"PRIu64":%u",
			    info->major, info->minor);
	if (rc < 0)
		return -1;

	/*
	 * find the pci root domain and port; they basically look like:
	 * ../../devices/pci0000:00/
	 *                  ^d   ^p
	 */
	uint16_t root_domain;
	uint8_t root_bus;
	rc = sscanf(linkbuf+off, "../../devices/pci%hx:%hhx/%n",
		    &root_domain, &root_bus, &sz);
	if (rc != 2)
		return -1;
	info->pci_root.root_pci_domain = root_domain;
	info->pci_root.root_pci_bus = root_bus;
	off += sz;

	/* find the pci domain/bus/device/function:
	 * 0000:00:01.0/0000:01:00.0/
	 *              ^d   ^b ^d ^f (of the last one in the series)
	 */
	int found=0;
	while (1) {
		uint16_t domain;
		uint8_t bus, device, function;
		rc = sscanf(linkbuf+off, "%hx:%hhx:%hhx.%hhx/%n",
			    &domain, &bus, &device, &function, &sz);
		if (rc != 4)
			break;
		info->pci_dev.pci_domain = domain;
		info->pci_dev.pci_bus = bus;
		info->pci_dev.pci_device = device;
		info->pci_dev.pci_function = function;
		found=1;
		off += sz;
	}
	if (!found)
		return -1;

	/* /dev/sda as SATA looks like:
	 * /sys/dev/block/8:0 -> ../../devices/pci0000:00/0000:00:1f.2/ata1/host0/target0:0:0/0:0:0:0/block/sda
	 */
	rc = sysfs_test_sata(linkbuf+off, PATH_MAX-off);
	if (rc < 0)
		return -1;
	if (rc > 0) {
		sz = sysfs_parse_sata(linkbuf+off, PATH_MAX-off, info);
		if (sz < 0)
			return sz;
		off += sz;
	}

	/* /dev/sdc as SAS looks like:
	 * /sys/dev/block/8:32 -> ../../devices/pci0000:00/0000:00:01.0/0000:01:00.0/host4/port-4:0/end_device-4:0/target4:0:0/4:0:0:0/block/sdc
	 */
	rc = sysfs_test_sas(linkbuf+off, PATH_MAX-off, info);
	if (rc < 0)
		return -1;
	if (rc > 0) {
		sz = sysfs_parse_sas(linkbuf+off, PATH_MAX-off, info);
		if (sz < 0)
			return -1;
		off += sz;
	}

	if (info->interface_type == scsi) {
		char diskname[PATH_MAX+1]="";
		char *linkbuf;
		rc = get_disk_name(info->major, info->minor, diskname,
				   PATH_MAX);
		if (rc < 0)
			return 0;

		rc = sysfs_readlink(&linkbuf, "/sys/class/block/%s/device",
			      diskname);
		if (rc < 0)
			return 0;

		rc = sscanf(linkbuf, "../../../%d:%d:%d:%"PRIu64,
			    &info->scsi_info.scsi_bus,
			    &info->scsi_info.scsi_device,
			    &info->scsi_info.scsi_target,
			    &info->scsi_info.scsi_lun);
		if (rc != 4)
			return -1;
	}

	return 0;
}

int
__attribute__((__visibility__ ("hidden")))
eb_disk_info_from_fd(int fd, struct disk_info *info)
{
	struct stat buf;
	int rc;

	memset(info, 0, sizeof *info);

	info->pci_root.root_pci_domain = 0xffff;
	info->pci_root.root_pci_bus = 0xff;

	memset(&buf, 0, sizeof(struct stat));
	rc = fstat(fd, &buf);
	if (rc == -1) {
		perror("stat");
		return 1;
	}
	if (S_ISBLK(buf.st_mode)) {
		info->major = buf.st_rdev >> 8;
		info->minor = buf.st_rdev & 0xFF;
	} else if (S_ISREG(buf.st_mode)) {
		info->major = buf.st_dev >> 8;
		info->minor = buf.st_dev & 0xFF;
	} else {
		printf("Cannot stat non-block or non-regular file\n");
		return 1;
	}

	/* IDE disks can have up to 64 partitions, or 6 bits worth,
	 * and have one bit for the disk number.
	 * This leaves an extra bit at the top.
	 */
	if (info->major == 3) {
		info->disknum = (info->minor >> 6) & 1;
		info->controllernum = (info->major - 3 + 0) + info->disknum;
		info->interface_type = ata;
		info->part    = info->minor & 0x3F;
		return 0;
	} else if (info->major == 22) {
		info->disknum = (info->minor >> 6) & 1;
		info->controllernum = (info->major - 22 + 2) + info->disknum;
		info->interface_type = ata;
		info->part    = info->minor & 0x3F;
		return 0;
	} else if (info->major >= 33 && info->major <= 34) {
		info->disknum = (info->minor >> 6) & 1;
		info->controllernum = (info->major - 33 + 4) + info->disknum;
		info->interface_type = ata;
		info->part    = info->minor & 0x3F;
		return 0;
	} else if (info->major >= 56 && info->major <= 57) {
		info->disknum = (info->minor >> 6) & 1;
		info->controllernum = (info->major - 56 + 8) + info->disknum;
		info->interface_type = ata;
		info->part    = info->minor & 0x3F;
		return 0;
	} else if (info->major >= 88 && info->major <= 91) {
		info->disknum = (info->minor >> 6) & 1;
		info->controllernum = (info->major - 88 + 12) + info->disknum;
		info->interface_type = ata;
		info->part    = info->minor & 0x3F;
		return 0;
	}

        /* I2O disks can have up to 16 partitions, or 4 bits worth. */
	if (info->major >= 80 && info->major <= 87) {
		info->interface_type = i2o;
		info->disknum = 16*(info->major-80) + (info->minor >> 4);
		info->part    = (info->minor & 0xF);
		return 0;
	}

	/* SCSI disks can have up to 16 partitions, or 4 bits worth
	 * and have one bit for the disk number.
	 */
	if (info->major == 8) {
		info->interface_type = scsi;
		info->disknum = (info->minor >> 4);
		info->part    = (info->minor & 0xF);
		return 0;
	} else  if (info->major >= 65 && info->major <= 71) {
		info->interface_type = scsi;
		info->disknum = 16*(info->major-64) + (info->minor >> 4);
		info->part    = (info->minor & 0xF);
		return 0;
	} else  if (info->major >= 128 && info->major <= 135) {
		info->interface_type = scsi;
		info->disknum = 16*(info->major-128) + (info->minor >> 4);
		info->part    = (info->minor & 0xF);
		return 0;
	}

	int64_t major = get_dynamic_major("nvme", 1);
	if (major >= 0 && (uint64_t)major == info->major) {
		info->interface_type = nvme;
		return 0;
	}

	major = get_dynamic_major("virtblk", 1);
	if (major >= 0 && (uint64_t)major == info->major) {
		info->interface_type = virtblk;
		info->disknum = info->minor >> 4;
		info->part = info->minor & 0xF;
		return 0;
	}

	errno = ENOSYS;
	return -1;
}
