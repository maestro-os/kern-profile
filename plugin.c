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
	// Registers context
	struct qemu_plugin_reg_ctx *reg_ctx;
	// The delay between each sample to be collected in microseconds
	suseconds_t sample_delay;
	// The timestamp of the next sample to collect
	struct timeval next_sample_ts;
};

static struct ctx ctx;

// Internal QEMU function. Allows to read or write guest memory.
void cpu_physical_memory_rw(uint64_t addr, uint8_t *buf,
                            uint64_t len, int is_write);

// This is used as a clock to perform sampling
static void vcpu_insn_exec(unsigned int cpu_index, void *udata)
{
	// If the delay isn't expired, ignore
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (
		tv.tv_sec < ctx.next_sample_ts.tv_sec
			|| (tv.tv_sec == ctx.next_sample_ts.tv_sec && tv.tv_usec < ctx.next_sample_ts.tv_usec)
	)
		return;
	ctx.next_sample_ts = tv;
	ctx.next_sample_ts.tv_sec += ctx.sample_delay / 1000000;
	ctx.next_sample_ts.tv_usec += ctx.sample_delay % 1000000;

	// The sample delay has expired. Read the stack and write it to the output file

	// Get registers
	qemu_plugin_regs_load(ctx.reg_ctx);
	uint64_t eip = (uint64_t) qemu_plugin_reg_ptr(ctx.reg_ctx, 8);
	uint64_t ebp = (uint64_t) qemu_plugin_reg_ptr(ctx.reg_ctx, 6);

	// Iterate through stack
	uint64_t frames_buf[MAX_DEPTH];
	frames_buf[0] = eip;
	uint8_t i;
	for (i = 1; i < MAX_DEPTH; ++i)
	{
		// If reached the end of the stack (exclude code outside of the kernel)
		if (ebp <= (uint64_t) 0xc0000000) // TODO 64 bits
			break;

		char buf[8];

		// TODO do only one read in memory

		// Get function address (return address on the stack)
		cpu_physical_memory_rw(ebp + 4, buf, sizeof(buf), 0); // TODO 64 bits
		frames_buf[i] = *(uint64_t *) &buf[0];

		// Get next frame
		cpu_physical_memory_rw(ebp, buf, sizeof(buf), 0);
		ebp = *(uint64_t *) &buf[0];
	}

	// Build iovec
	struct iovec v[MAX_DEPTH + 1];
	// Frames count
	v[0].iov_base = (void *) &i;
	v[0].iov_len = sizeof(i);
	size_t j;
	for (j = 1; j < i + 1; ++j)
	{
		v[j].iov_base = (void *) frames_buf[i - 1];
		v[j].iov_len = sizeof(uint64_t);
	}

	// TODO loop until everything is written
	writev(ctx.out_fd, v, j);
	// TODO check errno
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
			insn, vcpu_insn_exec, QEMU_PLUGIN_CB_NO_REGS,
			GUINT_TO_POINTER(vaddr));
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
	qemu_plugin_reg_free_context(ctx.reg_ctx);

	// TODO ensure everything is written to file
	close(ctx.out_fd);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
	if (argc == 0)
	{
		dprintf(STDERR_FILENO, "TODO: error message"); // TODO
		return 1;
	}
	// TODO take sample rate as argument

	// Open output file
	char *out_path = argv[0];
	ctx.out_fd = open(out_path, O_CREAT | O_TRUNC);
	if (errno)
	{
		dprintf(STDERR_FILENO, "qemu: %s: %s", out_path, strerror(errno));
		return 1;
	}

	ctx.reg_ctx = qemu_plugin_reg_create_context(X86_64_REGS, sizeof(X86_64_REGS) / sizeof(X86_64_REGS[0]));

	// Init timing
	ctx.sample_delay = 0; // TODO take from arg
	gettimeofday(&ctx.next_sample_ts, NULL);

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
	return 0;
}
