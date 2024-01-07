#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/time.h>

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
	ctx.next_sample_ts.tv_sec += sample_delay / 1000000;
	ctx.next_sample_ts.tv_usec += sample_delay % 1000000;

	// The sample delay has expired. Read the stack and write it to the output file

	// Get registers
	qemu_plugin_regs_load(ctx->reg_ctx);
	void *eip = qemu_plugin_reg_ptr(ctx->reg_ctx, 8);
	void *esp = qemu_plugin_reg_ptr(ctx->reg_ctx, 7);
	void *ebp = qemu_plugin_reg_ptr(ctx->reg_ctx, 6);

	// Iterate through stack
	struct iovec frames_buf[MAX_DEPTH];
	frames_buf[0] = eip;
	uint8_t i;
	for (i = 1; i < MAX_DEPTH; ++i)
	{
		// If reached the end of the stack (exclude code outside of the kernel)
		if (ebp <= 0xc0000000) // TODO adapt for 64 bits
			break;

		char buf[4]; // TODO adapt size for 64 bits

		// Get function address (return address on the stack)
		cpu_physical_memory_rw(ebp + sizeof(buf), buf, sizeof(buf), 0);
		frames_buf[i].iov_base = *(uint32_t *) &buf[0];
		frames_buf[i].iov_len = sizeof(buf);

		// Get next frame
		cpu_physical_memory_rw(ebp, buf, sizeof(buf), 0);
		ebp = *(uint32_t *) &buf[0];
	}

	// Write the number of frames in stack
	write(out_fd, i, sizeof(i));
	// TODO check errno
	// Write frames
	writev(out_fd, frames_buf, i);
	// TODO check errno
}

// Executed each time a block of instructions is translated
static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;

    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        if (do_inline) {
            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, &inline_insn_count, 1);
        } else {
            uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_insn_exec, QEMU_PLUGIN_CB_NO_REGS,
                GUINT_TO_POINTER(vaddr));
        }

        if (do_size) {
            size_t sz = qemu_plugin_insn_size(insn);
            if (sz > sizes->len) {
                g_array_set_size(sizes, sz);
            }
            unsigned long *cnt = &g_array_index(sizes, unsigned long, sz);
            (*cnt)++;
        }

        /*
         * If we are tracking certain instructions we will need more
         * information about the instruction which we also need to
         * save if there is a hit.
         */
        if (matches) {
            char *insn_disas = qemu_plugin_insn_disas(insn);
            int j;
            for (j = 0; j < matches->len; j++) {
                Match *m = &g_array_index(matches, Match, j);
                if (g_str_has_prefix(insn_disas, m->match_string)) {
                    Instruction *rec = g_new0(Instruction, 1);
                    rec->disas = g_strdup(insn_disas);
                    rec->vaddr = qemu_plugin_insn_vaddr(insn);
                    rec->match = m;
                    qemu_plugin_register_vcpu_insn_exec_cb(
                        insn, vcpu_insn_matched_exec_before,
                        QEMU_PLUGIN_CB_NO_REGS, rec);
                }
            }
            g_free(insn_disas);
        }
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
	qemu_plugin_reg_free_context(cache->reg_ctx);

	// TODO ensure everything is written to file
	close(out_fd);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
	if (argc == 0)
	{
		fprintf(STDERR_FILENO, "TODO: error message"); // TODO
		return 1;
	}
	// TODO take sample rate as argument

	// Open output file
	char *out_path = argv[0];
	out_fd = open(out_path, O_CREAT | O_TRUNC);
	if (errno)
	{
		fprintf(STDERR_FILENO, "qemu: %s: %s", out_path, strerror(errno));
		return 1;
	}

	ctx.reg_ctx = qemu_plugin_reg_create_context(X86_64_REGS, sizeof(X86_64_REGS) / sizeof(X86_64_REGS[0]));

	// Init timing
	sample_delay = 0; // TODO take from arg
	gettimeofday(&last_sample_ts, NULL);

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
	return 0;
}
