/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "argos.h"

static char bss_buf[ARGOS_STR_LEN];

static void read_dt_string(const char *path, char *dest, size_t dest_sz)
{
	dest[0] = '\0';
	FILE *fp = fopen(path, "r");
	if (!fp)
		return;

	if (fgets_unlocked(bss_buf, sizeof(bss_buf), fp))
	{
		size_t len = strlen(bss_buf);
		if (len > 0 && bss_buf[len - 1] == '\n')
			bss_buf[len - 1] = '\0';
		snprintf(dest, dest_sz, "%s", bss_buf);
	}
	fclose(fp);
}

static uint64_t read_dt_interval(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 1000;

	uint8_t buf[8] = {0};
	ssize_t n = read(fd, buf, sizeof(buf));
	close(fd);

	if (n >= 4)
	{
		uint32_t interval_idx = ((uint32_t)buf[0] << 24) |
		                        ((uint32_t)buf[1] << 16) |
		                        ((uint32_t)buf[2] << 8)  |
		                        (uint32_t)buf[3];
		if (interval_idx == 2)
			return 250;
		if (interval_idx == 1)
			return 500;
	}

	return 1000;
}

argos_state_t *argos_boot(void)
{
	DIR *dir = opendir(ARGOS_DT_PATH);
	if (!dir)
	{
		fprintf(stderr, "opendir failed, %s\n", strerror(errno));
		return NULL;
	}

	int count = 0;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL)
	{
		if (strchr(ent->d_name, '@'))
			count++;
	}
	closedir(dir);

	if (count < 1)
		return NULL;

	argos_device_t *devices = malloc(count * sizeof(argos_device_t));
	if (!devices)
	{
		ALOGE("argos: (ERR! %s) %s: malloc fail\n", strerror(errno), "argos_monitor");
		return NULL;
	}
	memset(devices, 0, count * sizeof(argos_device_t));

	int net_thru_fd = open(ARGOS_NET_THROUGHPUT_DEV, O_RDWR);
	if (net_thru_fd < 0)
	{
		ALOGE("argos: (ERR! %s) %s: Device doesn't support qos\n", strerror(errno), "argos_monitor");
		free(devices);
		return NULL;
	}

	uint64_t min_interval = 1000;
	char path[ARGOS_STR_LEN];

	for (int i = 0; i < count; ++i)
	{
		int dev_num = i + 1;
		argos_device_t *dev = &devices[i];

		snprintf(path, sizeof(path), "%s%d%s", ARGOS_BOOT_DEVICE_PREFIX, dev_num, "/net_boost,node");
		read_dt_string(path, dev->node, sizeof(dev->node));

		snprintf(path, sizeof(path), "%s%d%s", ARGOS_BOOT_DEVICE_PREFIX, dev_num, "/net_boost,label");
		read_dt_string(path, dev->label, sizeof(dev->label));

		snprintf(path, sizeof(path), "%s%d%s", ARGOS_BOOT_DEVICE_PREFIX, dev_num, "/net_boost,interval_idx");
		dev->interval = read_dt_interval(path);

		dev->prev_bytes = 0;
		dev->prev_mbps = 0;

		ALOGD("argos: %s: NODE:%s", "argos_monitor", dev->node);
		ALOGD("argos: %s: LABEL:%s", "argos_monitor", dev->label);
		ALOGD("argos: %s: INTERVAL:%lu", "argos_monitor", (unsigned long)dev->interval);

		if (dev->interval < min_interval)
			min_interval = dev->interval;
	}

	ALOGD("argos: %s: MIN INTERVAL:%lu", "argos_monitor", (unsigned long)min_interval);

	argos_state_t *state = malloc(sizeof(argos_state_t));
	if (!state)
	{
		close(net_thru_fd);
		free(devices);
		return NULL;
	}

	state->count = count;
	state->net_thru_fd = net_thru_fd;
	state->min_interval = min_interval;
	state->devices = devices;

	return state;
}

void argos_shutdown(argos_state_t *state)
{
	if (!state)
		return;

	if (state->net_thru_fd >= 0)
		close(state->net_thru_fd);

	if (state->devices)
		free(state->devices);

	free(state);
}

int argos_run(argos_state_t *state)
{
	if (!state)
		return -ENODEV;

	uint64_t time_ms = 0;
	char path[ARGOS_STR_LEN];
	char line[1024];

	while (1)
	{
		for (int i = 0; i < state->count; ++i)
			state->devices[i].total_bytes = 0;

		for (int i = 0; i < state->count; ++i)
		{
			argos_device_t *dev = &state->devices[i];
			if (dev->node[0] == '\0')
			{
				snprintf(path, sizeof(path), "%s%d%s", ARGOS_BOOT_DEVICE_PREFIX, i + 1, "/net_boost,sysnode");
				FILE *dt_fp = fopen(path, "r");
				if (dt_fp)
				{
					char sysfs_path[ARGOS_STR_LEN] = {0};
					if (fgets_unlocked(sysfs_path, sizeof(sysfs_path), dt_fp))
					{
						size_t plen = strlen(sysfs_path);
						if (plen > 0 && sysfs_path[plen - 1] == '\n')
							sysfs_path[plen - 1] = '\0';
					}
					fclose(dt_fp);

					if (sysfs_path[0] != '\0')
					{
						FILE *sysfs_fp = fopen(sysfs_path, "r");
						if (sysfs_fp)
						{
							char val_buf[ARGOS_STR_LEN] = {0};
							if (fgets_unlocked(val_buf, sizeof(val_buf), sysfs_fp))
							{
								dev->sysnode_val = strtoull(val_buf, NULL, 10);
								dev->total_bytes += dev->sysnode_val;
							}
							fclose(sysfs_fp);
						}
					}
				}
			}
		}

		FILE *net_fp = fopen(ARGOS_PROC_NET_DEV, "r");
		if (!net_fp)
		{
			ALOGE("argos: (ERR! %s) %s: Fail to open node with(%d)\n",
			           strerror(errno), "argos_monitor", errno);
			if (state->net_thru_fd >= 0)
			{
				close(state->net_thru_fd);
				state->net_thru_fd = -1;
			}
			free(state->devices);
			state->devices = NULL;
			return -ENOENT;
		}

		if (!fgets_unlocked(line, sizeof(line), net_fp)) {}
		if (!fgets_unlocked(line, sizeof(line), net_fp)) {}

		while (fgets_unlocked(line, sizeof(line), net_fp))
		{
			size_t len = strlen(line);
			if (len > 0)
				line[len - 1] = '\0';

			char *p = line;
			while (*p && (*p == ' ' || (*p >= '\t' && *p <= '\r')))
				p++;

			for (int i = 0; i < state->count; ++i)
			{
				argos_device_t *dev = &state->devices[i];
				char iface[32] = {0};
				unsigned long long rx = 0;
				unsigned long long tx = 0;

				sscanf(p, "%30[^:]%*[:] %20llu %*s %*s %*s %*s %*s %*s %*s %20llu", iface, &rx, &tx);
				dev->rx_bytes = rx;
				dev->tx_bytes = tx;

				char *node_copy = strdup(dev->node);
				if (node_copy)
				{
					char *saveptr = NULL;
					char *token = strtok_r(node_copy, " ", &saveptr);
					int match = 0;
					while (token)
					{
						if (strcmp(token, iface) == 0)
						{
							match = 1;
							break;
						}
						token = strtok_r(NULL, " ", &saveptr);
					}
					free(node_copy);

					if (match)
					{
						size_t label_len = strlen(dev->label);
						char *label_end = dev->label + (label_len >= 2 ? label_len - 2 : 0);

						if (strcmp(label_end, "RX") != 0)
							dev->total_bytes += dev->tx_bytes;

						if (strcmp(label_end, "TX") != 0)
							dev->total_bytes += dev->rx_bytes;
					}
				}
			}
		}
		fclose(net_fp);

		for (int i = 0; i < state->count; ++i)
		{
			argos_device_t *dev = &state->devices[i];

			if (time_ms % dev->interval == 0)
			{
				if (dev->total_bytes >= dev->prev_bytes)
				{
					uint64_t delta = dev->total_bytes - dev->prev_bytes;
					dev->delta_bytes = delta;
					uint32_t mbps = (uint32_t)((delta * 8) / 1000000);

					if (mbps != 0 || dev->prev_mbps != 0)
					{
						char write_buf[16];
						sprintf(write_buf, "0x%x%x\n", mbps, i + 1);
						ssize_t w = write(state->net_thru_fd, write_buf, strlen(write_buf) + 1);
						(void)w;

						ALOGD("argos: %s: %s - Mbps:%u prev_tput:%lu",
						           "argos_monitor", dev->label, mbps, (unsigned long)dev->prev_mbps);
					}

					dev->prev_mbps = mbps;
				}
				dev->prev_bytes = dev->total_bytes;
			}
		}

		time_ms = (time_ms + state->min_interval) % 1000;
		usleep((useconds_t)(state->min_interval * 1000));
	}

	return 0;
}

int argos_monitor(void)
{
	argos_state_t *state = argos_boot();
	if (!state)
		return -ENODEV;

	int ret = argos_run(state);
	argos_shutdown(state);
	return ret;
}
