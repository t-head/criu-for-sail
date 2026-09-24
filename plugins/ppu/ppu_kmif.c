#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "ppu_kmif.h"
#include "ppu_plugin_util.h"

static int ppu_ioctl(int fd, uint64_t request, void *arg)
{
	int ret = 0;

	do {
		ret = ioctl(fd, request, arg);
	} while ((ret == -1) && (errno == EINTR || errno == EAGAIN));

	return ret;
}

int ppu_support_criu(void)
{
	union ppu_criu_args args = {};
	int ret = 0;
	int fd = 0;

	fd = open(PPU_CHAR_NODE_NAME, O_RDWR);
	if (fd < 0) {
		ppu_err("Failed to open %s", PPU_CHAR_NODE_NAME);
		return -1;
	}

	args.in.op = PPU_CRIU_GET_VERSION;
	if (ppu_ioctl(fd, PPU_IOC_CRIU_OP, &args) != 0) {
		ret = -1;
		goto exit;
	} else {
		ppu_info("Criu version %d.%02d", args.out.version.major, args.out.version.minor);
	}

exit:
	close(fd);

	return ret;
}

int ppu_query_process_info(int fd, struct ppu_criu_process_info *info)
{
	union ppu_criu_args args = {};

	args.in.op = PPU_CRIU_PROCESS_INFO;

	if (ppu_ioctl(fd, PPU_IOC_CRIU_OP, &args) != 0)
		return -1;

	*info = args.out.process_info;

	return 0;
}

int ppu_process_checkpoint(int fd, struct ppu_criu_process_info *info, uint64_t device_ptr,
			   uint64_t queue_ptr, uint64_t bo_ptr, uint64_t prb_ptr,
			   uint64_t qw_ptr, uint64_t esr_ptr, uint64_t mmio_ptr,
			   uint64_t priv_ptr)
{
	union ppu_criu_args args = {};

	args.in.op = PPU_CRIU_CHECKPOINT;
	args.in.checkpoint.n_dev = info->n_dev;
	args.in.checkpoint.n_bo = info->n_bo;
	args.in.checkpoint.n_prb = info->n_prb;
	args.in.checkpoint.n_queue = info->n_queue;
	args.in.checkpoint.n_qw = info->n_qw;
	args.in.checkpoint.n_esr = info->n_esr;
	args.in.checkpoint.n_mmio = info->n_mmio;
	args.in.checkpoint.priv_size = info->priv_size;
	args.in.checkpoint.device_ptr = device_ptr;
	args.in.checkpoint.queue_ptr = queue_ptr;
	args.in.checkpoint.bo_ptr = bo_ptr;
	args.in.checkpoint.prb_ptr = prb_ptr;
	args.in.checkpoint.qw_ptr = qw_ptr;
	args.in.checkpoint.esr_ptr = esr_ptr;
	args.in.checkpoint.mmio_ptr = mmio_ptr;
	args.in.checkpoint.priv_ptr = priv_ptr;

	if (ppu_ioctl(fd, PPU_IOC_CRIU_OP, &args) != 0)
		return -1;

	return 0;
}

int ppu_read_bo_content(int fd, uint64_t va, uint64_t size, void *buff)
{
	uint64_t len = 0;
	ssize_t ret = 0;

	while (len < size) {
		if ((ret = pread(fd, buff + len, size - len, va + len)) <= 0)
			return -1;

		len += ret;
	}

	return 0;
}

int ppu_write_bo_content(int fd, uint64_t va, uint64_t size, void *buff)
{
	uint64_t len = 0;
	ssize_t ret = 0;

	while (len < size) {
		if ((ret = pwrite(fd, buff + len, size - len, va + len)) <= 0)
			return -1;

		len += ret;
	}

	return 0;
}

int ppu_restore_process(int fd, struct ppu_criu_restore_info *info)
{
	int ret = 0;
	int ioctl_errno = 0;
	union ppu_criu_args args = {};
	int64_t timeout = 60000;

	args.in.op = PPU_CRIU_RESTORE;

	args.in.restore.n_dev = info->n_dev;
	args.in.restore.n_bo = info->n_bo;
	args.in.restore.n_prb = info->n_prb;
	args.in.restore.n_queue = info->n_queue;
	args.in.restore.n_qw = info->n_qw;
	args.in.restore.n_esr = info->n_esr;
	args.in.restore.n_mmio = info->n_mmio;
	args.in.restore.device_ptr = info->devices;
	args.in.restore.queue_ptr = info->queues;
	args.in.restore.bo_ptr = info->bos;
	args.in.restore.prb_ptr = info->prbs;
	args.in.restore.qw_ptr = info->qw;
	args.in.restore.esr_ptr = info->esr;
	args.in.restore.mmio_ptr = info->mmio;
	args.in.restore.priv_ptr = info->priv_ptr;
	args.in.restore.priv_size = info->priv_size;

	do {
		ret = ioctl(fd, PPU_IOC_CRIU_OP, &args);
		ioctl_errno = errno;

		if (ret == -1 && ioctl_errno == EAGAIN) {
			args.in.restore.n_dev = 0;
			args.in.restore.n_queue = 0;
			args.in.restore.n_qw = 0;
			args.in.restore.n_esr = 0;
			args.in.restore.n_mmio = 0;

			ppu_sleep(1);
			timeout--;
		}
	} while ((ret == -1) && (ioctl_errno == EINTR || ioctl_errno == EAGAIN) && timeout > 0);

	return ret;
}

int ppu_restore_process_late(int fd, uint32_t pid)
{
	union ppu_criu_args args = {};

	args.in.op = PPU_CRIU_RESTORE_LATE;
	args.in.restore_late.pid = pid;

	if (ppu_ioctl(fd, PPU_IOC_CRIU_OP, &args) != 0)
		return -1;

	return 0;
}

int ppu_resume_process(int fd, uint32_t pid)
{
	union ppu_criu_args args = {};

	args.in.op = PPU_CRIU_RESUME;
	args.in.resume.pid = pid;

	if (ppu_ioctl(fd, PPU_IOC_CRIU_OP, &args) != 0)
		return -1;

	return 0;
}

int ppu_pause_devices(int fd, uint32_t pid)
{
	union ppu_criu_args args = {};

	args.in.op = PPU_CRIU_PAUSE_DEVICES;
	args.in.pause_devices.pid = pid;

	if (ppu_ioctl(fd, PPU_IOC_CRIU_OP, &args) != 0)
		return -1;

	return 0;
}

int ppu_get_share_objects_handle_from_fd(int fd, int share_fd, uint32_t *handle)
{
	union ppu_general_op_args args = {};

	args.in.op = PPU_GENERAL_OP_QUERY_FD;
	args.in.query_fd.fd = share_fd;

	if (ppu_ioctl(fd, PPU_IOC_GENERAL_OP, &args) != 0)
		return -1;

	*handle = args.out.query_fd.share_handle[0];

	return 0;
}

int ppu_get_share_objects_fd_from_handle(int fd, uint32_t handle, int *share_fd)
{
	union ppu_criu_args args = {};
	int ret = 0, ioctl_errno = 0;
	uint64_t timeout = 60000;

	args.in.op = PPU_CRIU_CREATE_SHARE_FD_FROM_HANDLE;
	args.in.create_share_fd.handle = handle;

	do {
		ret = ioctl(fd, PPU_IOC_CRIU_OP, &args);
		ioctl_errno = errno;

		if (ret == -1 && ioctl_errno == EAGAIN) {
			ppu_sleep(1);
			timeout--;
		}
	} while ((ret == -1) && (ioctl_errno == EINTR || ioctl_errno == EAGAIN) && timeout > 0);

	if (!ret)
		*share_fd = args.out.create_share_fd.fd;

	return ret;
}

int ppu_restore_ctx_create(int fd)
{
	union ppu_criu_args args = {};

	args.in.op = PPU_CRIU_RESTORE_CTX_CREATE;

	if (ppu_ioctl(fd, PPU_IOC_CRIU_OP, &args) != 0)
		return -1;

	return 0;
}
