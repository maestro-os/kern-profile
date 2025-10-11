// This plugin has been written to support QEMU version 8.2.0
// Using other versions might break it

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#include <glib.h>

#include <qemu-plugin.h>

// Hard limit to the stack depth to observe
#define MAX_DEPTH	64

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

struct ctx
{
	// The FD of the output file
	int out_fd;
	// The delay between each sample to be collected in microseconds
	suseconds_t sample_delay;
	// The timestamp of the next sample to collect
	struct timeval next_sample_ts;

	// sizeof(target_ulong) (include/exec/target_ulong.h)
	size_t target_ulong_width;

	struct qemu_plugin_register *efer_handle;
	struct qemu_plugin_register *rsp_handle;
};

static struct ctx ctx;

// The usage of internal QEMU functions are hacks addressing the limitations of
// TCG plugins. Even though the public API is allowed to change from one version
// to another, those are even more prone to break
//
// QEMU provides the function `qemu_plugin_read_memory_vaddr`, however it does
// not guarantee all writes to memory have been flushed

// Internal QEMU function. Returns the CPU with the given ID.
extern void *qemu_get_cpu(int index);
// Internal QEMU function. Allows to read or write guest memory.
extern int cpu_memory_rw_debug(void *cpu, uint64_t addr,
                               void *ptr, uint64_t len, bool is_write);

// Find register handle from name
static struct qemu_plugin_register *find_reg(GArray *regs, const char *name)
{
	for (guint i = 1; i < regs->len; ++i) {
        qemu_plugin_reg_descriptor reg = g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (!strcmp(reg.name, name))
			return reg.handle;
    }
}

// Callback at VCPU init to get handles for registers
static void vcpu_get_reg(qemu_plugin_id_t id, unsigned int vcpu_index)
{
	// Retrieve registers of interest
	GArray *regs = qemu_plugin_get_registers();
	ctx.efer_handle = find_reg(regs, "efer");
	ctx.rsp_handle = find_reg(regs, "rsp");
	g_array_free(regs, 0);
}

static uint64_t get_register_val(struct qemu_plugin_register *reg)
{
	GByteArray *arr = g_byte_array_new();
	int res = qemu_plugin_read_register(reg, arr);
	assert(res == ctx.target_ulong_width);
	// Copy
	uint64_t val = 0;
	memcpy(&val, arr->data, ctx.target_ulong_width);
	return val;
}

// This is used as a clock to perform sampling
static void vcpu_insn_exec(unsigned int cpu_index, void *rip)
{
	// If the delay isn't expired, ignore
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (tv.tv_sec < ctx.next_sample_ts.tv_sec
		|| (tv.tv_sec == ctx.next_sample_ts.tv_sec
		&& tv.tv_usec < ctx.next_sample_ts.tv_usec))
		return;
	ctx.next_sample_ts = tv;
	ctx.next_sample_ts.tv_sec += ctx.sample_delay / 1000000;
	ctx.next_sample_ts.tv_usec += ctx.sample_delay % 1000000;

	// The sample delay has expired. Read the stack and write it to the output file

	// Get registers
	void *cpu = qemu_get_cpu(cpu_index);
	uint64_t frame_ptr = get_register_val(ctx.rsp_handle);
	bool long_mode = get_register_val(ctx.efer_handle) & (1 << 8);
	uint8_t ptr_width = long_mode ? 8 : 4;

	// Iterate through stack
	uint64_t frames_buf[MAX_DEPTH];
	frames_buf[0] = (uint64_t) rip;

	uint8_t i;
	char buf[8]; // We'll overallocate for 32-bit. It's fine.
	for (i = 1; i < MAX_DEPTH; ++i)
    {
		// TODO do only one read in memory

		// Get function address (return address on the stack)
		int err = cpu_memory_rw_debug(cpu, frame_ptr + ptr_width, buf, ctx.target_ulong_width, false);
		if (err)
			break;

		if (long_mode) {
			frames_buf[i] = *(uint64_t *) &buf[0];
		} else {
			frames_buf[i] = *(uint32_t *) &buf[0];
		}

		// XXX: Any frames outside the kernel are discarded in the parser.
		//
		// Get next frame
		err = cpu_memory_rw_debug(cpu, frame_ptr, buf, ctx.target_ulong_width, false);
		if (err)
			break;

		if (long_mode) {
			frame_ptr = *(uint64_t *) &buf[0];
		} else {
			frame_ptr = *(uint32_t *) &buf[0];
		}
	}

	// Build iovec
	struct iovec v[2];
	// Frames count
	v[0].iov_base = (void *) &i;
	v[0].iov_len = sizeof(i);
	// Frames
	v[1].iov_base = (void *) &frames_buf[0];
	v[1].iov_len = i * sizeof(uint64_t);

	// TODO loop until everything is written
	errno = 0;
	writev(ctx.out_fd, v, 2);
	if (errno)
		dprintf(STDERR_FILENO, "warning: could not write to output file: %s\n", strerror(errno));
}

// Executed each time a block of instructions is translated
static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;
    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
		uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
		qemu_plugin_register_vcpu_insn_exec_cb(
			insn, vcpu_insn_exec, QEMU_PLUGIN_CB_R_REGS,
			GUINT_TO_POINTER(vaddr));
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
	close(ctx.out_fd);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
	if (!strcmp(info->target_name, "x86_64")) {
		ctx.target_ulong_width = 8;
	} else if (!strcmp(info->target_name, "i386")) {
		ctx.target_ulong_width = 4;
	} else {
		dprintf(STDERR_FILENO, "unsupported target: %s\n", info->target_name);
		return -1;
	}

	// Default values
	char *out_path = "qemu-profile";
	suseconds_t sample_delay = 10;
	// Parse arguments
	for (size_t i = 0; i < argc; ++i)
	{
        char *name = argv[i];
        char *name_end = strchr(name, '=');
		char *val = name_end + 1;
		*name_end = '\0';
		if (g_strcmp0(name, "out") == 0)
			out_path = val;
		else if (g_strcmp0(name, "delay") == 0)
			sample_delay = atoi(val);
		else
		{
			dprintf(STDERR_FILENO, "invalid argument: %s\n", name);
			return -1;
		}
	}

	// Open output file
	errno = 0;
	ctx.out_fd = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
	if (errno)
	{
		dprintf(STDERR_FILENO, "qemu: %s: %s", out_path, strerror(errno));
		return 1;
	}

	// Init timing
	ctx.sample_delay = sample_delay;
	gettimeofday(&ctx.next_sample_ts, NULL);

	qemu_plugin_register_vcpu_init_cb(id, vcpu_get_reg);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
	return 0;
}
