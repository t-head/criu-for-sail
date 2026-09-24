#ifndef __PPU_KMIF_H__
#define __PPU_KMIF_H__

#include <stdint.h>
#include <stdbool.h>

#include "ppu_ioctl.h"
#include "ppu_plugin_util.h"

typedef struct ppu_criu_device ppu_criu_device_info;
typedef struct ppu_criu_bo     ppu_criu_bo_info;
typedef struct ppu_criu_queue  ppu_criu_queue_info;
typedef struct ppu_criu_util   ppu_criu_util_info;

struct ppu_criu_restore_info {
	uint64_t devices;
	uint64_t bos;
	uint64_t prbs;
	uint64_t queues;
	uint64_t qw;
	uint64_t esr;
	uint64_t mmio;
	uint64_t priv_ptr;
	uint64_t priv_size;
	uint32_t n_dev;
	uint32_t n_bo;
	uint32_t n_prb;
	uint32_t n_queue;
	uint32_t n_qw;
	uint32_t n_esr;
	uint32_t n_mmio;
};

int ppu_support_criu(void);
int ppu_query_process_info(int fd, struct ppu_criu_process_info *info);
int ppu_process_checkpoint(int fd, struct ppu_criu_process_info *info, uint64_t device_ptr,
			   uint64_t queue_ptr, uint64_t bo_ptr, uint64_t prb_ptr,
			   uint64_t qw_ptr, uint64_t esr_ptr, uint64_t mmio_ptr,
			   uint64_t priv_ptr);
int ppu_read_bo_content(int fd, uint64_t va, uint64_t size, void *buff);
int ppu_write_bo_content(int fd, uint64_t va, uint64_t size, void *buff);
int ppu_restore_process(int fd, struct ppu_criu_restore_info *info);
int ppu_restore_process_late(int fd, uint32_t pid);
int ppu_resume_process(int fd, uint32_t pid);
int ppu_pause_devices(int fd, uint32_t pid);
int ppu_get_share_objects_handle_from_fd(int fd, int share_fd, uint32_t *handle);
int ppu_get_share_objects_fd_from_handle(int fd, uint32_t handle, int* share_fd);
int ppu_restore_ctx_create(int fd);

#endif
