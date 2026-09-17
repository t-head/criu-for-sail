#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "criu-plugin.h"
#include "log.h"

#include "ppu_plugin_util.h"

static uint32_t g_ppu_log_level = PPU_LOG_INFO;

void ppu_set_log_level(enum ppu_log_level level)
{
	static const char *log_str[PPU_LOG_COUNT] = {
		"ERROR",
		"MSG",
		"WARN",
		"INFO",
		"DEBUG"
	};

	if (level < PPU_LOG_COUNT) {
		ppu_info("Set log level %s", log_str[level]);
		g_ppu_log_level = level;
	}
}

void ppu_printf(enum ppu_log_level level, const char *format, ...)
{
	int log_fd = log_get_fd();

	if (level <= g_ppu_log_level && log_fd > 0) {
		va_list argList;
		va_start(argList, format);
		vdprintf(log_fd, format, argList);
		va_end(argList);
	}
}

int ppu_getenv_value(const char *var, uint32_t *value)
{
	int ret = 0;
	char *env = getenv(var);
	char *endp = env;

	if (env) {
		uint32_t val = (uint32_t)strtoul(env, &endp, 0);
		if (errno || env == endp) {
			ret = -1;
			ppu_err("Ignoring environment %s=%s", var, env);
		} else {
			*value = val;
		}
	} else {
		ret = -1;
	}

	return ret;
}

int read_fp(FILE *fp, void *buf, const size_t buf_len)
{
	size_t len_read;

	len_read = fread(buf, 1, buf_len, fp);
	if (len_read != buf_len) {
		ppu_err("Unable to read file (read:%ld buf_len:%ld)", len_read, buf_len);
		return -EIO;
	}
	return 0;
}

int write_fp(FILE *fp, const void *buf, const size_t buf_len)
{
	size_t len_write;

	len_write = fwrite(buf, 1, buf_len, fp);
	if (len_write != buf_len) {
		ppu_err("Unable to write file (wrote:%ld buf_len:%ld)", len_write, buf_len);
		return -EIO;
	}
	return 0;
}

/**
 * @brief Open an image file
 *
 * We store the size of the actual contents in the first 8-bytes of
 * the file. This allows us to determine the file size when using
 * criu_image_streamer when fseek and fstat are not available. The
 * FILE * returned is already at the location of the first actual
 * contents.
 *
 * @param path The file path
 * @param write False for read, true for write
 * @param size Size of actual contents
 * @return FILE *if successful, NULL if failed
 */
FILE *open_img_file(char *path, bool write, size_t *size)
{
	FILE *fp = NULL;
	int fd, ret;

	fd = openat(criu_get_image_dir(), path, write ? (O_WRONLY | O_CREAT) : O_RDONLY, 0600);

	if (fd < 0) {
		ppu_err("%s: Failed to open for %s", path, write ? "write" : "read");
		return NULL;
	}

	fp = fdopen(fd, write ? "w" : "r");
	if (!fp) {
		ppu_err("%s: Failed get pointer for %s", path, write ? "write" : "read");
		return NULL;
	}

	if (write)
		ret = write_fp(fp, size, sizeof(*size));
	else
		ret = read_fp(fp, size, sizeof(*size));

	if (ret) {
		ppu_err("%s:Failed to access file size", path);
		fclose(fp);
		return NULL;
	}

	ppu_dbg("%s:Opened file for %s with size:%ld", path, write ? "write" : "read", *size);
	return fp;
}

int read_file(const char *file_path, void *buf, const size_t buf_len)
{
	int ret;
	FILE *fp;

	fp = fopen(file_path, "r");
	if (!fp) {
		ppu_err("Cannot fopen %s", file_path);
		return -errno;
	}

	ret = read_fp(fp, buf, buf_len);
	fclose(fp); /* this will also close fd */
	return ret;
}

int write_img_file(char *path, const void *buf, const uint64_t len)
{
	int ret;
	FILE *fp;
	size_t size = len;

	fp = open_img_file(path, true, &size);
	if (!fp)
		return -errno;

	ret = write_fp(fp, buf, len);
	fclose(fp); /* this will also close fd */
	return ret;
}

void ppu_sleep(uint64_t ms)
{
	struct timespec ts = {
		.tv_sec = ms / 1000,
		.tv_nsec = (ms % 1000) * 1000000,
	};

	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

static int read_comm(int pid, int tid, char *buf, int bufsz)
{
	char path[256] = {};
	snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", pid, tid);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;

	if (!fgets(buf, bufsz, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	int len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
		buf[--len] = '\0';
	}
	return 0;
}

static int is_criu_11hex(const char *s, uint64_t *out_val)
{
	const char prefix[] = "criu";
	const int hexlen = 11;
	if (strncmp(s, prefix, sizeof(prefix) - 1) != 0)
		return 0;

	int len = strlen(s);
	if (len != sizeof(prefix) - 1 + hexlen)
		return 0;

	const char *hex = s + (sizeof(prefix) - 1);
	for (int i = 0; i < hexlen; i++) {
		if (!isxdigit(hex[i]))
			return 0;
	}

	if (out_val)
		*out_val = strtoul(hex, NULL, 16);

	return 1;
}

int ppu_get_criu_restore_tid(int pid, int *p_tid, int *p_read_fd, int *p_write_fd)
{
	char path_task[256] = {};

	snprintf(path_task, sizeof(path_task), "/proc/%d/task", pid);
	DIR *d = opendir(path_task);
	if (!d) {
		ppu_err("Failed to open %s", path_task);
		return -1;
	}

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}

		char *end = NULL;
		int tid = strtol(de->d_name, &end, 10);
		if (!end || *end != '\0' || tid <= 0) {
			continue;
		}

		char comm[32] = {};
		if (read_comm(pid, tid, comm, sizeof(comm))) {
			continue;
		}

		uint64_t value;
		if (!is_criu_11hex(comm, &value)) {
			continue;
		}

		if (p_read_fd) {
			int read_target_fd = value & (0x3fffffu);
			char path_fd[256] = {};

			snprintf(path_fd, sizeof(path_fd), "/proc/%d/fd/%d", pid, read_target_fd);

			int read_fd = open(path_fd, O_RDONLY | O_CLOEXEC);
			if (read_fd == -1) {
				ppu_err("Failed to open %s", path_fd);
				return -1;
			}
			*p_read_fd = read_fd;
		}

		if (p_write_fd) {
			int write_target_fd = (value >> 22) & (0x3fffffu);
			char path_fd[256] = {};

			snprintf(path_fd, sizeof(path_fd), "/proc/%d/fd/%d", pid, write_target_fd);

			int write_fd = open(path_fd, O_WRONLY | O_CLOEXEC);
			if (write_fd == -1) {
				if (p_read_fd) {
					close(*p_read_fd);
					*p_read_fd = -1;
				}
				ppu_err("Failed to open %s", path_fd);
				return -1;
			}
			*p_write_fd = write_fd;
		}

		if (p_tid) {
			*p_tid = tid;
		}

		closedir(d);
		return 0;
	}

	closedir(d);

	ppu_info("Skipping device handling for PID %d", pid);
	return -2;
}

int read_with_timeout(int fd, void *buf, int n, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int ret = poll(&pfd, 1, timeout_ms);

	if (ret == 0) {
		return -ETIMEDOUT;
	}
	if (ret < 0) {
		return -EINVAL;
	}
	return read(fd, buf, n);
}
