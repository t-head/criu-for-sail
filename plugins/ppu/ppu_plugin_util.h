#ifndef __PPU_PLUGIN_UTIL_H__
#define __PPU_PLUGIN_UTIL_H__

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "criu-log.h"

#define PPU_NODE_NAME_LEN    (64)
#define PPU_NODE_NAME_PREFIX "alixpu"
#define PPU_CHAR_NODE_NAME   "/dev/alixpu"
#define PPU_DEV_NODE_NAME    "alixpu_ppu"
#define PPU_DEV_NODE_FMT     "/dev/alixpu_ppu%d"
#define PPU_IMG_FILE	     "ppu-%d-pb.img"
#define PPU_IMG_BO_FILE	     "ppu-%d-bo-ppu%d.img"
#define PPU_IMG_NODE_FILE    "ppu-%d-node.img"
#define PPU_NODE_COUNT_FILE  "ppu-%d-node-count.img"

#define ANON_INODE_PREFIX    "anon_inode:"
#define PPU_IPC_FD_NAME      "alixpu_ipc_obj"

enum ppu_log_level {
	PPU_LOG_ERROR,
	PPU_LOG_MSG,
	PPU_LOG_WARN,
	PPU_LOG_INFO,
	PPU_LOG_DEBUG,
	PPU_LOG_COUNT,
};

void ppu_printf(enum ppu_log_level level, const char *format, ...);

#define ppu_msg(format, arg...)	 ppu_printf(PPU_LOG_MSG, "PPU MSG  [%d] " format "\n", getpid(), ##arg)
#define ppu_err(format, arg...)	 ppu_printf(PPU_LOG_ERROR, "\033[31mPPU ERR  [%d] " format "\033[0m\n", getpid(), ##arg)
#define ppu_warn(format, arg...) ppu_printf(PPU_LOG_WARN, "\033[33mPPU WARN [%d] " format "\033[0m\n", getpid(), ##arg)
#define ppu_info(format, arg...) ppu_printf(PPU_LOG_INFO, "PPU INFO [%d] " format "\n", getpid(), ##arg)
#define ppu_dbg(format, arg...)	 ppu_printf(PPU_LOG_DEBUG, "PPU DBG  [%d] " format "\n", getpid(), ##arg)

void ppu_set_log_level(enum ppu_log_level);
int ppu_getenv_value(const char *var, uint32_t *value);
int write_img_file(char *path, const void *buf, const uint64_t len);
FILE *open_img_file(char *path, bool write, size_t *size);
int write_fp(FILE *fp, const void *buf, const size_t buf_len);
int read_fp(FILE *fp, void *buf, const size_t buf_len);
void ppu_sleep(uint64_t ms);
int ppu_get_criu_restore_tid(int pid, int *restore_tid, int *read_fd, int *write_fd);
int read_with_timeout(int fd, void *buf, int n, int timeout_ms);

#endif
