/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2012-2013 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#include "tgsi/tgsi_parse.h"
#include "intel_winsys.h"

#include "shader/ilo_shader_internal.h"
#include "ilo_state.h"
#include "ilo_shader.h"

struct ilo_shader_cache {
   struct list_head shaders;
   struct list_head changed;
};

/**
 * Create a shader cache.  A shader cache can manage shaders and upload them
 * to a bo as a whole.
 */
struct ilo_shader_cache *
ilo_shader_cache_create(void)
{
   struct ilo_shader_cache *shc;

   shc = CALLOC_STRUCT(ilo_shader_cache);
   if (!shc)
      return NULL;

   list_inithead(&shc->shaders);
   list_inithead(&shc->changed);

   return shc;
}

/**
 * Destroy a shader cache.
 */
void
ilo_shader_cache_destroy(struct ilo_shader_cache *shc)
{
   FREE(shc);
}

/**
 * Add a shader to the cache.
 */
void
ilo_shader_cache_add(struct ilo_shader_cache *shc,
                     struct ilo_shader_state *shader)
{
   struct ilo_shader *sh;

   shader->cache = shc;
   LIST_FOR_EACH_ENTRY(sh, &shader->variants, list)
      sh->uploaded = false;

   list_add(&shader->list, &shc->changed);
}

/**
 * Remove a shader from the cache.
 */
void
ilo_shader_cache_remove(struct ilo_shader_cache *shc,
                        struct ilo_shader_state *shader)
{
   list_del(&shader->list);
   shader->cache = NULL;
}

/**
 * Notify the cache that a managed shader has changed.
 */
static void
ilo_shader_cache_notify_change(struct ilo_shader_cache *shc,
                               struct ilo_shader_state *shader)
{
   if (shader->cache == shc) {
      list_del(&shader->list);
      list_add(&shader->list, &shc->changed);
   }
}

/**
 * Upload a managed shader to the bo.
 */
static int
ilo_shader_cache_upload_shader(struct ilo_shader_cache *shc,
                               struct ilo_shader_state *shader,
                               struct intel_bo *bo, unsigned offset,
                               bool incremental)
{
   const unsigned base = offset;
   struct ilo_shader *sh;

   LIST_FOR_EACH_ENTRY(sh, &shader->variants, list) {
      int err;

      if (incremental && sh->uploaded)
         continue;

      /* kernels must be aligned to 64-byte */
      offset = align(offset, 64);

      err = intel_bo_pwrite(bo, offset, sh->kernel_size, sh->kernel);
      if (unlikely(err))
         return -1;

      sh->uploaded = true;
      sh->cache_offset = offset;

      offset += sh->kernel_size;
   }

   return (int) (offset - base);
}

/**
 * Similar to ilo_shader_cache_upload(), except no upload happens.
 */
static int
ilo_shader_cache_get_upload_size(struct ilo_shader_cache *shc,
                                 unsigned offset,
                                 bool incremental)
{
   const unsigned base = offset;
   struct ilo_shader_state *shader;

   if (!incremental) {
      LIST_FOR_EACH_ENTRY(shader, &shc->shaders, list) {
         struct ilo_shader *sh;

         /* see ilo_shader_cache_upload_shader() */
         LIST_FOR_EACH_ENTRY(sh, &shader->variants, list) {
            if (!incremental || !sh->uploaded)
               offset = align(offset, 64) + sh->kernel_size;
         }
      }
   }

   LIST_FOR_EACH_ENTRY(shader, &shc->changed, list) {
      struct ilo_shader *sh;

      /* see ilo_shader_cache_upload_shader() */
      LIST_FOR_EACH_ENTRY(sh, &shader->variants, list) {
         if (!incremental || !sh->uploaded)
            offset = align(offset, 64) + sh->kernel_size;
      }
   }

   /*
    * From the Sandy Bridge PRM, volume 4 part 2, page 112:
    *
    *     "Due to prefetch of the instruction stream, the EUs may attempt to
    *      access up to 8 instructions (128 bytes) beyond the end of the
    *      kernel program - possibly into the next memory page.  Although
    *      these instructions will not be executed, software must account for
    *      the prefetch in order to avoid invalid page access faults."
    */
   if (offset > base)
      offset += 128;

   return (int) (offset - base);
}

/**
 * Upload managed shaders to the bo.  When incremental is true, only shaders
 * that are changed or added after the last upload are uploaded.
 */
int
ilo_shader_cache_upload(struct ilo_shader_cache *shc,
                        struct intel_bo *bo, unsigned offset,
                        bool incremental)
{
   struct ilo_shader_state *shader, *next;
   int size = 0, s;

   if (!bo)
      return ilo_shader_cache_get_upload_size(shc, offset, incremental);

   if (!incremental) {
      LIST_FOR_EACH_ENTRY(shader, &shc->shaders, list) {
         s = ilo_shader_cache_upload_shader(shc, shader,
               bo, offset, incremental);
         if (unlikely(s < 0))
            return s;

         size += s;
         offset += s;
      }
   }

   LIST_FOR_EACH_ENTRY_SAFE(shader, next, &shc->changed, list) {
      s = ilo_shader_cache_upload_shader(shc, shader,
            bo, offset, incremental);
      if (unlikely(s < 0))
         return s;

      size += s;
      offset += s;

      list_del(&shader->list);
      list_add(&shader->list, &shc->shaders);
   }

   return size;
}

/**
 * Initialize a shader variant.
 */
void
ilo_shader_variant_init(struct ilo_shader_variant *variant,
                        const struct ilo_shader_info *info,
                        const struct ilo_context *ilo)
{
   int num_views, i;

   memset(variant, 0, sizeof(*variant));

   switch (info->type) {
   case PIPE_SHADER_VERTEX:
      variant->u.vs.rasterizer_discard =
         ilo->rasterizer->state.rasterizer_discard;
      variant->u.vs.num_ucps =
         util_last_bit(ilo->rasterizer->state.clip_plane_enable);
      break;
   case PIPE_SHADER_GEOMETRY:
      variant->u.gs.rasterizer_discard =
         ilo->rasterizer->state.rasterizer_discard;
      variant->u.gs.num_inputs = ilo->vs->shader->out.count;
      for (i = 0; i < ilo->vs->shader->out.count; i++) {
         variant->u.gs.semantic_names[i] =
            ilo->vs->shader->out.semantic_names[i];
         variant->u.gs.semantic_indices[i] =
            ilo->vs->shader->out.semantic_indices[i];
      }
      break;
   case PIPE_SHADER_FRAGMENT:
      variant->u.fs.flatshade =
         (info->has_color_interp && ilo->rasterizer->state.flatshade);
      variant->u.fs.fb_height = (info->has_pos) ?
         ilo->fb.state.height : 1;
      variant->u.fs.num_cbufs = ilo->fb.state.nr_cbufs;
      break;
   default:
      assert(!"unknown shader type");
      break;
   }

   num_views = ilo->view[info->type].count;
   assert(info->num_samplers <= num_views);

   variant->num_sampler_views = info->num_samplers;
   for (i = 0; i < info->num_samplers; i++) {
      const struct pipe_sampler_view *view = ilo->view[info->type].states[i];
      const struct ilo_sampler_cso *sampler = ilo->sampler[info->type].cso[i];

      if (view) {
         variant->sampler_view_swizzles[i].r = view->swizzle_r;
         variant->sampler_view_swizzles[i].g = view->swizzle_g;
         variant->sampler_view_swizzles[i].b = view->swizzle_b;
         variant->sampler_view_swizzles[i].a = view->swizzle_a;
      }
      else if (info->shadow_samplers & (1 << i)) {
         variant->sampler_view_swizzles[i].r = PIPE_SWIZZLE_RED;
         variant->sampler_view_swizzles[i].g = PIPE_SWIZZLE_RED;
         variant->sampler_view_swizzles[i].b = PIPE_SWIZZLE_RED;
         variant->sampler_view_swizzles[i].a = PIPE_SWIZZLE_ONE;
      }
      else {
         variant->sampler_view_swizzles[i].r = PIPE_SWIZZLE_RED;
         variant->sampler_view_swizzles[i].g = PIPE_SWIZZLE_GREEN;
         variant->sampler_view_swizzles[i].b = PIPE_SWIZZLE_BLUE;
         variant->sampler_view_swizzles[i].a = PIPE_SWIZZLE_ALPHA;
      }

      /*
       * When non-nearest filter and PIPE_TEX_WRAP_CLAMP wrap mode is used,
       * the HW wrap mode is set to BRW_TEXCOORDMODE_CLAMP_BORDER, and we need
       * to manually saturate the texture coordinates.
       */
      if (sampler) {
         variant->saturate_tex_coords[0] |= sampler->saturate_s << i;
         variant->saturate_tex_coords[1] |= sampler->saturate_t << i;
         variant->saturate_tex_coords[2] |= sampler->saturate_r << i;
      }
   }
}

/**
 * Guess the shader variant, knowing that the context may still change.
 */
static void
ilo_shader_variant_guess(struct ilo_shader_variant *variant,
                         const struct ilo_shader_info *info,
                         const struct ilo_context *ilo)
{
   int i;

   memset(variant, 0, sizeof(*variant));

   switch (info->type) {
   case PIPE_SHADER_VERTEX:
      break;
   case PIPE_SHADER_GEOMETRY:
      break;
   case PIPE_SHADER_FRAGMENT:
      variant->u.fs.flatshade = false;
      variant->u.fs.fb_height = (info->has_pos) ?
         ilo->fb.state.height : 1;
      variant->u.fs.num_cbufs = 1;
      break;
   default:
      assert(!"unknown shader type");
      break;
   }

   variant->num_sampler_views = info->num_samplers;
   for (i = 0; i < info->num_samplers; i++) {
      if (info->shadow_samplers & (1 << i)) {
         variant->sampler_view_swizzles[i].r = PIPE_SWIZZLE_RED;
         variant->sampler_view_swizzles[i].g = PIPE_SWIZZLE_RED;
         variant->sampler_view_swizzles[i].b = PIPE_SWIZZLE_RED;
         variant->sampler_view_swizzles[i].a = PIPE_SWIZZLE_ONE;
      }
      else {
         variant->sampler_view_swizzles[i].r = PIPE_SWIZZLE_RED;
         variant->sampler_view_swizzles[i].g = PIPE_SWIZZLE_GREEN;
         variant->sampler_view_swizzles[i].b = PIPE_SWIZZLE_BLUE;
         variant->sampler_view_swizzles[i].a = PIPE_SWIZZLE_ALPHA;
      }
   }
}


/**
 * Parse a TGSI instruction for the shader info.
 */
static void
ilo_shader_info_parse_inst(struct ilo_shader_info *info,
                           const struct tgsi_full_instruction *inst)
{
   int i;

   /* look for edgeflag passthrough */
   if (info->edgeflag_out >= 0 &&
       inst->Instruction.Opcode == TGSI_OPCODE_MOV &&
       inst->Dst[0].Register.File == TGSI_FILE_OUTPUT &&
       inst->Dst[0].Register.Index == info->edgeflag_out) {

      assert(inst->Src[0].Register.File == TGSI_FILE_INPUT);
      info->edgeflag_in = inst->Src[0].Register.Index;
   }

   if (inst->Instruction.Texture) {
      bool shadow;

      switch (inst->Texture.Texture) {
      case TGSI_TEXTURE_SHADOW1D:
      case TGSI_TEXTURE_SHADOW2D:
      case TGSI_TEXTURE_SHADOWRECT:
      case TGSI_TEXTURE_SHADOW1D_ARRAY:
      case TGSI_TEXTURE_SHADOW2D_ARRAY:
      case TGSI_TEXTURE_SHADOWCUBE:
      case TGSI_TEXTURE_SHADOWCUBE_ARRAY:
         shadow = true;
         break;
      default:
         shadow = false;
         break;
      }

      for (i = 0; i < inst->Instruction.NumSrcRegs; i++) {
         const struct tgsi_full_src_register *src = &inst->Src[i];

         if (src->Register.File == TGSI_FILE_SAMPLER) {
            const int idx = src->Register.Index;

            if (idx >= info->num_samplers)
               info->num_samplers = idx + 1;

            if (shadow)
               info->shadow_samplers |= 1 << idx;
         }
      }
   }
}

/**
 * Parse a TGSI property for the shader info.
 */
static void
ilo_shader_info_parse_prop(struct ilo_shader_info *info,
                           const struct tgsi_full_property *prop)
{
   switch (prop->Property.PropertyName) {
   case TGSI_PROPERTY_FS_COLOR0_WRITES_ALL_CBUFS:
      info->fs_color0_writes_all_cbufs = prop->u[0].Data;
      break;
   default:
      break;
   }
}

/**
 * Parse a TGSI declaration for the shader info.
 */
static void
ilo_shader_info_parse_decl(struct ilo_shader_info *info,
                           const struct tgsi_full_declaration *decl)
{
   switch (decl->Declaration.File) {
   case TGSI_FILE_INPUT:
      if (decl->Declaration.Interpolate &&
          decl->Interp.Interpolate == TGSI_INTERPOLATE_COLOR)
         info->has_color_interp = true;
      if (decl->Declaration.Semantic &&
          decl->Semantic.Name == TGSI_SEMANTIC_POSITION)
         info->has_pos = true;
      break;
   case TGSI_FILE_OUTPUT:
      if (decl->Declaration.Semantic &&
          decl->Semantic.Name == TGSI_SEMANTIC_EDGEFLAG)
         info->edgeflag_out = decl->Range.First;
      break;
   case TGSI_FILE_SYSTEM_VALUE:
      if (decl->Declaration.Semantic &&
          decl->Semantic.Name == TGSI_SEMANTIC_INSTANCEID)
         info->has_instanceid = true;
      if (decl->Declaration.Semantic &&
          decl->Semantic.Name == TGSI_SEMANTIC_VERTEXID)
         info->has_vertexid = true;
      break;
   default:
      break;
   }
}

static void
ilo_shader_info_parse_tokens(struct ilo_shader_info *info)
{
   struct tgsi_parse_context parse;

   info->edgeflag_in = -1;
   info->edgeflag_out = -1;

   tgsi_parse_init(&parse, info->tokens);
   while (!tgsi_parse_end_of_tokens(&parse)) {
      const union tgsi_full_token *token;

      tgsi_parse_token(&parse);
      token = &parse.FullToken;

      switch (token->Token.Type) {
      case TGSI_TOKEN_TYPE_DECLARATION:
         ilo_shader_info_parse_decl(info, &token->FullDeclaration);
         break;
      case TGSI_TOKEN_TYPE_INSTRUCTION:
         ilo_shader_info_parse_inst(info, &token->FullInstruction);
         break;
      case TGSI_TOKEN_TYPE_PROPERTY:
         ilo_shader_info_parse_prop(info, &token->FullProperty);
         break;
      default:
         break;
      }
   }
   tgsi_parse_free(&parse);
}

/**
 * Create a shader state.
 */
static struct ilo_shader_state *
ilo_shader_state_create(const struct ilo_context *ilo,
                        int type, const void *templ)
{
   struct ilo_shader_state *state;
   struct ilo_shader_variant variant;

   state = CALLOC_STRUCT(ilo_shader_state);
   if (!state)
      return NULL;

   state->info.dev = ilo->dev;
   state->info.type = type;

   if (type == PIPE_SHADER_COMPUTE) {
      const struct pipe_compute_state *c =
         (const struct pipe_compute_state *) templ;

      state->info.tokens = tgsi_dup_tokens(c->prog);
      state->info.compute.req_local_mem = c->req_local_mem;
      state->info.compute.req_private_mem = c->req_private_mem;
      state->info.compute.req_input_mem = c->req_input_mem;
   }
   else {
      const struct pipe_shader_state *s =
         (const struct pipe_shader_state *) templ;

      state->info.tokens = tgsi_dup_tokens(s->tokens);
      state->info.stream_output = s->stream_output;
   }

   list_inithead(&state->variants);

   ilo_shader_info_parse_tokens(&state->info);

   /* guess and compile now */
   ilo_shader_variant_guess(&variant, &state->info, ilo);
   if (!ilo_shader_state_use_variant(state, &variant)) {
      ilo_shader_destroy(state);
      return NULL;
   }

   return state;
}

/**
 * Add a compiled shader to the shader state.
 */
static void
ilo_shader_state_add_shader(struct ilo_shader_state *state,
                            struct ilo_shader *sh)
{
   list_add(&sh->list, &state->variants);
   state->num_variants++;
   state->total_size += sh->kernel_size;

   if (state->cache)
      ilo_shader_cache_notify_change(state->cache, state);
}

/**
 * Remove a compiled shader from the shader state.
 */
static void
ilo_shader_state_remove_shader(struct ilo_shader_state *state,
                               struct ilo_shader *sh)
{
   list_del(&sh->list);
   state->num_variants--;
   state->total_size -= sh->kernel_size;
}

/**
 * Garbage collect shader variants in the shader state.
 */
static void
ilo_shader_state_gc(struct ilo_shader_state *state)
{
   /* activate when the variants take up more than 4KiB of space */
   const int limit = 4 * 1024;
   struct ilo_shader *sh, *next;

   if (state->total_size < limit)
      return;

   /* remove from the tail as the most recently ones are at the head */
   LIST_FOR_EACH_ENTRY_SAFE_REV(sh, next, &state->variants, list) {
      ilo_shader_state_remove_shader(state, sh);
      ilo_shader_destroy_kernel(sh);

      if (state->total_size <= limit / 2)
         break;
   }
}

/**
 * Search for a shader variant.
 */
static struct ilo_shader *
ilo_shader_state_search_variant(struct ilo_shader_state *state,
                                const struct ilo_shader_variant *variant)
{
   struct ilo_shader *sh = NULL, *tmp;

   LIST_FOR_EACH_ENTRY(tmp, &state->variants, list) {
      if (memcmp(&tmp->variant, variant, sizeof(*variant)) == 0) {
         sh = tmp;
         break;
      }
   }

   return sh;
}

static void
copy_so_info(struct ilo_shader *sh,
             const struct pipe_stream_output_info *so_info)
{
   unsigned i, attr;

   if (!so_info->num_outputs)
      return;

   sh->so_info = *so_info;

   for (i = 0; i < so_info->num_outputs; i++) {
      /* figure out which attribute is sourced */
      for (attr = 0; attr < sh->out.count; attr++) {
         const int reg_idx = sh->out.register_indices[attr];
         if (reg_idx == so_info->output[i].register_index)
            break;
      }

      if (attr < sh->out.count) {
         sh->so_info.output[i].register_index = attr;
      }
      else {
         assert(!"stream output an undefined register");
         sh->so_info.output[i].register_index = 0;
      }

      /* PSIZE is at W channel */
      if (sh->out.semantic_names[attr] == TGSI_SEMANTIC_PSIZE) {
         assert(so_info->output[i].start_component == 0);
         assert(so_info->output[i].num_components == 1);
         sh->so_info.output[i].start_component = 3;
      }
   }
}

/**
 * Add a shader variant to the shader state.
 */
static struct ilo_shader *
ilo_shader_state_add_variant(struct ilo_shader_state *state,
                             const struct ilo_shader_variant *variant)
{
   struct ilo_shader *sh;

   switch (state->info.type) {
   case PIPE_SHADER_VERTEX:
      sh = ilo_shader_compile_vs(state, variant);
      break;
   case PIPE_SHADER_FRAGMENT:
      sh = ilo_shader_compile_fs(state, variant);
      break;
   case PIPE_SHADER_GEOMETRY:
      sh = ilo_shader_compile_gs(state, variant);
      break;
   case PIPE_SHADER_COMPUTE:
      sh = ilo_shader_compile_cs(state, variant);
      break;
   default:
      sh = NULL;
      break;
   }
   if (!sh) {
      assert(!"failed to compile shader");
      return NULL;
   }

   sh->variant = *variant;

   copy_so_info(sh, &state->info.stream_output);

   ilo_shader_state_add_shader(state, sh);

   return sh;
}

/**
 * Update state->shader to point to a variant.  If the variant does not exist,
 * it will be added first.
 */
bool
ilo_shader_state_use_variant(struct ilo_shader_state *state,
                             const struct ilo_shader_variant *variant)
{
   struct ilo_shader *sh;
   bool construct_cso = false;

   sh = ilo_shader_state_search_variant(state, variant);
   if (!sh) {
      ilo_shader_state_gc(state);

      sh = ilo_shader_state_add_variant(state, variant);
      if (!sh)
         return false;

      construct_cso = true;
   }

   /* move to head */
   if (state->variants.next != &sh->list) {
      list_del(&sh->list);
      list_add(&sh->list, &state->variants);
   }

   state->shader = sh;

   if (construct_cso) {
      switch (state->info.type) {
      case PIPE_SHADER_VERTEX:
         ilo_gpe_init_vs_cso(state->info.dev, state, &sh->cso);
         break;
      case PIPE_SHADER_GEOMETRY:
         ilo_gpe_init_gs_cso(state->info.dev, state, &sh->cso);
         break;
      case PIPE_SHADER_FRAGMENT:
         ilo_gpe_init_fs_cso(state->info.dev, state, &sh->cso);
         break;
      default:
         break;
      }
   }

   return true;
}

struct ilo_shader_state *
ilo_shader_create_vs(const struct ilo_dev_info *dev,
                     const struct pipe_shader_state *state,
                     const struct ilo_context *precompile)
{
   struct ilo_shader_state *shader;

   shader = ilo_shader_state_create(precompile, PIPE_SHADER_VERTEX, state);

   /* states used in ilo_shader_variant_init() */
   shader->info.non_orthogonal_states = ILO_DIRTY_VERTEX_SAMPLER_VIEWS |
                                        ILO_DIRTY_RASTERIZER;

   return shader;
}

struct ilo_shader_state *
ilo_shader_create_gs(const struct ilo_dev_info *dev,
                     const struct pipe_shader_state *state,
                     const struct ilo_context *precompile)
{
   struct ilo_shader_state *shader;

   shader = ilo_shader_state_create(precompile, PIPE_SHADER_GEOMETRY, state);

   /* states used in ilo_shader_variant_init() */
   shader->info.non_orthogonal_states = ILO_DIRTY_GEOMETRY_SAMPLER_VIEWS |
                                        ILO_DIRTY_VS |
                                        ILO_DIRTY_RASTERIZER;

   return shader;
}

struct ilo_shader_state *
ilo_shader_create_fs(const struct ilo_dev_info *dev,
                     const struct pipe_shader_state *state,
                     const struct ilo_context *precompile)
{
   struct ilo_shader_state *shader;

   shader = ilo_shader_state_create(precompile, PIPE_SHADER_FRAGMENT, state);

   /* states used in ilo_shader_variant_init() */
   shader->info.non_orthogonal_states = ILO_DIRTY_FRAGMENT_SAMPLER_VIEWS |
                                        ILO_DIRTY_RASTERIZER |
                                        ILO_DIRTY_FRAMEBUFFER;

   return shader;
}

struct ilo_shader_state *
ilo_shader_create_cs(const struct ilo_dev_info *dev,
                     const struct pipe_compute_state *state,
                     const struct ilo_context *precompile)
{
   struct ilo_shader_state *shader;

   shader = ilo_shader_state_create(precompile, PIPE_SHADER_COMPUTE, state);

   shader->info.non_orthogonal_states = 0;

   return shader;
}

/**
 * Destroy a shader state.
 */
void
ilo_shader_destroy(struct ilo_shader_state *shader)
{
   struct ilo_shader *sh, *next;

   LIST_FOR_EACH_ENTRY_SAFE(sh, next, &shader->variants, list)
      ilo_shader_destroy_kernel(sh);

   FREE((struct tgsi_token *) shader->info.tokens);
   FREE(shader);
}

/**
 * Return the type (PIPE_SHADER_x) of the shader.
 */
int
ilo_shader_get_type(const struct ilo_shader_state *shader)
{
   return shader->info.type;
}

/**
 * Select a kernel for the given context.  This will compile a new kernel if
 * none of the existing kernels work with the context.
 *
 * \param ilo the context
 * \param dirty states of the context that are considered changed
 * \return true if a different kernel is selected
 */
bool
ilo_shader_select_kernel(struct ilo_shader_state *shader,
                         const struct ilo_context *ilo,
                         uint32_t dirty)
{
   const struct ilo_shader * const cur = shader->shader;
   struct ilo_shader_variant variant;

   if (!(shader->info.non_orthogonal_states & dirty))
      return false;

   ilo_shader_variant_init(&variant, &shader->info, ilo);
   ilo_shader_state_use_variant(shader, &variant);

   return (shader->shader != cur);
}

/**
 * Return the cache offset of the selected kernel.  This must be called after
 * ilo_shader_select_kernel() and ilo_shader_cache_upload().
 */
uint32_t
ilo_shader_get_kernel_offset(const struct ilo_shader_state *shader)
{
   const struct ilo_shader *kernel = shader->shader;

   assert(kernel && kernel->uploaded);

   return kernel->cache_offset;
}

/**
 * Query a kernel parameter for the selected kernel.
 */
int
ilo_shader_get_kernel_param(const struct ilo_shader_state *shader,
                            enum ilo_kernel_param param)
{
   const struct ilo_shader *kernel = shader->shader;
   int val;

   assert(kernel);

   switch (param) {
   case ILO_KERNEL_INPUT_COUNT:
      val = kernel->in.count;
      break;
   case ILO_KERNEL_OUTPUT_COUNT:
      val = kernel->out.count;
      break;
   case ILO_KERNEL_URB_DATA_START_REG:
      val = kernel->in.start_grf;
      break;

   case ILO_KERNEL_VS_INPUT_INSTANCEID:
      val = shader->info.has_instanceid;
      break;
   case ILO_KERNEL_VS_INPUT_VERTEXID:
      val = shader->info.has_vertexid;
      break;
   case ILO_KERNEL_VS_INPUT_EDGEFLAG:
      if (shader->info.edgeflag_in >= 0) {
         /* we rely on the state tracker here */
         assert(shader->info.edgeflag_in == kernel->in.count - 1);
         val = true;
      }
      else {
         val = false;
      }
      break;
   case ILO_KERNEL_VS_PCB_UCP_SIZE:
      val = kernel->pcb.clip_state_size;
      break;
   case ILO_KERNEL_VS_GEN6_SO:
      val = kernel->stream_output;
      break;
   case ILO_KERNEL_VS_GEN6_SO_START_REG:
      val = kernel->gs_start_grf;
      break;
   case ILO_KERNEL_VS_GEN6_SO_POINT_OFFSET:
      val = kernel->gs_offsets[0];
      break;
   case ILO_KERNEL_VS_GEN6_SO_LINE_OFFSET:
      val = kernel->gs_offsets[1];
      break;
   case ILO_KERNEL_VS_GEN6_SO_TRI_OFFSET:
      val = kernel->gs_offsets[2];
      break;

   case ILO_KERNEL_GS_DISCARD_ADJACENCY:
      val = kernel->in.discard_adj;
      break;
   case ILO_KERNEL_GS_GEN6_SVBI_POST_INC:
      val = kernel->svbi_post_inc;
      break;

   case ILO_KERNEL_FS_INPUT_Z:
   case ILO_KERNEL_FS_INPUT_W:
      val = kernel->in.has_pos;
      break;
   case ILO_KERNEL_FS_OUTPUT_Z:
      val = kernel->out.has_pos;
      break;
   case ILO_KERNEL_FS_USE_KILL:
      val = kernel->has_kill;
      break;
   case ILO_KERNEL_FS_BARYCENTRIC_INTERPOLATIONS:
      val = kernel->in.barycentric_interpolation_mode;
      break;
   case ILO_KERNEL_FS_DISPATCH_16_OFFSET:
      val = 0;
      break;

   default:
      assert(!"unknown kernel parameter");
      val = 0;
      break;
   }

   return val;
}

/**
 * Return the CSO of the selected kernel.
 */
const struct ilo_shader_cso *
ilo_shader_get_kernel_cso(const struct ilo_shader_state *shader)
{
   const struct ilo_shader *kernel = shader->shader;

   assert(kernel);

   return &kernel->cso;
}

/**
 * Return the SO info of the selected kernel.
 */
const struct pipe_stream_output_info *
ilo_shader_get_kernel_so_info(const struct ilo_shader_state *shader)
{
   const struct ilo_shader *kernel = shader->shader;

   assert(kernel);

   return &kernel->so_info;
}