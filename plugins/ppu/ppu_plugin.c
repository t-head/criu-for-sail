#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include <linux/limits.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>

#include "ppu_plugin_util.h"
#include "ppu_kmif.h"
#include "criu-ppu.pb-c.h"

#include "common/list.h"
#include "criu-log.h"
#include "plugin.h"
#include "util.h"
#include "pid.h"
#include "proc_parse.h"
#include "seize.h"
#include "files.h"

#define IPC_CRIU_CMD_CHECKPOINT_MEMORY 1
#define IPC_CRIU_CMD_RESTORE_MEMORY    2

#define IPC_ACK_SUCCESS 1
#define IPC_ACK_FAIL	2

uint32_t g_ipc_timeout = 300000; /* 300s */

static LIST_HEAD(vma_list);
struct vma_metadata {
	struct list_head list;
	uint64_t old_pgoff;
	uint64_t new_pgoff;
	uint64_t addr;
};

static LIST_HEAD(dev_list);
struct dev_entry {
	struct list_head list;
	uint32_t id;
	char name[PPU_NODE_NAME_LEN];
};

static int g_ppu_char_fd = -1;
static int g_char_node_id = 0;
static int g_node_count = 0;

/* Restore from ppu idx*/
static uint32_t g_ppu_idx = 0;

static void free_dev_list(struct list_head *head)
{
	struct dev_entry *ent = NULL;
	struct dev_entry *tmp = NULL;

	list_for_each_entry_safe(ent, tmp, head, list) {
		list_del(&ent->list);
		xfree(ent);
	}
}

static int interrupt_restore_thread(int restore_tid, k_rtsigset_t *restore_sigset)
{
	/* Since we resumed a thread that CRIU previously already froze we need to
	 * INTERRUPT it once again, task was already SEIZE'd so we don't need to do
	 * a compel_interrupt_task()
	 */
	if (ptrace(PTRACE_INTERRUPT, restore_tid, NULL, 0)) {
		ppu_err(
			"Could not interrupt cuda restore tid %d after checkpoint, process may be in strange "
			"state. %s",
			restore_tid, strerror(errno));
		return -1;
	}

	struct proc_status_creds creds = {};
	if (compel_wait_task(restore_tid, -1, parse_pid_status, NULL, &creds.s, NULL) !=
	    COMPEL_TASK_ALIVE) {
		ppu_err("compel_wait_task failed after interrupt");
		return -1;
	}

	if (ptrace(PTRACE_SETOPTIONS, restore_tid, NULL,
		   PTRACE_O_SUSPEND_SECCOMP | PTRACE_O_TRACESYSGOOD)) {
		ppu_err("Failed to set ptrace options on interrupt for restore tid %d. %s",
			restore_tid, strerror(errno));
		return -1;
	}

	if (ptrace(PTRACE_SETSIGMASK, restore_tid, sizeof(*restore_sigset), restore_sigset)) {
		ppu_err("Unable to restore original sigmask to restore tid %d. %s", restore_tid,
			strerror(errno));
		return -1;
	}

	return 0;
}

static int resume_restore_thread(int restore_tid, k_rtsigset_t *save_sigset)
{
	k_rtsigset_t block = {};

	if (ptrace(PTRACE_GETSIGMASK, restore_tid, sizeof(*save_sigset), save_sigset)) {
		ppu_err("Failed to get current sigmask for restore tid %d. %s", restore_tid,
			strerror(errno));
		return -1;
	}

	ksigfillset(&block);
	ksigdelset(&block, SIGTRAP);

	if (ptrace(PTRACE_SETSIGMASK, restore_tid, sizeof(block), &block)) {
		ppu_err("Failed to block signals on restore tid %d. %s", restore_tid, strerror(errno));
		return -1;
	}

	// Clear out PTRACE_O_SUSPEND_SECCOMP when we resume the restore thread
	if (ptrace(PTRACE_SETOPTIONS, restore_tid, NULL, 0)) {
		ppu_err("Could not clear ptrace options on restore tid %d. %s", restore_tid,
			strerror(errno));
		return -1;
	}

	if (ptrace(PTRACE_CONT, restore_tid, NULL, 0)) {
		ppu_err("Could not resume cuda restore tid %d. %s", restore_tid, strerror(errno));
		return -1;
	}

	return 0;
}

/**********************************************************************************************************************/
/***************************************** ppu_plugin_init and ppu_plugin_fini ****************************************/
/**********************************************************************************************************************/

int ppu_plugin_init(int stage)
{
	int ret = 0;
	uint32_t log_level = 0;

	ppu_info("Initialized:  %s (PPU)", CR_PLUGIN_DESC.name);

	ret = ppu_getenv_value("PPU_CRIU_LOG_LEVEL", &log_level);
	if (!ret) {
		ppu_set_log_level(log_level);
	}

	ret = ppu_getenv_value("PPU_CRIU_IPC_TIMEOUT", &g_ipc_timeout);
	if (!ret)
		ppu_info("Set IPC timeout to %.3f s", g_ipc_timeout / 1000.0);

	if (stage == CR_PLUGIN_STAGE__RESTORE) {
		/* Ignore error and use 0 index if failed*/
		ppu_getenv_value("PPU_CRIU_RESTORE_START_INDEX", &g_ppu_idx);
		ppu_info("Set restore from ppu index %d", g_ppu_idx);
	}

	return 0;
}

void ppu_plugin_fini(int stage, int ret)
{
	ppu_info("Finished  %s (PPU)", CR_PLUGIN_DESC.name);
}

CR_PLUGIN_REGISTER("ppu_plugin", ppu_plugin_init, ppu_plugin_fini)

/**********************************************************************************************************************/
/******************************************** ppu_plugin_pause_devices ************************************************/
/**********************************************************************************************************************/

int ppu_plugin_pause_devices(int pid)
{
	int ret = 0;
	int fd = 0;

	ppu_info("Pause target pid = %d", pid);

	fd = open(PPU_CHAR_NODE_NAME, O_RDWR);
	if (fd < 0) {
		ppu_err("Failed to open %s", PPU_CHAR_NODE_NAME);
		return -1;
	}

	ret = ppu_pause_devices(fd, pid);

	close(fd);

	return ret;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__PAUSE_DEVICES, ppu_plugin_pause_devices)

/**********************************************************************************************************************/
/******************************************** ppu_plugin_checkpoint_devices *******************************************/
/**********************************************************************************************************************/

int ppu_plugin_checkpoint_devices(int pid)
{
	int ret = 0, int_ret = 0;
	int restore_tid = 0;
	int read_fd, write_fd;
	k_rtsigset_t save_sigset;
	const uint64_t code = IPC_CRIU_CMD_CHECKPOINT_MEMORY;
	uint64_t ack;

	ppu_info("Checkpoint target pid = %d", pid);

	ret = ppu_get_criu_restore_tid(pid, &restore_tid, &read_fd, &write_fd);
	if (ret) {
		ppu_info("Failed to get restore process %d restore_tid/read_fd/write_fd", pid);
		return 0;
	}

	ret = resume_restore_thread(restore_tid, &save_sigset);
	if (ret) {
		goto out_close_translate_fd;
	}

	write(write_fd, &code, sizeof(code));
	ret = read_with_timeout(read_fd, &ack, sizeof(ack), g_ipc_timeout);
	if (ret == sizeof(ack) && ack == IPC_ACK_SUCCESS) {
		ret = 0;
	} else {
		ret = -1;
		ppu_err("Failed to get restore process %d ack", pid);
	}

	int_ret = interrupt_restore_thread(restore_tid, &save_sigset);

out_close_translate_fd:
	close(read_fd);
	close(write_fd);

	return ret != 0 ? ret : int_ret;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__CHECKPOINT_DEVICES, ppu_plugin_checkpoint_devices)

/**********************************************************************************************************************/
/****************************************** ppu_plugin_handle_device_vma **********************************************/
/**********************************************************************************************************************/

int ppu_plugin_handle_device_vma(int fd, const struct stat *st_buf)
{
	struct stat st = {};
	int ret = 0;

	ret = stat(PPU_CHAR_NODE_NAME, &st);
	if (ret == -1) {
		ppu_err("Stat error for %s", PPU_CHAR_NODE_NAME);
		goto out;
	}

	if (major(st_buf->st_rdev) == major(st.st_rdev)) {
		ppu_dbg("Known non-regular mapping, %s -> OK", PPU_CHAR_NODE_NAME);
	} else {
		ppu_dbg("Can't handle VMAs of input device");
		ret = -ENOTSUP;
	}

out:
	return ret;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__HANDLE_DEVICE_VMA, ppu_plugin_handle_device_vma)

/**********************************************************************************************************************/
/********************************************** ppu_plugin_dump_file **************************************************/
/**********************************************************************************************************************/

static void free_pb(ProtoCriu *pb)
{
	int i, j;

	for (i = 0; i < pb->n_bos; i++) {
		if (pb->bos[i]) {
			for (j = 0; j < pb->bos[i]->n_prb_ranges; j++) {
				xfree(pb->bos[i]->prb_ranges[j]);
			}
			xfree(pb->bos[i]->prb_ranges);
			xfree(pb->bos[i]);
		}
	}
	xfree(pb->bos);

	for (i = 0; i < pb->n_devices; i++) {
		xfree(pb->devices[i]);
	}
	xfree(pb->devices);

	for (i = 0; i < pb->n_queues; i++) {
		xfree(pb->queues[i]);
	}
	xfree(pb->queues);

	for (i = 0; i < pb->n_qw; i++) {
		xfree(pb->qw[i]);
	}
	xfree(pb->qw);

	for (i = 0; i < pb->n_esr; i++) {
		xfree(pb->esr[i]);
	}
	xfree(pb->esr);

	for (i = 0; i < pb->n_mmio; i++) {
		xfree(pb->mmio[i]);
	}
	xfree(pb->mmio);
}

static int allocate_proto_devices(ProtoCriu *pb, uint32_t num_device)
{
	int i;

	pb->devices = xmalloc(sizeof(ProtoDevice *) * num_device);
	if (!pb->devices) {
		ppu_err("Failed to allocate device protobuf pointer array");
		return -ENOMEM;
	}

	for (i = 0; i < num_device; i++) {
		ProtoDevice *pb_device = xzalloc(sizeof(*pb_device));
		if (!pb_device) {
			ppu_err("Failed to allocate device protobuf pointer");
			return -ENOMEM;
		}

		proto_device__init(pb_device);
		pb->devices[i] = pb_device;
		pb->n_devices++;
	}

	return 0;
}

static int save_device(int fd, uint32_t num_device, ppu_criu_device_info *device, ProtoCriu *pb)
{
	int ret = 0;
	int i;

	ppu_info("Save %d devices start", num_device);

	pb->num_device = num_device;

	ret = allocate_proto_devices(pb, num_device);
	if (ret) {
		ppu_err("Allocate proto device failed");
		goto out;
	}

	for (i = 0; i < num_device; i++) {
		ProtoDevice *pb_device = pb->devices[i];

		pb_device->dev_index = device[i].dev_index;
		pb_device->actual_ppuid = device[i].actual_ppuid;

		ppu_dbg("Device[%d] dev_index %d ppuid %d", i, pb_device->dev_index, pb_device->actual_ppuid);
	}

out:
	ppu_info("Save %d devices %s", num_device, ret ? "failed" : "success");
	return ret;
}

static int allocate_proto_bos(ProtoCriu *pb, uint32_t num_bo)
{
	int i;

	pb->bos = xmalloc(sizeof(ProtoBo *) * num_bo);
	if (!pb->bos) {
		ppu_err("Failed to allocate bo protobuf pointer array");
		return -ENOMEM;
	}

	for (i = 0; i < num_bo; i++) {
		ProtoBo *pb_bo = xzalloc(sizeof(*pb_bo));

		if (!pb_bo) {
			ppu_err("Failed to allocate bo protobuf pointer");
			return -ENOMEM;
		}
		proto_bo__init(pb_bo);
		pb->bos[i] = pb_bo;
		pb->n_bos++;
	}

	return 0;
}

static int allocate_proto_prb(ProtoBo *pb_bo)
{
	int i;

	pb_bo->prb_ranges = xmalloc(sizeof(ProtoBoPrbRange *) * pb_bo->n_prb_ranges);
	if (!pb_bo->prb_ranges) {
		ppu_err("Failed to allocate bo protobuf pointer array");
		return -ENOMEM;
	}

	for (i = 0; i < pb_bo->n_prb_ranges; i++) {
		ProtoBoPrbRange *pb_bo_prb = xzalloc(sizeof(*pb_bo_prb));

		if (!pb_bo_prb) {
			ppu_err("Failed to allocate bo protobuf pointer");
			return -ENOMEM;
		}
		proto_bo_prb_range__init(pb_bo_prb);
		pb_bo->prb_ranges[i] = pb_bo_prb;
	}

	return 0;
}

static int allocate_proto_queues(ProtoCriu *pb, uint32_t num_queue)
{
	int i;

	pb->queues = xmalloc(sizeof(ProtoQueue *) * num_queue);
	if (!pb->queues) {
		ppu_err("Failed to allocate queue protobuf pointer array");
		return -ENOMEM;
	}

	for (i = 0; i < num_queue; i++) {
		ProtoQueue *pb_queue = xzalloc(sizeof(*pb_queue));

		if (!pb_queue) {
			ppu_err("Failed to allocate queue protobuf pointer");
			return -ENOMEM;
		}
		proto_queue__init(pb_queue);
		pb->queues[i] = pb_queue;
		pb->n_queues++;
	}

	return 0;
}

static int allocate_proto_qw(ProtoCriu *pb, uint32_t num_qw)
{
	int i;

	pb->qw = xmalloc(sizeof(ProtoQw *) * num_qw);
	if (!pb->qw) {
		ppu_err("Failed to allocate qw protobuf pointer array");
		return -ENOMEM;
	}

	for (i = 0; i < num_qw; i++) {
		ProtoQw *pb_qw = xzalloc(sizeof(*pb_qw));

		if (!pb_qw) {
			ppu_err("Failed to allocate qw protobuf pointer");
			return -ENOMEM;
		}
		proto_qw__init(pb_qw);
		pb->qw[i] = pb_qw;
		pb->n_qw++;
	}

	return 0;
}

static int allocate_proto_esr(ProtoCriu *pb, uint32_t num_esr)
{
	int i;

	pb->esr = xmalloc(sizeof(ProtoEsr *) * num_esr);
	if (!pb->esr) {
		ppu_err("Failed to allocate esr protobuf pointer array");
		return -ENOMEM;
	}

	for (i = 0; i < num_esr; i++) {
		ProtoEsr *pb_esr = xzalloc(sizeof(*pb_esr));

		if (!pb_esr) {
			ppu_err("Failed to allocate esr protobuf pointer");
			return -ENOMEM;
		}
		proto_esr__init(pb_esr);
		pb->esr[i] = pb_esr;
		pb->n_esr++;
	}

	return 0;
}

static int allocate_proto_mmio(ProtoCriu *pb, uint32_t num_mmio)
{
	int i;

	pb->mmio = xmalloc(sizeof(ProtoMmio *) * num_mmio);
	if (!pb->mmio) {
		ppu_err("Failed to allocate mmio protobuf pointer array");
		return -ENOMEM;
	}

	for (i = 0; i < num_mmio; i++) {
		ProtoMmio *pb_mmio = xzalloc(sizeof(*pb_mmio));

		if (!pb_mmio) {
			ppu_err("Failed to allocate mmio protobuf pointer");
			return -ENOMEM;
		}
		proto_mmio__init(pb_mmio);
		pb->mmio[i] = pb_mmio;
		pb->n_mmio++;
	}

	return 0;
}

static int save_bo(int fd, uint32_t num_bo, ppu_criu_bo_info *bo, uint32_t num_prb, struct ppu_criu_bo_prb_range *prb, ProtoCriu *pb)
{
	int ret = 0;
	int i;

	ppu_info("Save %d bos", num_bo);

	pb->num_bo = num_bo;
	pb->num_prb = num_prb;
	ret = allocate_proto_bos(pb, num_bo);
	if (ret) {
		ppu_err("Allocate proto bo failed");
		goto out;
	}

	for (i = 0; i < num_bo; i++) {
		ProtoBo *pb_bo = pb->bos[i];

		pb_bo->addr = bo[i].addr;
		pb_bo->size = bo[i].size;
		pb_bo->dev_index = bo[i].dev_index;
		pb_bo->offset = bo[i].offset;
		pb_bo->sema_addr = bo[i].sema_addr;
		pb_bo->sema_offset = bo[i].sema_offset;
		pb_bo->rst_flags = bo[i].rst_flags;
		pb_bo->type = bo[i].type;
		pb_bo->n_prb_ranges = bo[i].n_prb;
		if (pb_bo->n_prb_ranges) {
			int piece;

			ret = allocate_proto_prb(pb_bo);
			if (ret) {
				ppu_err("Allocate proto prb failed");
				goto out;
			}

			for (piece = 0; piece < pb_bo->n_prb_ranges; piece++) {
				ProtoBoPrbRange *pb_bo_prb = pb_bo->prb_ranges[piece];

				pb_bo_prb->type = prb[piece].type;
				pb_bo_prb->offset = prb[piece].offset;
				pb_bo_prb->alias = prb[piece].alias;
				pb_bo_prb->size = prb[piece].size;
			}
		}

		ppu_dbg("Bo[%03d] addr 0x%010lx size 0x%010lx dev_index 0x%x offset 0x%lx rst_flags 0x%x sema addr 0x%lx sema offset 0x%lx ",
			i, pb_bo->addr, pb_bo->size, pb_bo->dev_index, pb_bo->offset, pb_bo->rst_flags, pb_bo->sema_addr, pb_bo->sema_offset);
	}

out:
	ppu_info("Save %d bos %s", num_bo, ret ? "failed" : "success");
	return ret;
}

static int save_queue(int fd, uint32_t num_queue, ppu_criu_queue_info *queue, ProtoCriu *pb)
{
	int ret = 0;
	int i;

	ppu_info("Save %d queues", num_queue);

	pb->num_queues = num_queue;

	ret = allocate_proto_queues(pb, num_queue);
	if (ret) {
		ppu_err("Allocate proto queues failed");
		goto out;
	}

	for (i = 0; i < num_queue; i++) {
		ProtoQueue *pb_queue = pb->queues[i];

		pb_queue->dev_index = queue[i].dev_index;
		pb_queue->addr = queue[i].addr;
		pb_queue->size = queue[i].size;
		pb_queue->offset = queue[i].offset;

		ppu_dbg("Queue[%03d] addr 0x%lx size 0x%lx offset 0x%lx", i, pb_queue->addr, pb_queue->size, pb_queue->offset);
	}

out:
	ppu_info("Save %d queues %s", num_queue, ret ? "failed" : "success");
	return ret;
}

static int save_qw(int fd, uint32_t num_qw, ppu_criu_util_info *qw, ProtoCriu *pb)
{
	int ret = 0;
	int i;

	ppu_info("Save %d qw", num_qw);

	pb->num_qw = num_qw;

	ret = allocate_proto_qw(pb, num_qw);
	if (ret) {
		ppu_err("Allocate proto qw failed");
		goto out;
	}

	for (i = 0; i < num_qw; i++) {
		ProtoQw *pb_qw = pb->qw[i];

		pb_qw->dev_index = qw[i].dev_index;
		pb_qw->addr = qw[i].addr;
		pb_qw->size = qw[i].size;
		pb_qw->offset = qw[i].offset;

		ppu_dbg("Qw[%03d] addr 0x%lx size 0x%lx offset 0x%lx", i, pb_qw->addr, pb_qw->size, pb_qw->offset);
	}

out:
	ppu_info("Save %d qw %s", num_qw, ret ? "failed" : "success");
	return ret;
}

static int save_esr(int fd, uint32_t num_esr, ppu_criu_util_info *esr, ProtoCriu *pb)
{
	int ret = 0;
	int i;

	ppu_info("Save %d esr", num_esr);

	pb->num_esr = num_esr;

	ret = allocate_proto_esr(pb, num_esr);
	if (ret) {
		ppu_err("Allocate proto esr failed");
		goto out;
	}

	for (i = 0; i < num_esr; i++) {
		ProtoEsr *pb_esr = pb->esr[i];

		pb_esr->dev_index = esr[i].dev_index;
		pb_esr->addr = esr[i].addr;
		pb_esr->size = esr[i].size;
		pb_esr->offset = esr[i].offset;

		ppu_dbg("Esr[%03d] addr 0x%lx size 0x%lx offset 0x%lx", i, pb_esr->addr, pb_esr->size, pb_esr->offset);
	}

out:
	ppu_info("Save %d esr %s", num_esr, ret ? "failed" : "success");
	return ret;
}

static int save_mmio(int fd, uint32_t num_mmio, ppu_criu_util_info *mmio, ProtoCriu *pb)
{
	int ret = 0;
	int i;

	ppu_info("Save %d mmio", num_mmio);

	pb->num_mmio = num_mmio;

	ret = allocate_proto_mmio(pb, num_mmio);
	if (ret) {
		ppu_err("Allocate proto mmio failed");
		goto out;
	}

	for (i = 0; i < num_mmio; i++) {
		ProtoMmio *pb_mmio = pb->mmio[i];

		pb_mmio->dev_index = mmio[i].dev_index;
		pb_mmio->addr = mmio[i].addr;
		pb_mmio->size = mmio[i].size;
		pb_mmio->offset = mmio[i].offset;

		ppu_dbg("Mmio[%03d] addr 0x%lx size 0x%lx offset 0x%lx", i, pb_mmio->addr, pb_mmio->size, pb_mmio->offset);
	}

out:
	ppu_info("Save %d mmio %s", num_mmio, ret ? "failed" : "success");
	return ret;
}

static int ppu_inc_node_count()
{
	int fd = -1;
	FILE *fp = NULL;
	char file_path[PATH_MAX] = {};

	snprintf(file_path, sizeof(file_path), PPU_NODE_COUNT_FILE, g_char_node_id);

	fd = openat(criu_get_image_dir(), file_path, O_WRONLY | O_CREAT, 0600);
	if (fd < 0) {
		return -1;
	}

	fp = fdopen(fd, "w");
	if (!fp) {
		close(fd);
		return -1;
	}

	g_node_count++;

	if (fwrite(&g_node_count, sizeof(int), 1, fp) != 1) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return 0;
}

static int ppu_setup_node_count()
{
	int fd = -1;
	FILE *fp = NULL;
	char file_path[PATH_MAX] = {};

	snprintf(file_path, sizeof(file_path), PPU_NODE_COUNT_FILE, g_char_node_id);

	fd = openat(criu_get_image_dir(), file_path, O_RDONLY);
	if (fd < 0) {
		return -1;
	}

	fp = fdopen(fd, "r");
	if (!fp) {
		close(fd);
		return -1;
	}

	if (fread(&g_node_count, sizeof(int), 1, fp) != 1) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return 0;
}

static int ppu_dump_device_node(int fd, int id, struct stat *st)
{
	int ret = 0;
	ProtoNode pb_node = PROTO_NODE__INIT;
	char name[PPU_NODE_NAME_LEN] = {};
	char path[PATH_MAX] = {};
	int size = 0;
	uint32_t len = 0;
	void *buf = NULL;
	bool inc_node_count = true;

	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	len = readlink(path, name, sizeof(name) - 1);
	if (len == -1) {
		ppu_err("Readlink %s fail.", path);
		return -1;
	}
	name[len] = '\0';
	ppu_info("Dumping node %s", name);

	if (strstr(name, PPU_NODE_NAME_PREFIX) == NULL) {
		ppu_err("Node %s(fd %d) is not an ppu device node", name, fd);
		return -1;
	}

	if (strcmp(name, PPU_CHAR_NODE_NAME) == 0) {
		g_char_node_id = id;
		g_node_count = 0;
	} else if (strncmp(name, ANON_INODE_PREFIX, strlen(ANON_INODE_PREFIX)) == 0) {
		ppu_err("fd%d -> %s is not supported yet", fd, name);
		return -1;
	}

	len = strlen(name) + 1;
	pb_node.name = xzalloc(len);
	if (!pb_node.name) {
		ret = -ENOMEM;
		goto err_pb_alloc;
	}
	strncpy(pb_node.name, name, len);

	size = proto_node__get_packed_size(&pb_node);
	buf = xmalloc(size);
	if (!buf) {
		ret = -ENOMEM;
		goto err_buf_alloc;
	}

	proto_node__pack(&pb_node, buf);
	snprintf(path, sizeof(path), PPU_IMG_NODE_FILE, id);
	ret = write_img_file(path, buf, size);
	if (ret) {
		goto err_write_img_file;
	}

	if (inc_node_count)
		ret = ppu_inc_node_count();

err_write_img_file:
	xfree(buf);
err_buf_alloc:
	xfree(pb_node.name);
err_pb_alloc:
	return ret;
}

/* Assumption: Dump the ppu node first,
 *             because we need to assign a value to g_char_node_id. */
int ppu_plugin_dump_file(int fd, int id)
{
	int ret = 0;
	uint64_t placement = 0;
	void *priv_data = NULL;
	struct ppu_criu_process_info info = {};
	ppu_criu_device_info *device = NULL;
	ppu_criu_bo_info *bo = NULL;
	struct ppu_criu_bo_prb_range *prb = NULL;
	ppu_criu_queue_info *queue = NULL;
	ppu_criu_util_info *qw = NULL;
	ppu_criu_util_info *esr = NULL;
	ppu_criu_util_info *mmio = NULL;
	ProtoCriu *pb = NULL;
	uint64_t len = 0;
	uint8_t *buf = NULL;
	struct stat st_fd = {};
	struct stat st_ch = {};
	char pb_path[PATH_MAX] = {};

	ppu_info("fd %d/id %d", fd, id);

	ret = fstat(fd, &st_fd);
	if (ret) {
		ppu_err("Fstat fd %d failed", fd);
		goto out;
	}

	ret = stat(PPU_CHAR_NODE_NAME, &st_ch);
	if (ret) {
		ppu_err("Fstat %s failed", PPU_CHAR_NODE_NAME);
		goto out;
	}

	if (ppu_support_criu() != 0) {
		ppu_err("Kernel not supporte criu");
		ret = -ENOTSUP;
		goto out;
	}

	ret = ppu_dump_device_node(fd, id, &st_fd);
	if (ret) {
		ppu_err("Failed to dump fd %d", fd);
		goto out;
	}

	if ((major(st_fd.st_rdev) != major(st_ch.st_rdev)) || (minor(st_fd.st_rdev) != 0)) {
		return ret;
	}

	ret = ppu_query_process_info(fd, &info);
	if (ret) {
		ppu_err("Failed to query process info");
		goto out;
	}

	ppu_info("Pid %d, device: %d, bo: %d, queue: %d, qw: %d, esr: %d, priv_data: 0x%llx",
		 info.pid, info.n_dev, info.n_bo, info.n_queue, info.n_qw, info.n_esr, info.priv_size);

	placement = (uintptr_t)xzalloc(info.n_dev * sizeof(*device) +  /* device */
				       info.n_queue * sizeof(*queue) + /* queue */
				       info.n_bo * sizeof(*bo) +       /* bos */
				       info.n_prb * sizeof(*prb) +     /* bo prb ranges */
				       info.n_qw * sizeof(*qw) +       /* qw pool */
				       info.n_esr * sizeof(*esr) +     /* esr */
				       info.n_mmio * sizeof(*mmio) +   /* mmio */
				       info.priv_size + sizeof(*pb));
	if (!placement) {
		ppu_err("Failed to alloc memory");
		ret = -ENOMEM;
		goto out;
	}

	device = (ppu_criu_device_info *)placement;
	queue = (ppu_criu_queue_info *)&device[info.n_dev];
	bo = (ppu_criu_bo_info *)&queue[info.n_queue];
	prb = (struct ppu_criu_bo_prb_range *)&bo[info.n_bo];
	qw = (ppu_criu_util_info *)&prb[info.n_prb];
	esr = (ppu_criu_util_info *)&qw[info.n_qw];
	mmio = (ppu_criu_util_info *)&esr[info.n_esr];
	pb = (ProtoCriu *)&mmio[info.n_mmio];
	priv_data = (void *)(pb + 1);

	ret = ppu_process_checkpoint(fd, &info, (uint64_t)device, (uint64_t)queue, (uint64_t)bo,
				     (uint64_t)prb, (uint64_t)qw, (uint64_t)esr, (uint64_t)mmio,
				     (uint64_t)priv_data);
	if (ret) {
		ppu_err("Failed to checkpoint process");
		goto err0;
	}

	proto_criu__init(pb);
	pb->pid = info.pid;

	ret = save_device(fd, info.n_dev, device, pb);
	if (ret) {
		ppu_err("Failed to save devices");
		goto err1;
	}

	ret = save_bo(fd, info.n_bo, bo, info.n_prb, prb, pb);
	if (ret) {
		ppu_err("Failed to save bos");
		goto err1;
	}

	ret = save_queue(fd, info.n_queue, queue, pb);
	if (ret) {
		ppu_err("Failed to save queues");
		goto err1;
	}

	ret = save_qw(fd, info.n_qw, qw, pb);
	if (ret) {
		ppu_err("Failed to save qw");
		goto err1;
	}

	ret = save_esr(fd, info.n_esr, esr, pb);
	if (ret) {
		ppu_err("Failed to save esr");
		goto err1;
	}

	ret = save_mmio(fd, info.n_mmio, mmio, pb);
	if (ret) {
		ppu_err("Failed to save mmio");
		goto err1;
	}

	pb->priv_data.data = priv_data;
	pb->priv_data.len = info.priv_size;

	/* Save protobuf data */
	len = proto_criu__get_packed_size(pb);
	buf = xmalloc(len);
	if (!buf) {
		ppu_err("Failed to allocate memory to store protobuf");
		ret = -ENOMEM;
		goto err1;
	}

	proto_criu__pack(pb, buf);

	snprintf(pb_path, sizeof(pb_path), PPU_IMG_FILE, g_char_node_id);
	ret = write_img_file(pb_path, buf, len);
	if (ret) {
		ppu_err("Failed to write protobuf to image file");
		goto err2;
	}

err2:
	xfree(buf);
err1:
	free_pb(pb);
err0:
	xfree((void *)placement);
out:
	return ret;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__DUMP_EXT_FILE, ppu_plugin_dump_file)

/**********************************************************************************************************************/
/******************************************* ppu_plugin_restore_file **************************************************/
/**********************************************************************************************************************/

static int restore_device(struct ppu_criu_restore_info *info, ProtoCriu *pb)
{
	int ret = 0;
	int i;
	ppu_criu_device_info *device = NULL;

	ppu_info("Restore %d devices", pb->num_device);

	info->n_dev = pb->num_device;

	device = xzalloc(pb->num_device * sizeof(*device));
	if (!device) {
		ret = -ENOMEM;
		goto out;
	}

	info->devices = (uint64_t)device;

	for (i = 0; i < pb->num_device; i++) {
		ProtoDevice *pb_device = pb->devices[i];

		device[i].dev_index = pb_device->dev_index;
		device[i].actual_ppuid = pb_device->actual_ppuid;

		ppu_dbg("Device[%d] dev_index %d ppuid %d", i, device[i].dev_index, device[i].actual_ppuid);
	}

out:
	ppu_info("Restore %d devices %s", pb->num_device, ret ? "failed" : "success");
	return ret;
}

static int restore_bo(struct ppu_criu_restore_info *info, ProtoCriu *pb)
{
	int ret = 0;
	int i, j;
	ppu_criu_bo_info *bos = NULL;
	struct ppu_criu_bo_prb_range *prbs = NULL;

	ppu_info("Restore %ld Bos, %d prbs", pb->num_bo, pb->num_prb);

	info->n_bo = pb->num_bo;
	info->n_prb = pb->num_prb;
	bos = xzalloc(pb->num_bo * sizeof(*bos) + pb->num_prb * sizeof(*prbs));
	if (!bos) {
		ret = -ENOMEM;
		goto out;
	}
	prbs = (struct ppu_criu_bo_prb_range *)&bos[pb->num_bo];

	info->bos = (uint64_t)bos;
	info->prbs = (uint64_t)prbs;

	for (i = 0; i < pb->num_bo; i++) {
		ProtoBo *pb_bo = pb->bos[i];

		bos[i].addr = pb_bo->addr;
		bos[i].size = pb_bo->size;
		bos[i].dev_index = pb_bo->dev_index;
		bos[i].offset = pb_bo->offset;
		bos[i].sema_addr = pb_bo->sema_addr;
		bos[i].sema_offset = pb_bo->sema_offset;
		bos[i].rst_flags = pb_bo->rst_flags;
		bos[i].type = pb_bo->type;
		bos[i].n_prb = pb_bo->n_prb_ranges;
		for (j = 0; j < pb_bo->n_prb_ranges; j++) {
			prbs[j].type = pb_bo->prb_ranges[j]->type;
			prbs[j].offset = pb_bo->prb_ranges[j]->offset;
			prbs[j].alias = pb_bo->prb_ranges[j]->alias;
			prbs[j].size = pb_bo->prb_ranges[j]->size;
		}
		prbs += pb_bo->n_prb_ranges;

		ppu_dbg("Bo[%03d] addr 0x%010lx size 0x%010lx dev_index 0x%x offset 0x%lx rst_flags 0x%x sema addr 0x%lx sema offset 0x%lx type 0x%x n_prb %ld",
			i, pb_bo->addr, pb_bo->size, pb_bo->dev_index, pb_bo->offset, pb_bo->rst_flags, pb_bo->sema_addr, pb_bo->sema_offset, pb_bo->type, pb_bo->n_prb_ranges);
	}

out:
	ppu_info("Restore %ld Bos %s", pb->num_bo, ret ? "failed" : "success");
	return ret;
}

static int restore_queue(struct ppu_criu_restore_info *info, ProtoCriu *pb)
{
	int ret = 0;
	int i;
	ppu_criu_queue_info *queue = NULL;

	ppu_info("Restore %d Queues", pb->num_queues);

	info->n_queue = pb->num_queues;

	queue = xzalloc(pb->num_queues * sizeof(*queue));
	if (!queue) {
		ret = -ENOMEM;
		goto out;
	}

	info->queues = (uint64_t)queue;

	for (i = 0; i < pb->num_queues; i++) {
		ProtoQueue *pb_queue = pb->queues[i];

		queue[i].dev_index = pb_queue->dev_index;
		queue[i].addr = pb_queue->addr;
		queue[i].size = pb_queue->size;
		queue[i].offset = pb_queue->offset;

		ppu_dbg("Queue[%03d] addr 0x%lx size 0x%lx offset 0x%lx", i, pb_queue->addr, pb_queue->size, pb_queue->offset);
	}

out:
	ppu_info("Restore %d Queues %s", pb->num_queues, ret ? "failed" : "success");
	return ret;
}

static int restore_queues_vma_list(struct ppu_criu_restore_info *info, ProtoCriu *pb)
{
	int ret = 0;
	int i;
	ppu_criu_queue_info *queue = (ppu_criu_queue_info *)info->queues;
	struct vma_metadata *vma_md = NULL;

	ppu_info("Restore %d Queues vma list", pb->num_queues);

	for (i = 0; i < pb->num_queues; i++) {
		vma_md = xzalloc(sizeof(*vma_md));
		if (!vma_md) {
			ret = -ENOMEM;
			goto out;
		}

		vma_md->addr = queue[i].addr;
		vma_md->old_pgoff = queue[i].offset;
		vma_md->new_pgoff = queue[i].restored_offset;
		ppu_dbg("Add queue vma[%d] addr 0x%lx old_pgoff 0x%lx new_pgoff 0x%lx",
			i, vma_md->addr, vma_md->old_pgoff, vma_md->new_pgoff);

		/* transfer ownership */
		list_add_tail(&vma_md->list, &vma_list);
		vma_md = NULL;
	}

out:
	ppu_info("Restore %d Queues vma list %s", pb->num_queues, ret ? "failed" : "success");
	return ret;
}

static int restore_qw(struct ppu_criu_restore_info *info, ProtoCriu *pb)
{
	int ret = 0;
	int i;
	ppu_criu_util_info *qw = NULL;

	ppu_info("Restore %d qw", pb->num_qw);
	info->n_qw = pb->num_qw;

	qw = xzalloc(pb->num_qw * sizeof(*qw));
	if (!qw) {
		ret = -ENOMEM;
		goto out;
	}

	info->qw = (uint64_t)qw;

	for (i = 0; i < pb->num_qw; i++) {
		ProtoQw *pb_qw = pb->qw[i];

		qw[i].dev_index = pb_qw->dev_index;
		qw[i].addr = pb_qw->addr;
		qw[i].size = pb_qw->size;
		qw[i].offset = pb_qw->offset;

		ppu_dbg("Qw[%03d] addr 0x%lx size 0x%lx offset 0x%lx", i, pb_qw->addr, pb_qw->size, pb_qw->offset);
	}

out:
	ppu_info("Restore %d qw %s", pb->num_qw, ret ? "failed" : "success");
	return ret;
}

static int restore_qw_vma_list(struct ppu_criu_restore_info *info, ProtoCriu *pb)
{
	int ret = 0;
	int i;
	ppu_criu_util_info *qw = (ppu_criu_util_info *)info->qw;
	struct vma_metadata *vma_md = NULL;

	ppu_info("Restore %d qw vma list", pb->num_qw);

	for (i = 0; i < pb->num_qw; i++) {
		vma_md = xzalloc(sizeof(*vma_md));
		if (!vma_md) {
			ret = -ENOMEM;
			goto out;
		}

		vma_md->addr = qw[i].addr;
		vma_md->old_pgoff = qw[i].offset;
		vma_md->new_pgoff = qw[i].restored_offset;
		ppu_dbg("Add qw vma[%d] addr 0x%lx old_pgoff 0x%lx new_pgoff 0x%lx",
			i, vma_md->addr, vma_md->old_pgoff, vma_md->new_pgoff);

		/* transfer ownership */
		list_add_tail(&vma_md->list, &vma_list);
		vma_md = NULL;
	}

out:
	ppu_info("Restore %d qw vma list %s", pb->num_qw, ret ? "failed" : "success");
	return ret;
}

static int restore_esr(struct ppu_criu_restore_info *info, ProtoCriu *pb)
{
	int ret = 0;
	int i;
	ppu_criu_util_info *esr = NULL;

	ppu_info("Restore %d esr", pb->num_esr);
	info->n_esr = pb->num_esr;

	esr = xzalloc(pb->num_esr * sizeof(*esr));
	if (!esr) {
		ret = -ENOMEM;
		goto out;
	}

	info->esr = (uint64_t)esr;

	for (i = 0; i < pb->num_esr; i++) {
		ProtoEsr *pb_esr = pb->esr[i];

		esr[i].dev_index = pb_esr->dev_index;
		esr[i].addr = pb_esr->addr;
		esr[i].size = pb_esr->size;
		esr[i].offset = pb_esr->offset;

		ppu_dbg("Esr[%03d] addr 0x%lx size 0x%lx offset 0x%lx", i, pb_esr->addr, pb_esr->size, pb_esr->offset);
	}

out:
	ppu_info("Restore %d esr %s", pb->num_esr, ret ? "failed" : "success");
	return ret;
}

static int restore_esr_vma_list(struct ppu_criu_restore_info *info, ProtoCriu *pb)
{
	int ret = 0;
	int i;
	ppu_criu_util_info *esr = (ppu_criu_util_info *)info->esr;
	struct vma_metadata *vma_md = NULL;

	ppu_info("Restore %d esr vma list", pb->num_esr);

	for (i = 0; i < pb->num_esr; i++) {
		vma_md = xzalloc(sizeof(*vma_md));
		if (!vma_md) {
			ret = -ENOMEM;
			goto out;
		}

		vma_md->addr = esr[i].addr;
		vma_md->old_pgoff = esr[i].offset;
		vma_md->new_pgoff = esr[i].restored_offset;
		ppu_dbg("Add esr vma[%d] addr 0x%lx old_pgoff 0x%lx new_pgoff 0x%lx",
			i, vma_md->addr, vma_md->old_pgoff, vma_md->new_pgoff);

		/* transfer ownership */
		list_add_tail(&vma_md->list, &vma_list);
		vma_md = NULL;
	}

out:
	ppu_info("Restore %d esr vma list %s", pb->num_esr, ret ? "failed" : "success");
	return ret;
}

static int restore_mmio(struct ppu_criu_restore_info *info, ProtoCriu *pb)
{
	int ret = 0;
	int i;
	ppu_criu_util_info *mmio = NULL;

	ppu_info("Restore %d mmio", pb->num_mmio);
	info->n_mmio = pb->num_mmio;

	mmio = xzalloc(pb->num_mmio * sizeof(*mmio));
	if (!mmio) {
		ret = -ENOMEM;
		goto out;
	}

	info->mmio = (uint64_t)mmio;

	for (i = 0; i < pb->num_mmio; i++) {
		ProtoMmio *pb_mmio = pb->mmio[i];

		mmio[i].dev_index = pb_mmio->dev_index;
		mmio[i].addr = pb_mmio->addr;
		mmio[i].size = pb_mmio->size;
		mmio[i].offset = pb_mmio->offset;

		ppu_dbg("Mmio[%03d] addr 0x%lx size 0x%lx offset 0x%lx", i, pb_mmio->addr, pb_mmio->size, pb_mmio->offset);
	}

out:
	ppu_info("Restore %d mmio %s", pb->num_mmio, ret ? "failed" : "success");
	return ret;
}

static int restore_mmio_vma_list(struct ppu_criu_restore_info *info, ProtoCriu *pb)
{
	int ret = 0;
	int i;
	ppu_criu_util_info *mmio = (ppu_criu_util_info *)info->mmio;
	struct vma_metadata *vma_md = NULL;

	ppu_info("Restore %d mmio vma list", pb->num_mmio);

	for (i = 0; i < pb->num_mmio; i++) {
		vma_md = xzalloc(sizeof(*vma_md));
		if (!vma_md) {
			ret = -ENOMEM;
			goto out;
		}

		vma_md->addr = mmio[i].addr;
		vma_md->old_pgoff = mmio[i].offset;
		vma_md->new_pgoff = mmio[i].restored_offset;
		ppu_dbg("Add mmio vma[%d] addr 0x%lx old_pgoff 0x%lx new_pgoff 0x%lx",
			i, vma_md->addr, vma_md->old_pgoff, vma_md->new_pgoff);

		/* transfer ownership */
		list_add_tail(&vma_md->list, &vma_list);
		vma_md = NULL;
	}

out:
	ppu_info("Restore %d mmio vma list %s", pb->num_mmio, ret ? "failed" : "success");
	return ret;
}

static int ppu_init_device_topo(void)
{
	int ret = 0;
	struct dirent *node = NULL;
	DIR *dir = NULL;
	struct dev_entry *ent = NULL;
	struct dev_entry *dummy = NULL;
	LIST_HEAD(ent_list);
	int id = 0;
	int idx = 0;

	dir = opendir("/dev");
	if (!dir) {
		ppu_err("Failed to open /dev");
		ret = -EACCES;
		goto out;
	}

	while ((node = readdir(dir)) != NULL) {
		struct dev_entry *dev = NULL;
		struct dev_entry *tmp = NULL;
		char name[PPU_NODE_NAME_LEN] = "/dev/";

		strncat(name, node->d_name, sizeof(name) - strlen(name) - 1);
		if (sscanf(name, PPU_DEV_NODE_FMT, &id) != 1)
			continue;

		dev = xzalloc(sizeof(*dev));
		if (!dev) {
			ret = -ENOMEM;
			goto out;
		}
		dev->id = id;
		strncpy(dev->name, name, sizeof(dev->name));

		list_for_each_entry(tmp, &ent_list, list) {
			if (tmp->id > dev->id) {
				break;
			}
		}
		list_add_tail(&dev->list, &tmp->list);
	}

	list_for_each_entry_safe(ent, dummy, &ent_list, list) {
		if (idx >= g_ppu_idx) {
			list_add_tail(&ent->list, &dev_list);
			ppu_info("Discover ppu %s", ent->name);
		} else {
			list_del(&ent->list);
			xfree(ent);
			idx++;
		}
	}

out:
	if (dir) {
		closedir(dir);
	}
	return ret;
}

static int restore_bo_data(ppu_criu_bo_info *bos, ProtoCriu *pb, int fd)
{
	struct vma_metadata *vma_md = NULL;
	int i;

	for (i = 0; i < pb->num_bo; i++) {
		if (bos[i].type & PPU_CRIU_BO_TYPE_MMAPPED) {
			vma_md = xzalloc(sizeof(*vma_md));
			if (!vma_md) {
				return -ENOMEM;
			}

			vma_md->addr = bos[i].addr;
			vma_md->old_pgoff = bos[i].offset;
			vma_md->new_pgoff = bos[i].restored_offset;

			ppu_dbg("Add bo vma addr 0x%lx old_pgoff 0x%lx new_pgoff 0x%lx",
				vma_md->addr, vma_md->old_pgoff, vma_md->new_pgoff);

			/* transfer ownership */
			list_add_tail(&vma_md->list, &vma_list);
			vma_md = NULL;
		}

		if (bos[i].sema_addr) {
			vma_md = xzalloc(sizeof(*vma_md));
			if (!vma_md) {
				return -ENOMEM;
			}

			vma_md->addr = bos[i].sema_addr;
			vma_md->old_pgoff = bos[i].sema_offset;
			vma_md->new_pgoff = bos[i].sema_restored_offset;

			ppu_dbg("Add sem vma addr 0x%lx old_pgoff %lx new_pgoff %lx",
				vma_md->addr, vma_md->old_pgoff, vma_md->new_pgoff);

			/* transfer ownership */
			list_add_tail(&vma_md->list, &vma_list);
			vma_md = NULL;
		}
	}

	return 0;
}

static int ppu_plugin_restore_core(void)
{
	int ret = 0;
	FILE *fp = NULL;
	size_t img_size = 0;
	char img_path[PATH_MAX] = {};
	void *buf = NULL;
	struct ppu_criu_restore_info info = {};
	ProtoCriu *pb = NULL;
	int fd = 0;

	ppu_info("Initialized kfd plugin post restore");

	snprintf(img_path, sizeof(img_path), PPU_IMG_FILE, g_char_node_id);
	fp = open_img_file(img_path, false, &img_size);
	if (!fp) {
		goto out;
	}

	fd = open(PPU_CHAR_NODE_NAME, O_RDWR);
	if (fd < 0) {
		ppu_err("Failed to open %s", PPU_CHAR_NODE_NAME);
		ret = -1;
		goto out;
	}

	ppu_info("Open %s file size:%ld success", PPU_IMG_FILE, img_size);

	if (ppu_support_criu() != 0) {
		ppu_err("Kernel not supporte criu");
		ret = -ENOTSUP;
		goto out;
	}

	buf = xmalloc(img_size);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	ret = read_fp(fp, buf, img_size);
	if (ret) {
		ppu_err("Failed to read from %s", img_path);
		goto out;
	}

	pb = proto_criu__unpack(NULL, img_size, buf);
	if (!pb) {
		ppu_err("Failed to unpack protobuf");
		ret = -1;
		goto out;
	}

	ret = restore_device(&info, pb);
	if (ret) {
		ppu_err("Faile to restore device information");
		goto out;
	}

	ret = restore_bo(&info, pb);
	if (ret) {
		ppu_err("Failed to restore bo information");
		goto out;
	}

	ret = restore_queue(&info, pb);
	if (ret) {
		ppu_err("Failed to restore queues information");
		goto out;
	}

	ret = restore_qw(&info, pb);
	if (ret) {
		ppu_err("Failed to restore qw information");
		goto out;
	}

	ret = restore_esr(&info, pb);
	if (ret) {
		ppu_err("Failed to restore esr information");
		goto out;
	}

	ret = restore_mmio(&info, pb);
	if (ret) {
		ppu_err("Failed to restore mmio information");
		goto out;
	}

	info.priv_ptr = (uint64_t)pb->priv_data.data;
	info.priv_size = pb->priv_data.len;

	ret = ppu_restore_process(fd, &info);
	if (ret) {
		ppu_err("Failed to restore process");
		goto out;
	}

	ret = restore_bo_data((void *)info.bos, pb, fd);
	if (ret) {
		ppu_err("Failed to restore bo data");
		goto out;
	}

	ret = restore_queues_vma_list(&info, pb);
	if (ret) {
		ppu_err("Failed to restore queues vma list");
		goto out;
	}

	ret = restore_qw_vma_list(&info, pb);
	if (ret) {
		ppu_err("Failed to restore qw vma list");
		goto out;
	}

	ret = restore_esr_vma_list(&info, pb);
	if (ret) {
		ppu_err("Failed to restore esr vma list");
		goto out;
	}

	ret = restore_mmio_vma_list(&info, pb);
	if (ret) {
		ppu_err("Failed to restore mmio vma list");
		goto out;
	}

out:
	if (pb) {
		proto_criu__free_unpacked(pb, NULL);
	}

	if (info.devices) {
		xfree((void *)info.devices);
	}

	if (info.bos) {
		xfree((void *)info.bos);
	}

	if (fp) {
		fclose(fp);
	}

	if (buf) {
		xfree(buf);
	}

	if (fd) {
		close(fd);
	}

	return ret;
}

static int ppu_restore_device_node(int id)
{
	int ret = 0;
	char img_path[PATH_MAX] = {};
	FILE *fp = NULL;
	size_t img_size = 0;
	ProtoNode *pb_node = NULL;
	char name[PPU_NODE_NAME_LEN] = {};
	void *buf = NULL;
	int fd = 0;

	snprintf(img_path, sizeof(img_path), PPU_IMG_NODE_FILE, id);

	ppu_info("Restore device node %s", img_path);

	fp = open_img_file(img_path, false, &img_size);
	if (!fp) {
		ppu_err("Failed to open %s img file", img_path);
		ret = -EINVAL;
		goto out;
	}

	buf = xmalloc(img_size);
	if (!buf) {
		ppu_err("Failed to allocate device node protobuf");
		ret = -ENOMEM;
		goto out;
	}

	ret = read_fp(fp, buf, img_size);
	if (ret) {
		ppu_err("Failed to read from %s", img_path);
		goto out;
	}

	pb_node = proto_node__unpack(NULL, img_size, buf);
	if (!pb_node) {
		ppu_err("Failed to unpack device node protobuf");
		ret = -1;
		goto out;
	}

	if (strstr(pb_node->name, PPU_DEV_NODE_NAME) != NULL) {
		if (list_empty(&dev_list) == true) {
			ppu_err("No valid device");
			ret = -1;
			goto out;
		} else {
			struct dev_entry *dev = list_first_entry(&dev_list, struct dev_entry, list);
			strncpy(name, dev->name, sizeof(name));
			name[sizeof(name) - 1] = '\0';
			list_del(&dev->list);
			xfree(dev);
		}
	} else if (strcmp(pb_node->name, ANON_INODE_PREFIX PPU_IPC_FD_NAME) == 0) {
		ppu_err(ANON_INODE_PREFIX PPU_IPC_FD_NAME " is not supported yet");
		ret = -EINVAL;
		goto out;
	} else {
		strncpy(name, pb_node->name, sizeof(name));
		name[sizeof(name) - 1] = '\0';
	}

	ppu_info("Restore device node name %s vs %s", pb_node->name, name);

	fd = open(name, O_RDWR);
	if (fd < 0) {
		ppu_err("Failed to open %s: %s", name, strerror(errno));
		ret = -EBADFD;
		goto out;
	}

	if (strcmp(pb_node->name, PPU_CHAR_NODE_NAME) == 0) {
		pid_t pid_cur = getpid();
		int fd_nxt = find_unused_fd_pid(pid_cur);
		if (fd_nxt <= 0) {
			ppu_err("Failed to find unused fd (fd_nxt:%d)", fd_nxt);
			ret = -EINVAL;
			goto out;
		}

		int fd_dup = dup2(fd, fd_nxt);
		if (fd_dup != fd_nxt) {
			ppu_err("Failed to duplicate fd (fd_nxt:%d)", fd_nxt);
			ret = -EBADFD;
			goto out;
		}

		g_ppu_char_fd = fd_dup;
		g_char_node_id = id;
		ppu_info("Ppu fd %d, id %d", g_ppu_char_fd, g_char_node_id);

		ppu_init_device_topo();

		if (ppu_setup_node_count()) {
			ppu_err("Failed to ppu_setup_node_count");
			ret = -EINVAL;
			goto out;
		}
	}

	g_node_count--;
	if (g_node_count == 0) {
		free_dev_list(&dev_list);
		ret = ppu_plugin_restore_core();
		if (ret) {
			goto out;
		}
	}

out:
	if (fp) {
		fclose(fp);
	}

	if (buf) {
		xfree(buf);
	}

	if (ret) {
		if (fd)
			close(fd);
		return ret;
	}

	return fd;
}

/* Assumption: Restore the ppu node first,
 *             because we need to assign a value to g_nodes_id_array. */
int ppu_plugin_restore_file(int id)
{
	int ret = 0;
	int fd = 0;

	ppu_info("Initialized kfd plugin restore with ID = %d", id);

	if (ppu_support_criu() != 0) {
		ppu_err("Kernel not supporte criu");
		ret = -ENOTSUP;
		goto out;
	}

	fd = ppu_restore_device_node(id);

out:

	if (!ret) {
		ret = fd;
	}
	return ret;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__RESTORE_EXT_FILE, ppu_plugin_restore_file)

/**********************************************************************************************************************/
/******************************************* ppu_plugin_update_vmamap *************************************************/
/**********************************************************************************************************************/

int ppu_plugin_update_vmamap(const char *path, const uint64_t addr, const uint64_t old_offset,
			     uint64_t *new_offset, int *updated_fd)
{
	int ret = 0;
	struct vma_metadata *vma_md = NULL;

	ppu_dbg("path %s addr 0x%010lx old_offset 0x%lx", path, addr, old_offset);

	if (strcmp(path, PPU_CHAR_NODE_NAME) != 0) {
		ppu_info("Skipping unsupported path:%s addr 0x%lx old_offset 0x%lx", path, addr, old_offset);
		goto out;
	}

	list_for_each_entry(vma_md, &vma_list, list) {
		if ((addr == vma_md->addr) && (old_offset == vma_md->old_pgoff)) {
			int fd = dup(g_ppu_char_fd);
			if (fd == -1) {
				ppu_err("Failed to duplicate the fd %d", g_ppu_char_fd);
				ret = -1;
				goto out;
			}
			*new_offset = vma_md->new_pgoff;
			*updated_fd = fd;
			ppu_info("Update vmamap old_pgoff 0x%lx new_pgoff 0x%lx fd %d", vma_md->old_pgoff, vma_md->new_pgoff, g_ppu_char_fd);
			list_del(&vma_md->list);
			free(vma_md);

			if (list_empty(&vma_list) && (g_ppu_char_fd > 0)) {
				ppu_info("Close ppu char fd %d", g_ppu_char_fd);
				close(g_ppu_char_fd);
			}
			return 1;
		}
	}

	ppu_err("Failed to match vma path %s addr 0x%010lx old_offset 0x%lx", path, addr, old_offset);

out:
	return ret;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__UPDATE_VMA_MAP, ppu_plugin_update_vmamap)

int ppu_plugin_resume_devices_late(int pid)
{
	int ret = 0, int_ret = 0;
	int restore_tid;
	int ppu_fd;
	int read_fd, write_fd;
	k_rtsigset_t save_sigset;
	const uint64_t code = IPC_CRIU_CMD_RESTORE_MEMORY;
	uint64_t ack;

	ppu_info("Resume target pid = %d", pid);

	ppu_fd = open(PPU_CHAR_NODE_NAME, O_RDWR);
	if (ppu_fd < 0) {
		ppu_err("Failed to open %s", PPU_CHAR_NODE_NAME);
		return -1;
	}

	ret = ppu_restore_process_late(ppu_fd, pid);
	if (ret) {
		ppu_err("Failed to restore process %d late", pid);
		goto out_close_ppu_fd;
	}

	ret = ppu_get_criu_restore_tid(pid, &restore_tid, &read_fd, &write_fd);
	if (ret == 0) {
		ret = resume_restore_thread(restore_tid, &save_sigset);
		if (ret) {
			goto out_close_translate_fd;
		}

		write(write_fd, &code, sizeof(code));
		ret = read_with_timeout(read_fd, &ack, sizeof(ack), g_ipc_timeout);
		if (ret == sizeof(ack) && ack == IPC_ACK_SUCCESS) {
			ret = 0;
		} else {
			ret = -1;
			ppu_err("Failed to get restore process %d ack", pid);
		}

		int_ret = interrupt_restore_thread(restore_tid, &save_sigset);
		ret = ret != 0 ? ret : int_ret;

out_close_translate_fd:
		close(read_fd);
		close(write_fd);

		if (ret)
			goto out_close_ppu_fd;
	} else {
		ppu_info("Failed to get restore process %d restore_tid/read_fd/write_fd", pid);
	}

	ret = ppu_resume_process(ppu_fd, pid);
	if (ret) {
		ppu_err("Failed to resume process %d", pid);
	}

out_close_ppu_fd:
	close(ppu_fd);

	ppu_info("Resume finished");

	return ret;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__RESUME_DEVICES_LATE, ppu_plugin_resume_devices_late)
