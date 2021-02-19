// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Ampere FW uprade based on efivar to use EFI variables to
 * execute FW upgrade on Ampere's platform.
 * 
 * libefivar - library for the manipulation of EFI variables
 * Copyright 2012 Red Hat, Inc.
 * Copyright 2021 Ampere Computing LLC.
 */

#include "fix_coverity.h"

#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

extern char *optarg;
extern int optind, opterr, optopt;

#include "efivar.h"

#define VER_MAJOR	1
#define VER_MINOR	0

#define ACTION_USAGE		0x00
#define ACTION_UPGRADE		0x01

static int verbose = 0;
static char fwupgrade_guid[] = {"38b9ed29-d7c6-4bf4-9678-9da058bd2e99"};
static char full_fw_name[] = {"UpgradeATFUEFIRequest"};
static char uefi_fw_name[] = {"UpgradeUEFIRequest"};
static char ueficfg_fw_name[] = {"UpgradeCFGUEFIRequest"};
static char scp_fw_name[] = {"UpgradeSCPRequest"};

static int
parse_status(char *data, char **str_left, char **str_right)
{
	if (!data)
		return -1;

	*str_left = strchr(data, ',');
	if (!*str_left)
		return -1;

	data[(long)(*str_left) - (long)data] = '\0';
	*str_right = *str_left + 1;
	*str_left = data;

	return 0;
}

static void poll_status(const char *name)
{
	char *str_status = NULL;
	size_t str_status_size = 0;
	uint32_t attributes = 0;
	efi_guid_t guid;
	char *str_left, *str_right;
	int rc;
	unsigned long ul;
	int first_run = true;
	int num_retry = 5;

	rc = text_to_guid(fwupgrade_guid, &guid);
	if (rc < 0)
		return;

	do {
		sleep(1);
		rc = efi_get_variable(guid, name, (uint8_t **)&str_status, &str_status_size,
				&attributes);
		if (rc < 0) {
			if (num_retry > 0) {
				if (verbose) {
					fprintf(stderr, "amp_fwupgrade(%d): %m\n", __LINE__);
					fprintf(stderr, "amp_fwupgrade(%d): retrying\n", __LINE__);
				}
				num_retry--;
				continue;
			}
			fprintf(stderr, "amp_fwupgrade(%d): %m\n", __LINE__);
			exit(1);
		}

		str_status[str_status_size - 1] = '\0';
		rc = parse_status(str_status, &str_left, &str_right);
		if (rc < 0) {
			fprintf(stderr, "amp_fwupgrade(%d): failed to parse status\n", __LINE__);
			if (str_status)
				free(str_status);
			exit(1);
		}

		if (!strcmp(str_left, "NULL")) {
			/* Not started */
			free(str_status);
			return;
		}
		if (strstr(str_right, "IN_PROCESS,")) {
			if (first_run)
				fprintf(stdout, "Upgrading %s ", str_left);
			rc = parse_status(str_right, &str_left, &str_right);
			if (rc < 0) {
				fprintf(stderr, "\namp_fwupgrade(%d): failed to parse percentage process\n", __LINE__);
				free(str_status);
				exit(1);
			}
			ul = strtoul(str_right, NULL, 0);
			if (ul > 0) {
				if (first_run)
					fprintf(stdout, "processed: %2lu%%", ul);
				else
					fprintf(stdout, "\b\b\b%2lu%%", ul);
			}
			fflush(stdout);
		} else {
			if (!strcmp(str_right, "SUCCESS"))
				fprintf(stdout, "\nUpgraded %s succesfully\n", str_left);
			else
				fprintf(stderr, "\nError while upgrading %s with status %s\n", str_left, str_right);
			free(str_status);
			return;
		}
		if (first_run)
			first_run = false;
		free(str_status);
	} while (1);
}

static void
start_fwupgrade(const char *name, void *data, size_t data_size)
{
	size_t str_status_size = 0;
	uint32_t attributes = 0;
	efi_guid_t guid;
	char *str_status = NULL;
	char *str_left, *str_right;
	int rc;

	rc = text_to_guid(fwupgrade_guid, &guid);
	if (rc < 0)
		return;

	rc = efi_get_variable(guid, name, (uint8_t **)&str_status, &str_status_size,
			      &attributes);
	if (rc == 0) {
		rc = parse_status(str_status, &str_left, &str_right);
		if (rc < 0) {
			fprintf(stderr, "amp_fwupgrade(%d): failed to parse status\n", __LINE__);
			if (str_status)
				free(str_status);
			exit(1);
		}

		/* Only upgrade if has not started */
		if (strstr(str_right, "IN_PROCESS")) {
			fprintf(stderr, "amp_fwupgrade: Can't start upgrading: (%s,%s)\n", str_left, str_right);
			free(str_status);
			exit(1);
		}
	}

	rc = efi_set_variable(guid, name,
				data, data_size,
				EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_BOOTSERVICE_ACCESS, 0644);

	free(str_status);

	if (rc < 0) {
		fprintf(stderr, "amp_fwupgrade(%d): %m\n", __LINE__);
		exit(1);
	}
	fprintf(stdout, "amp_fwupgrade: Upgrade is in process, do not terminate this application\n");
}

static void
prepare_data(const char *filename, uint8_t **data, size_t *data_size)
{
	int fd = -1;
	void *buf;
	size_t buflen = 0;
	struct stat statbuf;
	int rc;

	if (filename == NULL) {
		fprintf(stderr, "Input filename must be provided.\n");
		exit(1);
	}

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		goto err;

	memset(&statbuf, '\0', sizeof (statbuf));
	rc = fstat(fd, &statbuf);
	if (rc < 0)
		goto err;

	buflen = statbuf.st_size;
	buf = mmap(NULL, buflen, PROT_READ, MAP_PRIVATE|MAP_POPULATE, fd, 0);
	if (buf == MAP_FAILED)
		goto err;

	*data = buf;
	*data_size = buflen;

	close(fd);
	return;
err:
	if (fd >= 0)
		close(fd);
	fprintf(stderr, "Could not use \"%s\": %m\n", filename);
	exit(1);
}

static void __attribute__((__noreturn__))
usage(int ret)
{
	FILE *out = ret == 0 ? stdout : stderr;
	fprintf(out,
		"Usage: %s [OPTION...]\n"
		"  -a, --fullfw=<file>             Upgrade full firmware (excluding SCP) from <file>\n"
		"  -c, --uefiandcfg=<file>         Upgrade only UEFI and board settings from <file>\n"
		"  -u, --uefi=<file>               Upgrade only UEFI from <file>\n"
		"  -s, --scp=<file>                Upgrade SCP from <file>\n"
		"Help options:\n"
		"  -?, --help                      Show this help message\n"
		"      --usage                     Display brief usage message\n"
		"      --version                   Display version and copyright information\n",
		program_invocation_short_name);
	exit(ret);
}

static void __attribute__((__noreturn__))
	show_version(int ret)
{
    FILE *out = ret == 0 ? stdout : stderr;
    fprintf(out,
        "%s (Ampere Firmware Upgrade) version %d.%d\n\n"
        "Copyright 2012 Red Hat, Inc.\n"
        "Copyright 2021 Ampere Computing LLC.\n"
        "SPDX-License-Identifier: LGPL-2.1-or-later.\n",
		program_invocation_short_name, VER_MAJOR, VER_MINOR);
        exit(ret);
}


int main(int argc, char *argv[])
{
	int c = 0;
	int i = 0;
	int action = 0;
	uint8_t *data = NULL;
	size_t data_size = 0;
	char *infile = NULL;
	char *name = NULL;
	char *sopts = "a:c:u:s:v?V";
	struct option lopts[] = {
		{"fullfw", required_argument, 0, 'a'},
		{"uefiandcfg", required_argument, 0, 'c'},
		{"uefi", required_argument, 0, 'u'},
		{"scp", required_argument, 0, 's'},
		{"help", no_argument, 0, '?'},
		{"usage", no_argument, 0, 0},
               {"verbose", no_argument, 0, 'v'},
               {"version", no_argument, 0, 'V'},
		{0, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, sopts, lopts, &i)) != -1) {
		switch (c) {
			case 'a':
				infile = optarg;
				name = full_fw_name;
				action |= ACTION_UPGRADE;
				break;
			case 'c':
				infile = optarg;
				name = ueficfg_fw_name;
				action |= ACTION_UPGRADE;
				break;
			case 'u':
				infile = optarg;
				name = uefi_fw_name;
				action |= ACTION_UPGRADE;
				break;
			case 's':
				infile = optarg;
				name = scp_fw_name;
				action |= ACTION_UPGRADE;
				break;
			case 'v':
				verbose += 1;
				break;
			case 'V':
				show_version(EXIT_SUCCESS);
				break;
			case '?':
				usage(EXIT_SUCCESS);
				break;
			case 0:
				if (strcmp(lopts[i].name, "usage"))
					usage(EXIT_SUCCESS);
				break;
		}
	}

	efi_set_verbose(verbose, stderr);

	switch (action) {
		case ACTION_UPGRADE:
			prepare_data(infile, &data, &data_size);
			start_fwupgrade(name, data, data_size);
			poll_status(name);
			break;
		case ACTION_USAGE:
		default:
			usage(EXIT_FAILURE);
	};

	return 0;
}
