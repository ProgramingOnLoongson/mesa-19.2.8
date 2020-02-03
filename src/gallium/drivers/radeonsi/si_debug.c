/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "si_pipe.h"
#include "si_compute.h"
#include "sid.h"
#include "sid_tables.h"
#include "tgsi/tgsi_from_mesa.h"
#include "driver_ddebug/dd_util.h"
#include "util/u_dump.h"
#include "util/u_log.h"
#include "util/u_memory.h"
#include "util/u_string.h"
#include "ac_debug.h"
#include "ac_rtld.h"

static void si_dump_bo_list(struct si_context *sctx,
			    const struct radeon_saved_cs *saved, FILE *f);

DEBUG_GET_ONCE_OPTION(replace_shaders, "RADEON_REPLACE_SHADERS", NULL)

/**
 * Store a linearized copy of all chunks of \p cs together with the buffer
 * list in \p saved.
 */
void si_save_cs(struct radeon_winsys *ws, struct radeon_cmdbuf *cs,
		struct radeon_saved_cs *saved, bool get_buffer_list)
{
	uint32_t *buf;
	unsigned i;

	/* Save the IB chunks. */
	saved->num_dw = cs->prev_dw + cs->current.cdw;
	saved->ib = MALLOC(4 * saved->num_dw);
	if (!saved->ib)
		goto oom;

	buf = saved->ib;
	for (i = 0; i < cs->num_prev; ++i) {
		memcpy(buf, cs->prev[i].buf, cs->prev[i].cdw * 4);
		buf += cs->prev[i].cdw;
	}
	memcpy(buf, cs->current.buf, cs->current.cdw * 4);

	if (!get_buffer_list)
		return;

	/* Save the buffer list. */
	saved->bo_count = ws->cs_get_buffer_list(cs, NULL);
	saved->bo_list = CALLOC(saved->bo_count,
				sizeof(saved->bo_list[0]));
	if (!saved->bo_list) {
		FREE(saved->ib);
		goto oom;
	}
	ws->cs_get_buffer_list(cs, saved->bo_list);

	return;

oom:
	fprintf(stderr, "%s: out of memory\n", __func__);
	memset(saved, 0, sizeof(*saved));
}

void si_clear_saved_cs(struct radeon_saved_cs *saved)
{
	FREE(saved->ib);
	FREE(saved->bo_list);

	memset(saved, 0, sizeof(*saved));
}

void si_destroy_saved_cs(struct si_saved_cs *scs)
{
	si_clear_saved_cs(&scs->gfx);
	si_resource_reference(&scs->trace_buf, NULL);
	free(scs);
}

static void si_dump_shader(struct si_screen *sscreen,
			   struct si_shader *shader, FILE *f)
{
	if (shader->shader_log)
		fwrite(shader->shader_log, shader->shader_log_size, 1, f);
	else
		si_shader_dump(sscreen, shader, NULL, f, false);

	if (shader->bo && sscreen->options.dump_shader_binary) {
		unsigned size = shader->bo->b.b.width0;
		fprintf(f, "BO: VA=%"PRIx64" Size=%u\n", shader->bo->gpu_address, size);

		const char *mapped = sscreen->ws->buffer_map(shader->bo->buf, NULL,
						       PIPE_TRANSFER_UNSYNCHRONIZED |
						       PIPE_TRANSFER_READ |
						       RADEON_TRANSFER_TEMPORARY);

		for (unsigned i = 0; i < size; i += 4) {
			fprintf(f, " %4x: %08x\n", i, *(uint32_t*)(mapped + i));
		}

		sscreen->ws->buffer_unmap(shader->bo->buf);

		fprintf(f, "\n");
	}
}

struct si_log_chunk_shader {
	/* The shader destroy code assumes a current context for unlinking of
	 * PM4 packets etc.
	 *
	 * While we should be able to destroy shaders without a context, doing
	 * so would happen only very rarely and be therefore likely to fail
	 * just when you're trying to debug something. Let's just remember the
	 * current context in the chunk.
	 */
	struct si_context *ctx;
	struct si_shader *shader;

	/* For keep-alive reference counts */
	struct si_shader_selector *sel;
	struct si_compute *program;
};

static void
si_log_chunk_shader_destroy(void *data)
{
	struct si_log_chunk_shader *chunk = data;
	si_shader_selector_reference(chunk->ctx, &chunk->sel, NULL);
	si_compute_reference(&chunk->program, NULL);
	FREE(chunk);
}

static void
si_log_chunk_shader_print(void *data, FILE *f)
{
	struct si_log_chunk_shader *chunk = data;
	struct si_screen *sscreen = chunk->ctx->screen;
	si_dump_shader(sscreen, chunk->shader, f);
}

static struct u_log_chunk_type si_log_chunk_type_shader = {
	.destroy = si_log_chunk_shader_destroy,
	.print = si_log_chunk_shader_print,
};

static void si_dump_gfx_shader(struct si_context *ctx,
			       const struct si_shader_ctx_state *state,
			       struct u_log_context *log)
{
	struct si_shader *current = state->current;

	if (!state->cso || !current)
		return;

	struct si_log_chunk_shader *chunk = CALLOC_STRUCT(si_log_chunk_shader);
	chunk->ctx = ctx;
	chunk->shader = current;
	si_shader_selector_reference(ctx, &chunk->sel, current->selector);
	u_log_chunk(log, &si_log_chunk_type_shader, chunk);
}

static void si_dump_compute_shader(struct si_context *ctx,
				   struct u_log_context *log)
{
	const struct si_cs_shader_state *state = &ctx->cs_shader_state;

	if (!state->program)
		return;

	struct si_log_chunk_shader *chunk = CALLOC_STRUCT(si_log_chunk_shader);
	chunk->ctx = ctx;
	chunk->shader = &state->program->shader;
	si_compute_reference(&chunk->program, state->program);
	u_log_chunk(log, &si_log_chunk_type_shader, chunk);
}

/**
 * Shader compiles can be overridden with arbitrary ELF objects by setting
 * the environment variable RADEON_REPLACE_SHADERS=num1:filename1[;num2:filename2]
 *
 * TODO: key this off some hash
 */
bool si_replace_shader(unsigned num, struct si_shader_binary *binary)
{
	const char *p = debug_get_option_replace_shaders();
	const char *semicolon;
	char *copy = NULL;
	FILE *f;
	long filesize, nread;
	bool replaced = false;

	if (!p)
		return false;

	while (*p) {
		unsigned long i;
		char *endp;
		i = strtoul(p, &endp, 0);

		p = endp;
		if (*p != ':') {
			fprintf(stderr, "RADEON_REPLACE_SHADERS formatted badly.\n");
			exit(1);
		}
		++p;

		if (i == num)
			break;

		p = strchr(p, ';');
		if (!p)
			return false;
		++p;
	}
	if (!*p)
		return false;

	semicolon = strchr(p, ';');
	if (semicolon) {
		p = copy = strndup(p, semicolon - p);
		if (!copy) {
			fprintf(stderr, "out of memory\n");
			return false;
		}
	}

	fprintf(stderr, "radeonsi: replace shader %u by %s\n", num, p);

	f = fopen(p, "r");
	if (!f) {
		perror("radeonsi: failed to open file");
		goto out_free;
	}

	if (fseek(f, 0, SEEK_END) != 0)
		goto file_error;

	filesize = ftell(f);
	if (filesize < 0)
		goto file_error;

	if (fseek(f, 0, SEEK_SET) != 0)
		goto file_error;

	binary->elf_buffer = MALLOC(filesize);
	if (!binary->elf_buffer) {
		fprintf(stderr, "out of memory\n");
		goto out_close;
	}

	nread = fread((void*)binary->elf_buffer, 1, filesize, f);
	if (nread != filesize) {
		FREE((void*)binary->elf_buffer);
		binary->elf_buffer = NULL;
		goto file_error;
	}

	binary->elf_size = nread;
	replaced = true;

out_close:
	fclose(f);
out_free:
	free(copy);
	return replaced;

file_error:
	perror("radeonsi: reading shader");
	goto out_close;
}

/* Parsed IBs are difficult to read without colors. Use "less -R file" to
 * read them, or use "aha -b -f file" to convert them to html.
 */
#define COLOR_RESET	"\033[0m"
#define COLOR_RED	"\033[31m"
#define COLOR_GREEN	"\033[1;32m"
#define COLOR_YELLOW	"\033[1;33m"
#define COLOR_CYAN	"\033[1;36m"

static void si_dump_mmapped_reg(struct si_context *sctx, FILE *f,
				unsigned offset)
{
	struct radeon_winsys *ws = sctx->ws;
	uint32_t value;

	if (ws->read_registers(ws, offset, 1, &value))
		ac_dump_reg(f, sctx->chip_class, offset, value, ~0);
}

static void si_dump_debug_registers(struct si_context *sctx, FILE *f)
{
	if (!sctx->screen->info.has_read_registers_query)
		return;

	fprintf(f, "Memory-mapped registers:\n");
	si_dump_mmapped_reg(sctx, f, R_008010_GRBM_STATUS);

	/* No other registers can be read on DRM < 3.1.0. */
	if (!sctx->screen->info.is_amdgpu ||
	    sctx->screen->info.drm_minor < 1) {
		fprintf(f, "\n");
		return;
	}

	si_dump_mmapped_reg(sctx, f, R_008008_GRBM_STATUS2);
	si_dump_mmapped_reg(sctx, f, R_008014_GRBM_STATUS_SE0);
	si_dump_mmapped_reg(sctx, f, R_008018_GRBM_STATUS_SE1);
	si_dump_mmapped_reg(sctx, f, R_008038_GRBM_STATUS_SE2);
	si_dump_mmapped_reg(sctx, f, R_00803C_GRBM_STATUS_SE3);
	si_dump_mmapped_reg(sctx, f, R_00D034_SDMA0_STATUS_REG);
	si_dump_mmapped_reg(sctx, f, R_00D834_SDMA1_STATUS_REG);
	if (sctx->chip_class <= GFX8) {
		si_dump_mmapped_reg(sctx, f, R_000E50_SRBM_STATUS);
		si_dump_mmapped_reg(sctx, f, R_000E4C_SRBM_STATUS2);
		si_dump_mmapped_reg(sctx, f, R_000E54_SRBM_STATUS3);
	}
	si_dump_mmapped_reg(sctx, f, R_008680_CP_STAT);
	si_dump_mmapped_reg(sctx, f, R_008674_CP_STALLED_STAT1);
	si_dump_mmapped_reg(sctx, f, R_008678_CP_STALLED_STAT2);
	si_dump_mmapped_reg(sctx, f, R_008670_CP_STALLED_STAT3);
	si_dump_mmapped_reg(sctx, f, R_008210_CP_CPC_STATUS);
	si_dump_mmapped_reg(sctx, f, R_008214_CP_CPC_BUSY_STAT);
	si_dump_mmapped_reg(sctx, f, R_008218_CP_CPC_STALLED_STAT1);
	si_dump_mmapped_reg(sctx, f, R_00821C_CP_CPF_STATUS);
	si_dump_mmapped_reg(sctx, f, R_008220_CP_CPF_BUSY_STAT);
	si_dump_mmapped_reg(sctx, f, R_008224_CP_CPF_STALLED_STAT1);
	fprintf(f, "\n");
}

struct si_log_chunk_cs {
	struct si_context *ctx;
	struct si_saved_cs *cs;
	bool dump_bo_list;
	unsigned gfx_begin, gfx_end;
	unsigned compute_begin, compute_end;
};

static void si_log_chunk_type_cs_destroy(void *data)
{
	struct si_log_chunk_cs *chunk = data;
	si_saved_cs_reference(&chunk->cs, NULL);
	free(chunk);
}

static void si_parse_current_ib(FILE *f, struct radeon_cmdbuf *cs,
				unsigned begin, unsigned end,
				int *last_trace_id, unsigned trace_id_count,
				const char *name, enum chip_class chip_class)
{
	unsigned orig_end = end;

	assert(begin <= end);

	fprintf(f, "------------------ %s begin (dw = %u) ------------------\n",
		name, begin);

	for (unsigned prev_idx = 0; prev_idx < cs->num_prev; ++prev_idx) {
		struct radeon_cmdbuf_chunk *chunk = &cs->prev[prev_idx];

		if (begin < chunk->cdw) {
			ac_parse_ib_chunk(f, chunk->buf + begin,
					  MIN2(end, chunk->cdw) - begin,
					  last_trace_id, trace_id_count,
				          chip_class, NULL, NULL);
		}

		if (end <= chunk->cdw)
			return;

		if (begin < chunk->cdw)
			fprintf(f, "\n---------- Next %s Chunk ----------\n\n",
				name);

		begin -= MIN2(begin, chunk->cdw);
		end -= chunk->cdw;
	}

	assert(end <= cs->current.cdw);

	ac_parse_ib_chunk(f, cs->current.buf + begin, end - begin, last_trace_id,
			  trace_id_count, chip_class, NULL, NULL);

	fprintf(f, "------------------- %s end (dw = %u) -------------------\n\n",
		name, orig_end);
}

static void si_log_chunk_type_cs_print(void *data, FILE *f)
{
	struct si_log_chunk_cs *chunk = data;
	struct si_context *ctx = chunk->ctx;
	struct si_saved_cs *scs = chunk->cs;
	int last_trace_id = -1;
	int last_compute_trace_id = -1;

	/* We are expecting that the ddebug pipe has already
	 * waited for the context, so this buffer should be idle.
	 * If the GPU is hung, there is no point in waiting for it.
	 */
	uint32_t *map = ctx->ws->buffer_map(scs->trace_buf->buf,
					      NULL,
					      PIPE_TRANSFER_UNSYNCHRONIZED |
					      PIPE_TRANSFER_READ);
	if (map) {
		last_trace_id = map[0];
		last_compute_trace_id = map[1];
	}

	if (chunk->gfx_end != chunk->gfx_begin) {
		if (chunk->gfx_begin == 0) {
			if (ctx->init_config)
				ac_parse_ib(f, ctx->init_config->pm4, ctx->init_config->ndw,
					    NULL, 0, "IB2: Init config", ctx->chip_class,
					    NULL, NULL);

			if (ctx->init_config_gs_rings)
				ac_parse_ib(f, ctx->init_config_gs_rings->pm4,
					    ctx->init_config_gs_rings->ndw,
					    NULL, 0, "IB2: Init GS rings", ctx->chip_class,
					    NULL, NULL);
		}

		if (scs->flushed) {
			ac_parse_ib(f, scs->gfx.ib + chunk->gfx_begin,
				    chunk->gfx_end - chunk->gfx_begin,
				    &last_trace_id, map ? 1 : 0, "IB", ctx->chip_class,
				    NULL, NULL);
		} else {
			si_parse_current_ib(f, ctx->gfx_cs, chunk->gfx_begin,
					    chunk->gfx_end, &last_trace_id, map ? 1 : 0,
					    "IB", ctx->chip_class);
		}
	}

	if (chunk->compute_end != chunk->compute_begin) {
		assert(ctx->prim_discard_compute_cs);

		if (scs->flushed) {
			ac_parse_ib(f, scs->compute.ib + chunk->compute_begin,
				    chunk->compute_end - chunk->compute_begin,
				    &last_compute_trace_id, map ? 1 : 0, "Compute IB", ctx->chip_class,
				    NULL, NULL);
		} else {
			si_parse_current_ib(f, ctx->prim_discard_compute_cs, chunk->compute_begin,
					    chunk->compute_end, &last_compute_trace_id,
					    map ? 1 : 0, "Compute IB", ctx->chip_class);
		}
	}

	if (chunk->dump_bo_list) {
		fprintf(f, "Flushing. Time: ");
		util_dump_ns(f, scs->time_flush);
		fprintf(f, "\n\n");
		si_dump_bo_list(ctx, &scs->gfx, f);
	}
}

static const struct u_log_chunk_type si_log_chunk_type_cs = {
	.destroy = si_log_chunk_type_cs_destroy,
	.print = si_log_chunk_type_cs_print,
};

static void si_log_cs(struct si_context *ctx, struct u_log_context *log,
		      bool dump_bo_list)
{
	assert(ctx->current_saved_cs);

	struct si_saved_cs *scs = ctx->current_saved_cs;
	unsigned gfx_cur = ctx->gfx_cs->prev_dw + ctx->gfx_cs->current.cdw;
	unsigned compute_cur = 0;

	if (ctx->prim_discard_compute_cs)
		compute_cur = ctx->prim_discard_compute_cs->prev_dw + ctx->prim_discard_compute_cs->current.cdw;

	if (!dump_bo_list &&
	    gfx_cur == scs->gfx_last_dw &&
	    compute_cur == scs->compute_last_dw)
		return;

	struct si_log_chunk_cs *chunk = calloc(1, sizeof(*chunk));

	chunk->ctx = ctx;
	si_saved_cs_reference(&chunk->cs, scs);
	chunk->dump_bo_list = dump_bo_list;

	chunk->gfx_begin = scs->gfx_last_dw;
	chunk->gfx_end = gfx_cur;
	scs->gfx_last_dw = gfx_cur;

	chunk->compute_begin = scs->compute_last_dw;
	chunk->compute_end = compute_cur;
	scs->compute_last_dw = compute_cur;

	u_log_chunk(log, &si_log_chunk_type_cs, chunk);
}

void si_auto_log_cs(void *data, struct u_log_context *log)
{
	struct si_context *ctx = (struct si_context *)data;
	si_log_cs(ctx, log, false);
}

void si_log_hw_flush(struct si_context *sctx)
{
	if (!sctx->log)
		return;

	si_log_cs(sctx, sctx->log, true);

	if (&sctx->b == sctx->screen->aux_context) {
		/* The aux context isn't captured by the ddebug wrapper,
		 * so we dump it on a flush-by-flush basis here.
		 */
		FILE *f = dd_get_debug_file(false);
		if (!f) {
			fprintf(stderr, "radeonsi: error opening aux context dump file.\n");
		} else {
			dd_write_header(f, &sctx->screen->b, 0);

			fprintf(f, "Aux context dump:\n\n");
			u_log_new_page_print(sctx->log, f);

			fclose(f);
		}
	}
}

static const char *priority_to_string(enum radeon_bo_priority priority)
{
#define ITEM(x) [RADEON_PRIO_##x] = #x
	static const char *table[64] = {
		ITEM(FENCE),
	        ITEM(TRACE),
	        ITEM(SO_FILLED_SIZE),
	        ITEM(QUERY),
	        ITEM(IB1),
	        ITEM(IB2),
	        ITEM(DRAW_INDIRECT),
	        ITEM(INDEX_BUFFER),
		ITEM(CP_DMA),
	        ITEM(CONST_BUFFER),
	        ITEM(DESCRIPTORS),
	        ITEM(BORDER_COLORS),
	        ITEM(SAMPLER_BUFFER),
	        ITEM(VERTEX_BUFFER),
	        ITEM(SHADER_RW_BUFFER),
	        ITEM(COMPUTE_GLOBAL),
	        ITEM(SAMPLER_TEXTURE),
	        ITEM(SHADER_RW_IMAGE),
	        ITEM(SAMPLER_TEXTURE_MSAA),
	        ITEM(COLOR_BUFFER),
	        ITEM(DEPTH_BUFFER),
	        ITEM(COLOR_BUFFER_MSAA),
	        ITEM(DEPTH_BUFFER_MSAA),
	        ITEM(SEPARATE_META),
		ITEM(SHADER_BINARY),
		ITEM(SHADER_RINGS),
		ITEM(SCRATCH_BUFFER),
	};
#undef ITEM

	assert(priority < ARRAY_SIZE(table));
	return table[priority];
}

static int bo_list_compare_va(const struct radeon_bo_list_item *a,
				   const struct radeon_bo_list_item *b)
{
	return a->vm_address < b->vm_address ? -1 :
	       a->vm_address > b->vm_address ? 1 : 0;
}

static void si_dump_bo_list(struct si_context *sctx,
			    const struct radeon_saved_cs *saved, FILE *f)
{
	unsigned i,j;

	if (!saved->bo_list)
		return;

	/* Sort the list according to VM adddresses first. */
	qsort(saved->bo_list, saved->bo_count,
	      sizeof(saved->bo_list[0]), (void*)bo_list_compare_va);

	fprintf(f, "Buffer list (in units of pages = 4kB):\n"
		COLOR_YELLOW "        Size    VM start page         "
		"VM end page           Usage" COLOR_RESET "\n");

	for (i = 0; i < saved->bo_count; i++) {
		/* Note: Buffer sizes are expected to be aligned to 4k by the winsys. */
		const unsigned page_size = sctx->screen->info.gart_page_size;
		uint64_t va = saved->bo_list[i].vm_address;
		uint64_t size = saved->bo_list[i].bo_size;
		bool hit = false;

		/* If there's unused virtual memory between 2 buffers, print it. */
		if (i) {
			uint64_t previous_va_end = saved->bo_list[i-1].vm_address +
						   saved->bo_list[i-1].bo_size;

			if (va > previous_va_end) {
				fprintf(f, "  %10"PRIu64"    -- hole --\n",
					(va - previous_va_end) / page_size);
			}
		}

		/* Print the buffer. */
		fprintf(f, "  %10"PRIu64"    0x%013"PRIX64"       0x%013"PRIX64"       ",
			size / page_size, va / page_size, (va + size) / page_size);

		/* Print the usage. */
		for (j = 0; j < 32; j++) {
			if (!(saved->bo_list[i].priority_usage & (1u << j)))
				continue;

			fprintf(f, "%s%s", !hit ? "" : ", ", priority_to_string(j));
			hit = true;
		}
		fprintf(f, "\n");
	}
	fprintf(f, "\nNote: The holes represent memory not used by the IB.\n"
		   "      Other buffers can still be allocated there.\n\n");
}

static void si_dump_framebuffer(struct si_context *sctx, struct u_log_context *log)
{
	struct pipe_framebuffer_state *state = &sctx->framebuffer.state;
	struct si_texture *tex;
	int i;

	for (i = 0; i < state->nr_cbufs; i++) {
		if (!state->cbufs[i])
			continue;

		tex = (struct si_texture*)state->cbufs[i]->texture;
		u_log_printf(log, COLOR_YELLOW "Color buffer %i:" COLOR_RESET "\n", i);
		si_print_texture_info(sctx->screen, tex, log);
		u_log_printf(log, "\n");
	}

	if (state->zsbuf) {
		tex = (struct si_texture*)state->zsbuf->texture;
		u_log_printf(log, COLOR_YELLOW "Depth-stencil buffer:" COLOR_RESET "\n");
		si_print_texture_info(sctx->screen, tex, log);
		u_log_printf(log, "\n");
	}
}

typedef unsigned (*slot_remap_func)(unsigned);

struct si_log_chunk_desc_list {
	/** Pointer to memory map of buffer where the list is uploader */
	uint32_t *gpu_list;
	/** Reference of buffer where the list is uploaded, so that gpu_list
	 * is kept live. */
	struct si_resource *buf;

	const char *shader_name;
	const char *elem_name;
	slot_remap_func slot_remap;
	enum chip_class chip_class;
	unsigned element_dw_size;
	unsigned num_elements;

	uint32_t list[0];
};

static void
si_log_chunk_desc_list_destroy(void *data)
{
	struct si_log_chunk_desc_list *chunk = data;
	si_resource_reference(&chunk->buf, NULL);
	FREE(chunk);
}

static void
si_log_chunk_desc_list_print(void *data, FILE *f)
{
	struct si_log_chunk_desc_list *chunk = data;
	unsigned sq_img_rsrc_word0 = chunk->chip_class >= GFX10 ? R_00A000_SQ_IMG_RSRC_WORD0
								: R_008F10_SQ_IMG_RSRC_WORD0;

	for (unsigned i = 0; i < chunk->num_elements; i++) {
		unsigned cpu_dw_offset = i * chunk->element_dw_size;
		unsigned gpu_dw_offset = chunk->slot_remap(i) * chunk->element_dw_size;
		const char *list_note = chunk->gpu_list ? "GPU list" : "CPU list";
		uint32_t *cpu_list = chunk->list + cpu_dw_offset;
		uint32_t *gpu_list = chunk->gpu_list ? chunk->gpu_list + gpu_dw_offset : cpu_list;

		fprintf(f, COLOR_GREEN "%s%s slot %u (%s):" COLOR_RESET "\n",
			chunk->shader_name, chunk->elem_name, i, list_note);

		switch (chunk->element_dw_size) {
		case 4:
			for (unsigned j = 0; j < 4; j++)
				ac_dump_reg(f, chunk->chip_class,
					    R_008F00_SQ_BUF_RSRC_WORD0 + j*4,
					    gpu_list[j], 0xffffffff);
			break;
		case 8:
			for (unsigned j = 0; j < 8; j++)
				ac_dump_reg(f, chunk->chip_class,
					    sq_img_rsrc_word0 + j*4,
					    gpu_list[j], 0xffffffff);

			fprintf(f, COLOR_CYAN "    Buffer:" COLOR_RESET "\n");
			for (unsigned j = 0; j < 4; j++)
				ac_dump_reg(f, chunk->chip_class,
					    R_008F00_SQ_BUF_RSRC_WORD0 + j*4,
					    gpu_list[4+j], 0xffffffff);
			break;
		case 16:
			for (unsigned j = 0; j < 8; j++)
				ac_dump_reg(f, chunk->chip_class,
					    sq_img_rsrc_word0 + j*4,
					    gpu_list[j], 0xffffffff);

			fprintf(f, COLOR_CYAN "    Buffer:" COLOR_RESET "\n");
			for (unsigned j = 0; j < 4; j++)
				ac_dump_reg(f, chunk->chip_class,
					    R_008F00_SQ_BUF_RSRC_WORD0 + j*4,
					    gpu_list[4+j], 0xffffffff);

			fprintf(f, COLOR_CYAN "    FMASK:" COLOR_RESET "\n");
			for (unsigned j = 0; j < 8; j++)
				ac_dump_reg(f, chunk->chip_class,
					    sq_img_rsrc_word0 + j*4,
					    gpu_list[8+j], 0xffffffff);

			fprintf(f, COLOR_CYAN "    Sampler state:" COLOR_RESET "\n");
			for (unsigned j = 0; j < 4; j++)
				ac_dump_reg(f, chunk->chip_class,
					    R_008F30_SQ_IMG_SAMP_WORD0 + j*4,
					    gpu_list[12+j], 0xffffffff);
			break;
		}

		if (memcmp(gpu_list, cpu_list, chunk->element_dw_size * 4) != 0) {
			fprintf(f, COLOR_RED "!!!!! This slot was corrupted in GPU memory !!!!!"
				COLOR_RESET "\n");
		}

		fprintf(f, "\n");
	}

}

static const struct u_log_chunk_type si_log_chunk_type_descriptor_list = {
	.destroy = si_log_chunk_desc_list_destroy,
	.print = si_log_chunk_desc_list_print,
};

static void si_dump_descriptor_list(struct si_screen *screen,
				    struct si_descriptors *desc,
				    const char *shader_name,
				    const char *elem_name,
				    unsigned element_dw_size,
				    unsigned num_elements,
				    slot_remap_func slot_remap,
				    struct u_log_context *log)
{
	if (!desc->list)
		return;

	/* In some cases, the caller doesn't know how many elements are really
	 * uploaded. Reduce num_elements to fit in the range of active slots. */
	unsigned active_range_dw_begin =
		desc->first_active_slot * desc->element_dw_size;
	unsigned active_range_dw_end =
		active_range_dw_begin + desc->num_active_slots * desc->element_dw_size;

	while (num_elements > 0) {
		int i = slot_remap(num_elements - 1);
		unsigned dw_begin = i * element_dw_size;
		unsigned dw_end = dw_begin + element_dw_size;

		if (dw_begin >= active_range_dw_begin && dw_end <= active_range_dw_end)
			break;

		num_elements--;
	}

	struct si_log_chunk_desc_list *chunk =
		CALLOC_VARIANT_LENGTH_STRUCT(si_log_chunk_desc_list,
					     4 * element_dw_size * num_elements);
	chunk->shader_name = shader_name;
	chunk->elem_name = elem_name;
	chunk->element_dw_size = element_dw_size;
	chunk->num_elements = num_elements;
	chunk->slot_remap = slot_remap;
	chunk->chip_class = screen->info.chip_class;

	si_resource_reference(&chunk->buf, desc->buffer);
	chunk->gpu_list = desc->gpu_list;

	for (unsigned i = 0; i < num_elements; ++i) {
		memcpy(&chunk->list[i * element_dw_size],
		       &desc->list[slot_remap(i) * element_dw_size],
		       4 * element_dw_size);
	}

	u_log_chunk(log, &si_log_chunk_type_descriptor_list, chunk);
}

static unsigned si_identity(unsigned slot)
{
	return slot;
}

static void si_dump_descriptors(struct si_context *sctx,
				enum pipe_shader_type processor,
				const struct tgsi_shader_info *info,
				struct u_log_context *log)
{
	struct si_descriptors *descs =
		&sctx->descriptors[SI_DESCS_FIRST_SHADER +
				   processor * SI_NUM_SHADER_DESCS];
	static const char *shader_name[] = {"VS", "PS", "GS", "TCS", "TES", "CS"};
	const char *name = shader_name[processor];
	unsigned enabled_constbuf, enabled_shaderbuf, enabled_samplers;
	unsigned enabled_images;

	if (info) {
		enabled_constbuf = info->const_buffers_declared;
		enabled_shaderbuf = info->shader_buffers_declared;
		enabled_samplers = info->samplers_declared;
		enabled_images = info->images_declared;
	} else {
		enabled_constbuf = sctx->const_and_shader_buffers[processor].enabled_mask >>
				   SI_NUM_SHADER_BUFFERS;
		enabled_shaderbuf = sctx->const_and_shader_buffers[processor].enabled_mask &
				    u_bit_consecutive(0, SI_NUM_SHADER_BUFFERS);
		enabled_shaderbuf = util_bitreverse(enabled_shaderbuf) >>
				    (32 - SI_NUM_SHADER_BUFFERS);
		enabled_samplers = sctx->samplers[processor].enabled_mask;
		enabled_images = sctx->images[processor].enabled_mask;
	}

	if (processor == PIPE_SHADER_VERTEX &&
	    sctx->vb_descriptors_buffer &&
	    sctx->vb_descriptors_gpu_list &&
	    sctx->vertex_elements) {
		assert(info); /* only CS may not have an info struct */
		struct si_descriptors desc = {};

		desc.buffer = sctx->vb_descriptors_buffer;
		desc.list = sctx->vb_descriptors_gpu_list;
		desc.gpu_list = sctx->vb_descriptors_gpu_list;
		desc.element_dw_size = 4;
		desc.num_active_slots = sctx->vertex_elements->desc_list_byte_size / 16;

		si_dump_descriptor_list(sctx->screen, &desc, name,
					" - Vertex buffer", 4, info->num_inputs,
					si_identity, log);
	}

	si_dump_descriptor_list(sctx->screen,
				&descs[SI_SHADER_DESCS_CONST_AND_SHADER_BUFFERS],
				name, " - Constant buffer", 4,
				util_last_bit(enabled_constbuf),
				si_get_constbuf_slot, log);
	si_dump_descriptor_list(sctx->screen,
				&descs[SI_SHADER_DESCS_CONST_AND_SHADER_BUFFERS],
				name, " - Shader buffer", 4,
				util_last_bit(enabled_shaderbuf),
				si_get_shaderbuf_slot, log);
	si_dump_descriptor_list(sctx->screen,
				&descs[SI_SHADER_DESCS_SAMPLERS_AND_IMAGES],
				name, " - Sampler", 16,
				util_last_bit(enabled_samplers),
				si_get_sampler_slot, log);
	si_dump_descriptor_list(sctx->screen,
				&descs[SI_SHADER_DESCS_SAMPLERS_AND_IMAGES],
				name, " - Image", 8,
				util_last_bit(enabled_images),
				si_get_image_slot, log);
}

static void si_dump_gfx_descriptors(struct si_context *sctx,
				    const struct si_shader_ctx_state *state,
				    struct u_log_context *log)
{
	if (!state->cso || !state->current)
		return;

	si_dump_descriptors(sctx, state->cso->type, &state->cso->info, log);
}

static void si_dump_compute_descriptors(struct si_context *sctx,
					struct u_log_context *log)
{
	if (!sctx->cs_shader_state.program)
		return;

	si_dump_descriptors(sctx, PIPE_SHADER_COMPUTE, NULL, log);
}

struct si_shader_inst {
	const char *text; /* start of disassembly for this instruction */
	unsigned textlen;
	unsigned size;   /* instruction size = 4 or 8 */
	uint64_t addr; /* instruction address */
};

/**
 * Open the given \p binary as \p rtld_binary and split the contained
 * disassembly string into instructions and add them to the array
 * pointed to by \p instructions, which must be sufficiently large.
 *
 * Labels are considered to be part of the following instruction.
 *
 * The caller must keep \p rtld_binary alive as long as \p instructions are
 * used and then close it afterwards.
 */
static void si_add_split_disasm(struct si_screen *screen,
				struct ac_rtld_binary *rtld_binary,
				struct si_shader_binary *binary,
				uint64_t *addr,
				unsigned *num,
				struct si_shader_inst *instructions,
				enum pipe_shader_type shader_type,
				unsigned wave_size)
{
	if (!ac_rtld_open(rtld_binary, (struct ac_rtld_open_info){
			.info = &screen->info,
			.shader_type = tgsi_processor_to_shader_stage(shader_type),
			.wave_size = wave_size,
			.num_parts = 1,
			.elf_ptrs = &binary->elf_buffer,
			.elf_sizes = &binary->elf_size }))
		return;

	const char *disasm;
	size_t nbytes;
	if (!ac_rtld_get_section_by_name(rtld_binary, ".AMDGPU.disasm",
					 &disasm, &nbytes))
		return;

	const char *end = disasm + nbytes;
	while (disasm < end) {
		const char *semicolon = memchr(disasm, ';', end - disasm);
		if (!semicolon)
			break;

		struct si_shader_inst *inst = &instructions[(*num)++];
		const char *inst_end = memchr(semicolon + 1, '\n', end - semicolon - 1);
		if (!inst_end)
			inst_end = end;

		inst->text = disasm;
		inst->textlen = inst_end - disasm;

		inst->addr = *addr;
		/* More than 16 chars after ";" means the instruction is 8 bytes long. */
		inst->size = inst_end - semicolon > 16 ? 8 : 4;
		*addr += inst->size;

		if (inst_end == end)
			break;
		disasm = inst_end + 1;
	}
}

/* If the shader is being executed, print its asm instructions, and annotate
 * those that are being executed right now with information about waves that
 * execute them. This is most useful during a GPU hang.
 */
static void si_print_annotated_shader(struct si_shader *shader,
				      struct ac_wave_info *waves,
				      unsigned num_waves,
				      FILE *f)
{
	if (!shader)
		return;

	struct si_screen *screen = shader->selector->screen;
	enum pipe_shader_type shader_type = shader->selector->type;
	uint64_t start_addr = shader->bo->gpu_address;
	uint64_t end_addr = start_addr + shader->bo->b.b.width0;
	unsigned i;

	/* See if any wave executes the shader. */
	for (i = 0; i < num_waves; i++) {
		if (start_addr <= waves[i].pc && waves[i].pc <= end_addr)
			break;
	}
	if (i == num_waves)
		return; /* the shader is not being executed */

	/* Remember the first found wave. The waves are sorted according to PC. */
	waves = &waves[i];
	num_waves -= i;

	/* Get the list of instructions.
	 * Buffer size / 4 is the upper bound of the instruction count.
	 */
	unsigned num_inst = 0;
	uint64_t inst_addr = start_addr;
	unsigned wave_size = si_get_shader_wave_size(shader);
	struct ac_rtld_binary rtld_binaries[5] = {};
	struct si_shader_inst *instructions =
		calloc(shader->bo->b.b.width0 / 4, sizeof(struct si_shader_inst));

	if (shader->prolog) {
		si_add_split_disasm(screen, &rtld_binaries[0], &shader->prolog->binary,
				    &inst_addr, &num_inst, instructions, shader_type, wave_size);
	}
	if (shader->previous_stage) {
		si_add_split_disasm(screen, &rtld_binaries[1], &shader->previous_stage->binary,
				    &inst_addr, &num_inst, instructions, shader_type, wave_size);
	}
	if (shader->prolog2) {
		si_add_split_disasm(screen, &rtld_binaries[2], &shader->prolog2->binary,
				    &inst_addr, &num_inst, instructions, shader_type, wave_size);
	}
	si_add_split_disasm(screen, &rtld_binaries[3], &shader->binary,
			    &inst_addr, &num_inst, instructions, shader_type, wave_size);
	if (shader->epilog) {
		si_add_split_disasm(screen, &rtld_binaries[4], &shader->epilog->binary,
				    &inst_addr, &num_inst, instructions, shader_type, wave_size);
	}

	fprintf(f, COLOR_YELLOW "%s - annotated disassembly:" COLOR_RESET "\n",
		si_get_shader_name(shader));

	/* Print instructions with annotations. */
	for (i = 0; i < num_inst; i++) {
		struct si_shader_inst *inst = &instructions[i];

		fprintf(f, "%.*s [PC=0x%"PRIx64", size=%u]\n",
			inst->textlen, inst->text, inst->addr, inst->size);

		/* Print which waves execute the instruction right now. */
		while (num_waves && inst->addr == waves->pc) {
			fprintf(f,
				"          " COLOR_GREEN "^ SE%u SH%u CU%u "
				"SIMD%u WAVE%u  EXEC=%016"PRIx64 "  ",
				waves->se, waves->sh, waves->cu, waves->simd,
				waves->wave, waves->exec);

			if (inst->size == 4) {
				fprintf(f, "INST32=%08X" COLOR_RESET "\n",
					waves->inst_dw0);
			} else {
				fprintf(f, "INST64=%08X %08X" COLOR_RESET "\n",
					waves->inst_dw0, waves->inst_dw1);
			}

			waves->matched = true;
			waves = &waves[1];
			num_waves--;
		}
	}

	fprintf(f, "\n\n");
	free(instructions);
	for (unsigned i = 0; i < ARRAY_SIZE(rtld_binaries); ++i)
		ac_rtld_close(&rtld_binaries[i]);
}

static void si_dump_annotated_shaders(struct si_context *sctx, FILE *f)
{
	struct ac_wave_info waves[AC_MAX_WAVES_PER_CHIP];
	unsigned num_waves = ac_get_wave_info(sctx->chip_class, waves);

	fprintf(f, COLOR_CYAN "The number of active waves = %u" COLOR_RESET
		"\n\n", num_waves);

	si_print_annotated_shader(sctx->vs_shader.current, waves, num_waves, f);
	si_print_annotated_shader(sctx->tcs_shader.current, waves, num_waves, f);
	si_print_annotated_shader(sctx->tes_shader.current, waves, num_waves, f);
	si_print_annotated_shader(sctx->gs_shader.current, waves, num_waves, f);
	si_print_annotated_shader(sctx->ps_shader.current, waves, num_waves, f);

	/* Print waves executing shaders that are not currently bound. */
	unsigned i;
	bool found = false;
	for (i = 0; i < num_waves; i++) {
		if (waves[i].matched)
			continue;

		if (!found) {
			fprintf(f, COLOR_CYAN
				"Waves not executing currently-bound shaders:"
				COLOR_RESET "\n");
			found = true;
		}
		fprintf(f, "    SE%u SH%u CU%u SIMD%u WAVE%u  EXEC=%016"PRIx64
			"  INST=%08X %08X  PC=%"PRIx64"\n",
			waves[i].se, waves[i].sh, waves[i].cu, waves[i].simd,
			waves[i].wave, waves[i].exec, waves[i].inst_dw0,
			waves[i].inst_dw1, waves[i].pc);
	}
	if (found)
		fprintf(f, "\n\n");
}

static void si_dump_command(const char *title, const char *command, FILE *f)
{
	char line[2000];

	FILE *p = popen(command, "r");
	if (!p)
		return;

	fprintf(f, COLOR_YELLOW "%s: " COLOR_RESET "\n", title);
	while (fgets(line, sizeof(line), p))
		fputs(line, f);
	fprintf(f, "\n\n");
	pclose(p);
}

static void si_dump_debug_state(struct pipe_context *ctx, FILE *f,
				unsigned flags)
{
	struct si_context *sctx = (struct si_context*)ctx;

	if (sctx->log)
		u_log_flush(sctx->log);

	if (flags & PIPE_DUMP_DEVICE_STATUS_REGISTERS) {
		si_dump_debug_registers(sctx, f);

		si_dump_annotated_shaders(sctx, f);
		si_dump_command("Active waves (raw data)", "umr -O halt_waves -wa | column -t", f);
		si_dump_command("Wave information", "umr -O halt_waves,bits -wa", f);
	}
}

void si_log_draw_state(struct si_context *sctx, struct u_log_context *log)
{
	struct si_shader_ctx_state *tcs_shader;

	if (!log)
		return;

	tcs_shader = &sctx->tcs_shader;
	if (sctx->tes_shader.cso && !sctx->tcs_shader.cso)
		tcs_shader = &sctx->fixed_func_tcs_shader;

	si_dump_framebuffer(sctx, log);

	si_dump_gfx_shader(sctx, &sctx->vs_shader, log);
	si_dump_gfx_shader(sctx, tcs_shader, log);
	si_dump_gfx_shader(sctx, &sctx->tes_shader, log);
	si_dump_gfx_shader(sctx, &sctx->gs_shader, log);
	si_dump_gfx_shader(sctx, &sctx->ps_shader, log);

	si_dump_descriptor_list(sctx->screen,
				&sctx->descriptors[SI_DESCS_RW_BUFFERS],
				"", "RW buffers", 4,
				sctx->descriptors[SI_DESCS_RW_BUFFERS].num_active_slots,
				si_identity, log);
	si_dump_gfx_descriptors(sctx, &sctx->vs_shader, log);
	si_dump_gfx_descriptors(sctx, tcs_shader, log);
	si_dump_gfx_descriptors(sctx, &sctx->tes_shader, log);
	si_dump_gfx_descriptors(sctx, &sctx->gs_shader, log);
	si_dump_gfx_descriptors(sctx, &sctx->ps_shader, log);
}

void si_log_compute_state(struct si_context *sctx, struct u_log_context *log)
{
	if (!log)
		return;

	si_dump_compute_shader(sctx, log);
	si_dump_compute_descriptors(sctx, log);
}

static void si_dump_dma(struct si_context *sctx,
			struct radeon_saved_cs *saved, FILE *f)
{
	static const char ib_name[] = "sDMA IB";
	unsigned i;

	si_dump_bo_list(sctx, saved, f);

	fprintf(f, "------------------ %s begin ------------------\n", ib_name);

	for (i = 0; i < saved->num_dw; ++i) {
		fprintf(f, " %08x\n", saved->ib[i]);
	}

	fprintf(f, "------------------- %s end -------------------\n", ib_name);
	fprintf(f, "\n");

	fprintf(f, "SDMA Dump Done.\n");
}

void si_check_vm_faults(struct si_context *sctx,
			struct radeon_saved_cs *saved, enum ring_type ring)
{
	struct pipe_screen *screen = sctx->b.screen;
	FILE *f;
	uint64_t addr;
	char cmd_line[4096];

	if (!ac_vm_fault_occured(sctx->chip_class,
				 &sctx->dmesg_timestamp, &addr))
		return;

	f = dd_get_debug_file(false);
	if (!f)
		return;

	fprintf(f, "VM fault report.\n\n");
	if (os_get_command_line(cmd_line, sizeof(cmd_line)))
		fprintf(f, "Command: %s\n", cmd_line);
	fprintf(f, "Driver vendor: %s\n", screen->get_vendor(screen));
	fprintf(f, "Device vendor: %s\n", screen->get_device_vendor(screen));
	fprintf(f, "Device name: %s\n\n", screen->get_name(screen));
	fprintf(f, "Failing VM page: 0x%08"PRIx64"\n\n", addr);

	if (sctx->apitrace_call_number)
		fprintf(f, "Last apitrace call: %u\n\n",
			sctx->apitrace_call_number);

	switch (ring) {
	case RING_GFX: {
		struct u_log_context log;
		u_log_context_init(&log);

		si_log_draw_state(sctx, &log);
		si_log_compute_state(sctx, &log);
		si_log_cs(sctx, &log, true);

		u_log_new_page_print(&log, f);
		u_log_context_destroy(&log);
		break;
	}
	case RING_DMA:
		si_dump_dma(sctx, saved, f);
		break;

	default:
		break;
	}

	fclose(f);

	fprintf(stderr, "Detected a VM fault, exiting...\n");
	exit(0);
}

void si_init_debug_functions(struct si_context *sctx)
{
	sctx->b.dump_debug_state = si_dump_debug_state;

	/* Set the initial dmesg timestamp for this context, so that
	 * only new messages will be checked for VM faults.
	 */
	if (sctx->screen->debug_flags & DBG(CHECK_VM))
		ac_vm_fault_occured(sctx->chip_class,
				    &sctx->dmesg_timestamp, NULL);
}
