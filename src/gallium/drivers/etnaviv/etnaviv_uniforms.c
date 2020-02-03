/*
 * Copyright (c) 2016 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include "etnaviv_uniforms.h"

#include "etnaviv_compiler.h"
#include "etnaviv_context.h"
#include "etnaviv_util.h"
#include "etnaviv_emit.h"
#include "pipe/p_defines.h"
#include "util/u_math.h"

static unsigned
get_const_idx(const struct etna_context *ctx, bool frag, unsigned samp_id)
{
   if (frag)
      return samp_id;

   return samp_id + ctx->specs.vertex_sampler_offset;
}

static uint32_t
get_texrect_scale(const struct etna_context *ctx, bool frag,
                  enum etna_immediate_contents contents, uint32_t data)
{
   unsigned index = get_const_idx(ctx, frag, data);
   struct pipe_sampler_view *texture = ctx->sampler_view[index];
   uint32_t dim;

   if (contents == ETNA_IMMEDIATE_TEXRECT_SCALE_X)
      dim = texture->texture->width0;
   else
      dim = texture->texture->height0;

   return fui(1.0f / dim);
}

void
etna_uniforms_write(const struct etna_context *ctx,
                    const struct etna_shader_variant *sobj,
                    struct pipe_constant_buffer *cb)
{
   struct etna_cmd_stream *stream = ctx->stream;
   const struct etna_shader_uniform_info *uinfo = &sobj->uniforms;
   bool frag = (sobj == ctx->shader.fs);
   uint32_t base = frag ? ctx->specs.ps_uniforms_offset : ctx->specs.vs_uniforms_offset;

   if (!uinfo->imm_count)
      return;

   etna_cmd_stream_reserve(stream, align(uinfo->imm_count + 1, 2));
   etna_emit_load_state(stream, base >> 2, uinfo->imm_count, 0);

   for (uint32_t i = 0; i < uinfo->imm_count; i++) {
      uint32_t val = uinfo->imm_data[i];

      switch (uinfo->imm_contents[i]) {
      case ETNA_IMMEDIATE_CONSTANT:
         etna_cmd_stream_emit(stream, val);
         break;

      case ETNA_IMMEDIATE_UNIFORM:
         assert(cb->user_buffer && val * 4 < cb->buffer_size);
         etna_cmd_stream_emit(stream, ((uint32_t*) cb->user_buffer)[val]);
         break;

      case ETNA_IMMEDIATE_TEXRECT_SCALE_X:
      case ETNA_IMMEDIATE_TEXRECT_SCALE_Y:
         etna_cmd_stream_emit(stream,
            get_texrect_scale(ctx, frag, uinfo->imm_contents[i], val));
         break;

      case ETNA_IMMEDIATE_UBO0_ADDR ... ETNA_IMMEDIATE_UBOMAX_ADDR:
         assert(uinfo->imm_contents[i] == ETNA_IMMEDIATE_UBO0_ADDR);
         etna_cmd_stream_reloc(stream, &(struct etna_reloc) {
            .bo = etna_resource(cb->buffer)->bo,
            .flags = ETNA_RELOC_READ,
            .offset = cb->buffer_offset + val,
         });
         break;

      case ETNA_IMMEDIATE_UNUSED:
         etna_cmd_stream_emit(stream, 0);
         break;
      }
   }

   if ((uinfo->imm_count % 2) == 0)
      etna_cmd_stream_emit(stream, 0);
}

void
etna_set_shader_uniforms_dirty_flags(struct etna_shader_variant *sobj)
{
   uint32_t dirty = 0;

   for (uint32_t i = 0; i < sobj->uniforms.imm_count; i++) {
      switch (sobj->uniforms.imm_contents[i]) {
      default:
         break;

      case ETNA_IMMEDIATE_TEXRECT_SCALE_X:
      case ETNA_IMMEDIATE_TEXRECT_SCALE_Y:
         dirty |= ETNA_DIRTY_SAMPLER_VIEWS;
         break;
      }
   }

   sobj->uniforms_dirty_bits = dirty;
}
