#ifndef __PPU_IOCTL_H__
#define __PPU_IOCTL_H__

#include <linux/types.h>
#include <asm/ioctl.h>

#if defined(__cplusplus)
extern "C" {
#endif

struct ppu_criu_device {
	__u32 dev_index;
	__u32 actual_ppuid;
};

enum ppu_criu_bo_type {
	PPU_CRIU_BO_TYPE_NORMAL        = 1 << 0,
	PPU_CRIU_BO_TYPE_IMPORTED      = 1 << 1,
	PPU_CRIU_BO_TYPE_EXPORTED      = 1 << 2,
	PPU_CRIU_BO_TYPE_POOL          = 1 << 3,
	PPU_CRIU_BO_TYPE_MMAPPED       = 1 << 4,
	PPU_CRIU_BO_TYPE_UNIFIED       = 1 << 5,
	PPU_CRIU_BO_TYPE_HOST_REGISTER = 1 << 6,
};

struct ppu_criu_bo {
	__u64 addr;
	__u64 size;
	__u64 offset;
	__u64 restored_offset;
	__u64 sema_addr;
	__u64 sema_offset;
	__u64 sema_restored_offset;
	__u32 dev_index;
	__u32 rst_flags;
	__u32 type; /* enum ppu_criu_bo_type */
	__u32 n_prb;
};

struct ppu_criu_queue {
	__u64 addr;
	__u32 dev_index;
	__u32 size;
	__u64 offset;
	__u64 restored_offset;
};

struct ppu_criu_util {
	__u64 addr;
	__u32 dev_index;
	__u32 size;
	__u64 offset;
	__u64 restored_offset;
};

#define PPU_CRIU_GET_VERSION		     0
#define PPU_CRIU_PROCESS_INFO		     1
#define PPU_CRIU_CHECKPOINT		     2
#define PPU_CRIU_UNPAUSE		     4
#define PPU_CRIU_RESTORE		     5
#define PPU_CRIU_RESUME			     6
#define PPU_CRIU_PAUSE_DEVICES		     7
#define PPU_CRIU_RESTORE_LATE		     8
#define PPU_CRIU_CHECKPOINT_MEMORY	     9
#define PPU_CRIU_RESTORE_MEMORY		     10
#define PPU_CRIU_GET_MEMORY_USAGE	     11
#define PPU_CRIU_CREATE_SHARE_FD_FROM_HANDLE 12
#define PPU_CRIU_RESTORE_CTX_CREATE          13

struct ppu_criu_in {
	__u32 op;
	__u32 pad;
	union {
		struct {
			__u32 n_dev;
			__u32 n_bo;
			__u32 n_prb;
			__u32 n_queue;
			__u32 n_qw;
			__u32 n_esr;
			__u32 n_mmio;
			__u32 pad;
			__u64 device_ptr;
			__u64 queue_ptr;
			__u64 bo_ptr;
			__u64 prb_ptr;
			__u64 qw_ptr;
			__u64 esr_ptr;
			__u64 mmio_ptr;
			__u64 priv_ptr;
			__u64 priv_size;
		} checkpoint;
		struct {
			__u32 n_dev;
			__u32 n_bo;
			__u32 n_prb;
			__u32 n_queue;
			__u32 n_qw;
			__u32 n_esr;
			__u32 n_mmio;
			__u32 pad;
			__u64 device_ptr;
			__u64 queue_ptr;
			__u64 bo_ptr;
			__u64 prb_ptr;
			__u64 qw_ptr;
			__u64 esr_ptr;
			__u64 mmio_ptr;
			__u64 priv_ptr;
			__u64 priv_size;
		} restore;
		struct {
			__u32 pid;
			__u32 pad;
		} restore_late;
		struct {
			__u32 pid;
			__u32 pad;
		} resume;
		struct {
			__u32 pid;
			__u32 pad;
		} pause_devices;
		struct {
			__u32 bo_handle;
			__u32 pad;
		} cr_device;
		struct {
			__u64 unified_info;
			__u64 unified_count;
		} get_memory_usage;
		struct {
			__u32 handle;
			__u32 pad;
		} create_share_fd;
	};
};

enum ppu_criu_bo_prb_range_type {
	PPU_CRIU_BO_PRB_RANGE_TYPE_ALIAS = 0,
	PPU_CRIU_BO_PRB_RANGE_TYPE_RESIDENT,
};

struct ppu_criu_bo_prb_range {
	enum ppu_criu_bo_prb_range_type type;
	__u32 pad;
	__u64 offset;
	__u64 alias;
	__u64 size;
};

struct ppu_criu_process_info {
	__u32 pid;
	__u32 n_dev;
	__u32 n_bo;
	__u32 n_prb;
	__u32 n_queue;
	__u32 n_qw;
	__u32 n_esr;
	__u32 n_mmio;
	__u64 priv_size;
};

union ppu_criu_out {
	struct {
		__u32 major;
		__u32 minor;
	} version;
	struct ppu_criu_process_info process_info;
	struct {
		__u64 size;
		__u64 unified_size;
		__u64 unified_count;
	} get_memory_usage;
	struct {
		__s32 fd;
		__u32 pad;
	} create_share_fd;
};

union ppu_criu_args {
	struct ppu_criu_in in;
	union ppu_criu_out out;
};

#define PPU_GENERAL_OP_QUERY_FD 15

union ppu_general_out {
	struct {
		__u32 share_handle[4];
		__u32 ppuid;
		__u32 pad;
	} query_fd;
};

struct ppu_general_in {
	__u32 op;
	__u32 ppuid;
	__u32 migid;
	__u32 pad;
	union {
		struct {
			__s32 fd;
			__u32 pad;
		} query_fd;
	};
};

union ppu_general_op_args {
	struct ppu_general_in in;
	union ppu_general_out out;
};

#define PPU_IOC_GENERAL_OP _IOWR('A', 0x13, union ppu_general_op_args)
#define PPU_IOC_CRIU_OP	   _IOWR('A', 0x1c, union ppu_criu_args)

#if defined(__cplusplus)
}
#endif

#endif /* __PPU_IOCTL_H__ */
