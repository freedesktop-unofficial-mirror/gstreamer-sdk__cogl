/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2008,2009,2010 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 *
 *
 * Authors:
 *   Robert Bragg <robert@linux.intel.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl.h"
#include "cogl-debug.h"
#include "cogl-internal.h"
#include "cogl-context-private.h"
#include "cogl-object.h"

#include "cogl-pipeline-private.h"
#include "cogl-pipeline-opengl-private.h"
#include "cogl-pipeline-state-private.h"
#include "cogl-pipeline-layer-state-private.h"
#include "cogl-texture-private.h"
#include "cogl-blend-string.h"
#include "cogl-journal-private.h"
#include "cogl-color-private.h"
#include "cogl-util.h"
#include "cogl-profile.h"
#include "cogl-depth-state-private.h"

#include <glib.h>
#include <glib/gprintf.h>
#include <string.h>

static CoglPipelineLayer *_cogl_pipeline_layer_copy (CoglPipelineLayer *layer);

static void _cogl_pipeline_free (CoglPipeline *tex);
static void _cogl_pipeline_layer_free (CoglPipelineLayer *layer);
static void _cogl_pipeline_add_layer_difference (CoglPipeline *pipeline,
                                                 CoglPipelineLayer *layer,
                                                 gboolean inc_n_layers);
static void recursively_free_layer_caches (CoglPipeline *pipeline);
static gboolean _cogl_pipeline_is_weak (CoglPipeline *pipeline);

const CoglPipelineFragend *_cogl_pipeline_fragends[COGL_PIPELINE_N_FRAGENDS];
const CoglPipelineVertend *_cogl_pipeline_vertends[COGL_PIPELINE_N_VERTENDS];
/* The 'MAX' here is so that we don't define an empty array when there
   are no progends */
const CoglPipelineProgend *
_cogl_pipeline_progends[MAX (COGL_PIPELINE_N_PROGENDS, 1)];

#ifdef COGL_PIPELINE_FRAGEND_GLSL
#include "cogl-pipeline-fragend-glsl-private.h"
#endif
#ifdef COGL_PIPELINE_FRAGEND_ARBFP
#include "cogl-pipeline-fragend-arbfp-private.h"
#endif
#ifdef COGL_PIPELINE_FRAGEND_FIXED
#include "cogl-pipeline-fragend-fixed-private.h"
#endif
#ifdef COGL_PIPELINE_PROGEND_GLSL
#include "cogl-pipeline-progend-glsl-private.h"
#endif

#ifdef COGL_PIPELINE_VERTEND_GLSL
#include "cogl-pipeline-vertend-glsl-private.h"
#endif
#ifdef COGL_PIPELINE_VERTEND_FIXED
#include "cogl-pipeline-vertend-fixed-private.h"
#endif

COGL_OBJECT_DEFINE (Pipeline, pipeline);
/* This type was made deprecated before the cogl_is_pipeline_layer
   function was ever exposed in the public headers so there's no need
   to make the cogl_is_pipeline_layer function public. We use INTERNAL
   so that the cogl_is_* function won't get defined */
COGL_OBJECT_INTERNAL_DEFINE (PipelineLayer, pipeline_layer);

GQuark
_cogl_pipeline_error_quark (void)
{
  return g_quark_from_static_string ("cogl-pipeline-error-quark");
}

static void
_cogl_pipeline_node_init (CoglPipelineNode *node)
{
  node->parent = NULL;
  COGL_LIST_INIT (&node->children);
}

static void
_cogl_pipeline_node_set_parent_real (CoglPipelineNode *node,
                                     CoglPipelineNode *parent,
                                     CoglPipelineNodeUnparentVFunc unparent,
                                     gboolean take_strong_reference)
{
  /* NB: the old parent may indirectly be keeping the new parent alive
   * so we have to ref the new parent before unrefing the old.
   *
   * Note: we take a reference here regardless of
   * take_strong_reference because weak children may need special
   * handling when the parent disposes itself which relies on a
   * consistent link to all weak nodes. Once the node is linked to its
   * parent then we remove the reference at the end if
   * take_strong_reference == FALSE. */
  cogl_object_ref (parent);

  if (node->parent)
    unparent (node);

  COGL_LIST_INSERT_HEAD (&parent->children, node, list_node);

  node->parent = parent;
  node->has_parent_reference = take_strong_reference;

  /* Now that there is a consistent parent->child link we can remove
   * the parent reference if no reference was requested. If it turns
   * out that the new parent was only being kept alive by the old
   * parent then it will be disposed of here. */
  if (!take_strong_reference)
    cogl_object_unref (parent);
}

static void
_cogl_pipeline_node_unparent_real (CoglPipelineNode *node)
{
  CoglPipelineNode *parent = node->parent;

  if (parent == NULL)
    return;

  g_return_if_fail (!COGL_LIST_EMPTY (&parent->children));

  COGL_LIST_REMOVE (node, list_node);

  if (node->has_parent_reference)
    cogl_object_unref (parent);

  node->parent = NULL;
}

void
_cogl_pipeline_node_foreach_child (CoglPipelineNode *node,
                                   CoglPipelineNodeChildCallback callback,
                                   void *user_data)
{
  CoglPipelineNode *child, *next;

  COGL_LIST_FOREACH_SAFE (child, &node->children, list_node, next)
    callback (child, user_data);
}

/*
 * This initializes the first pipeline owned by the Cogl context. All
 * subsequently instantiated pipelines created via the cogl_pipeline_new()
 * API will initially be a copy of this pipeline.
 *
 * The default pipeline is the topmost ancester for all pipelines.
 */
void
_cogl_pipeline_init_default_pipeline (void)
{
  /* Create new - blank - pipeline */
  CoglPipeline *pipeline = g_slice_new0 (CoglPipeline);
  /* XXX: NB: It's important that we zero this to avoid polluting
   * pipeline hash values with un-initialized data */
  CoglPipelineBigState *big_state = g_slice_new0 (CoglPipelineBigState);
  CoglPipelineLightingState *lighting_state = &big_state->lighting_state;
  CoglPipelineAlphaFuncState *alpha_state = &big_state->alpha_state;
  CoglPipelineBlendState *blend_state = &big_state->blend_state;
  CoglDepthState *depth_state = &big_state->depth_state;
  CoglPipelineLogicOpsState *logic_ops_state = &big_state->logic_ops_state;
  CoglPipelineCullFaceState *cull_face_state = &big_state->cull_face_state;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  /* Take this opportunity to setup the backends... */
#ifdef COGL_PIPELINE_FRAGEND_GLSL
  _cogl_pipeline_fragends[COGL_PIPELINE_FRAGEND_GLSL] =
    &_cogl_pipeline_glsl_fragend;
#endif
#ifdef COGL_PIPELINE_FRAGEND_ARBFP
  _cogl_pipeline_fragends[COGL_PIPELINE_FRAGEND_ARBFP] =
    &_cogl_pipeline_arbfp_fragend;
#endif
#ifdef COGL_PIPELINE_FRAGEND_FIXED
  _cogl_pipeline_fragends[COGL_PIPELINE_FRAGEND_FIXED] =
    &_cogl_pipeline_fixed_fragend;
#endif
#ifdef COGL_PIPELINE_PROGEND_GLSL
  _cogl_pipeline_progends[COGL_PIPELINE_PROGEND_GLSL] =
    &_cogl_pipeline_glsl_progend;
#endif

#ifdef COGL_PIPELINE_VERTEND_GLSL
  _cogl_pipeline_vertends[COGL_PIPELINE_VERTEND_GLSL] =
    &_cogl_pipeline_glsl_vertend;
#endif
#ifdef COGL_PIPELINE_VERTEND_FIXED
  _cogl_pipeline_vertends[COGL_PIPELINE_VERTEND_FIXED] =
    &_cogl_pipeline_fixed_vertend;
#endif

  _cogl_pipeline_node_init (COGL_PIPELINE_NODE (pipeline));

  pipeline->is_weak = FALSE;
  pipeline->journal_ref_count = 0;
  pipeline->fragend = COGL_PIPELINE_FRAGEND_UNDEFINED;
  pipeline->vertend = COGL_PIPELINE_VERTEND_UNDEFINED;
  pipeline->differences = COGL_PIPELINE_STATE_ALL_SPARSE;

  pipeline->real_blend_enable = FALSE;

  pipeline->blend_enable = COGL_PIPELINE_BLEND_ENABLE_AUTOMATIC;
  pipeline->layer_differences = NULL;
  pipeline->n_layers = 0;

  pipeline->big_state = big_state;
  pipeline->has_big_state = TRUE;

  pipeline->static_breadcrumb = "default pipeline";
  pipeline->has_static_breadcrumb = TRUE;

  pipeline->age = 0;

  /* Use the same defaults as the GL spec... */
  cogl_color_init_from_4ub (&pipeline->color, 0xff, 0xff, 0xff, 0xff);

  /* Use the same defaults as the GL spec... */
  lighting_state->ambient[0] = 0.2;
  lighting_state->ambient[1] = 0.2;
  lighting_state->ambient[2] = 0.2;
  lighting_state->ambient[3] = 1.0;

  lighting_state->diffuse[0] = 0.8;
  lighting_state->diffuse[1] = 0.8;
  lighting_state->diffuse[2] = 0.8;
  lighting_state->diffuse[3] = 1.0;

  lighting_state->specular[0] = 0;
  lighting_state->specular[1] = 0;
  lighting_state->specular[2] = 0;
  lighting_state->specular[3] = 1.0;

  lighting_state->emission[0] = 0;
  lighting_state->emission[1] = 0;
  lighting_state->emission[2] = 0;
  lighting_state->emission[3] = 1.0;

  lighting_state->shininess = 0.0f;

  /* Use the same defaults as the GL spec... */
  alpha_state->alpha_func = COGL_PIPELINE_ALPHA_FUNC_ALWAYS;
  alpha_state->alpha_func_reference = 0.0;

  /* Not the same as the GL default, but seems saner... */
#if defined(HAVE_COGL_GLES2) || defined(HAVE_COGL_GL)
  blend_state->blend_equation_rgb = GL_FUNC_ADD;
  blend_state->blend_equation_alpha = GL_FUNC_ADD;
  blend_state->blend_src_factor_alpha = GL_ONE;
  blend_state->blend_dst_factor_alpha = GL_ONE_MINUS_SRC_ALPHA;
  cogl_color_init_from_4ub (&blend_state->blend_constant,
                            0x00, 0x00, 0x00, 0x00);
#endif
  blend_state->blend_src_factor_rgb = GL_ONE;
  blend_state->blend_dst_factor_rgb = GL_ONE_MINUS_SRC_ALPHA;

  big_state->user_program = COGL_INVALID_HANDLE;

  /* The same as the GL defaults */
  depth_state->test_enabled = FALSE;
  depth_state->test_function = COGL_DEPTH_TEST_FUNCTION_LESS;
  depth_state->write_enabled = TRUE;
  depth_state->range_near = 0;
  depth_state->range_far = 1;

  big_state->point_size = 1.0f;

  logic_ops_state->color_mask = COGL_COLOR_MASK_ALL;

  cull_face_state->mode = COGL_PIPELINE_CULL_FACE_MODE_NONE;
  cull_face_state->front_winding = COGL_WINDING_COUNTER_CLOCKWISE;

  ctx->default_pipeline = _cogl_pipeline_object_new (pipeline);
}

static void
_cogl_pipeline_unparent (CoglPipelineNode *pipeline)
{
  /* Chain up */
  _cogl_pipeline_node_unparent_real (pipeline);
}

static gboolean
recursively_free_layer_caches_cb (CoglPipelineNode *node,
                                  void *user_data)
{
  recursively_free_layer_caches (COGL_PIPELINE (node));
  return TRUE;
}

/* This recursively frees the layers_cache of a pipeline and all of
 * its descendants.
 *
 * For instance if we change a pipelines ->layer_differences list
 * then that pipeline and all of its descendants may now have
 * incorrect layer caches. */
static void
recursively_free_layer_caches (CoglPipeline *pipeline)
{
  /* Note: we maintain the invariable that if a pipeline already has a
   * dirty layers_cache then so do all of its descendants. */
  if (pipeline->layers_cache_dirty)
    return;

  if (G_UNLIKELY (pipeline->layers_cache != pipeline->short_layers_cache))
    g_slice_free1 (sizeof (CoglPipelineLayer *) * pipeline->n_layers,
                   pipeline->layers_cache);
  pipeline->layers_cache_dirty = TRUE;

  _cogl_pipeline_node_foreach_child (COGL_PIPELINE_NODE (pipeline),
                                     recursively_free_layer_caches_cb,
                                     NULL);
}

static void
_cogl_pipeline_set_parent (CoglPipeline *pipeline,
                           CoglPipeline *parent,
                           gboolean take_strong_reference)
{
  /* Chain up */
  _cogl_pipeline_node_set_parent_real (COGL_PIPELINE_NODE (pipeline),
                                       COGL_PIPELINE_NODE (parent),
                                       _cogl_pipeline_unparent,
                                       take_strong_reference);

  /* Since we just changed the ancestry of the pipeline its cache of
   * layers could now be invalid so free it... */
  if (pipeline->differences & COGL_PIPELINE_STATE_LAYERS)
    recursively_free_layer_caches (pipeline);

  /* If the backends are also caching state along with the pipeline
   * that depends on the pipeline's ancestry then it may be notified
   * here...
   */
  if (pipeline->fragend != COGL_PIPELINE_FRAGEND_UNDEFINED &&
      _cogl_pipeline_fragends[pipeline->fragend]->pipeline_set_parent_notify)
    {
      const CoglPipelineFragend *fragend =
        _cogl_pipeline_fragends[pipeline->fragend];
      fragend->pipeline_set_parent_notify (pipeline);
    }
}

static void
_cogl_pipeline_promote_weak_ancestors (CoglPipeline *strong)
{
  CoglPipelineNode *n;

  g_return_if_fail (!strong->is_weak);

  /* If the parent of strong is weak, then we want to promote it by
     taking a reference on strong's grandparent. We don't need to take
     a reference on strong's direct parent */

  if (COGL_PIPELINE_NODE (strong)->parent == NULL)
    return;

  for (n = COGL_PIPELINE_NODE (strong)->parent;
       /* We can assume that all weak pipelines have a parent */
       COGL_PIPELINE (n)->is_weak;
       n = n->parent)
    /* 'n' is weak so we take a reference on its parent */
    cogl_object_ref (n->parent);
}

static void
_cogl_pipeline_revert_weak_ancestors (CoglPipeline *strong)
{
  CoglPipelineNode *n;

  g_return_if_fail (!strong->is_weak);

  /* This reverts the effect of calling
     _cogl_pipeline_promote_weak_ancestors */

  if (COGL_PIPELINE_NODE (strong)->parent == NULL)
    return;

  for (n = COGL_PIPELINE_NODE (strong)->parent;
       /* We can assume that all weak pipelines have a parent */
       COGL_PIPELINE (n)->is_weak;
       n = n->parent)
    /* 'n' is weak so we unref its parent */
    cogl_object_unref (n->parent);
}

/* XXX: Always have an eye out for opportunities to lower the cost of
 * cogl_pipeline_copy. */
static CoglPipeline *
_cogl_pipeline_copy (CoglPipeline *src, gboolean is_weak)
{
  CoglPipeline *pipeline = g_slice_new (CoglPipeline);

  _cogl_pipeline_node_init (COGL_PIPELINE_NODE (pipeline));

  pipeline->is_weak = is_weak;

  pipeline->journal_ref_count = 0;

  pipeline->differences = 0;

  pipeline->has_big_state = FALSE;

  /* NB: real_blend_enable isn't a sparse property, it's valid for
   * every pipeline node so we have fast access to it. */
  pipeline->real_blend_enable = src->real_blend_enable;

  /* XXX:
   * consider generalizing the idea of "cached" properties. These
   * would still have an authority like other sparse properties but
   * you wouldn't have to walk up the ancestry to find the authority
   * because the value would be cached directly in each pipeline.
   */

  pipeline->layers_cache_dirty = TRUE;
  pipeline->deprecated_get_layers_list = NULL;
  pipeline->deprecated_get_layers_list_dirty = TRUE;

  pipeline->fragend = src->fragend;

  pipeline->vertend = src->vertend;

  pipeline->has_static_breadcrumb = FALSE;

  pipeline->age = 0;

  _cogl_pipeline_set_parent (pipeline, src, !is_weak);

  /* The semantics for copying a weak pipeline are that we promote all
   * weak ancestors to temporarily become strong pipelines until the
   * copy is freed. */
  if (!is_weak)
    _cogl_pipeline_promote_weak_ancestors (pipeline);

  return _cogl_pipeline_object_new (pipeline);
}

CoglPipeline *
cogl_pipeline_copy (CoglPipeline *src)
{
  return _cogl_pipeline_copy (src, FALSE);
}

CoglPipeline *
_cogl_pipeline_weak_copy (CoglPipeline *pipeline,
                          CoglPipelineDestroyCallback callback,
                          void *user_data)
{
  CoglPipeline *copy;
  CoglPipeline *copy_pipeline;

  copy = _cogl_pipeline_copy (pipeline, TRUE);
  copy_pipeline = COGL_PIPELINE (copy);
  copy_pipeline->destroy_callback = callback;
  copy_pipeline->destroy_data = user_data;

  return copy;
}

CoglPipeline *
cogl_pipeline_new (void)
{
  CoglPipeline *new;

  _COGL_GET_CONTEXT (ctx, NULL);

  new = cogl_pipeline_copy (ctx->default_pipeline);
  _cogl_pipeline_set_static_breadcrumb (new, "new");
  return new;
}

static gboolean
destroy_weak_children_cb (CoglPipelineNode *node,
                          void *user_data)
{
  CoglPipeline *pipeline = COGL_PIPELINE (node);

  if (_cogl_pipeline_is_weak (pipeline))
    {
      _cogl_pipeline_node_foreach_child (COGL_PIPELINE_NODE (pipeline),
                                         destroy_weak_children_cb,
                                         NULL);

      pipeline->destroy_callback (pipeline, pipeline->destroy_data);
      _cogl_pipeline_unparent (COGL_PIPELINE_NODE (pipeline));
    }

  return TRUE;
}

static void
_cogl_pipeline_free (CoglPipeline *pipeline)
{
  if (!pipeline->is_weak)
    _cogl_pipeline_revert_weak_ancestors (pipeline);

  /* Weak pipelines don't take a reference on their parent */
  _cogl_pipeline_node_foreach_child (COGL_PIPELINE_NODE (pipeline),
                                     destroy_weak_children_cb,
                                     NULL);

  g_assert (COGL_LIST_EMPTY (&COGL_PIPELINE_NODE (pipeline)->children));

  _cogl_pipeline_unparent (COGL_PIPELINE_NODE (pipeline));

  if (pipeline->differences & COGL_PIPELINE_STATE_USER_SHADER &&
      pipeline->big_state->user_program)
    cogl_handle_unref (pipeline->big_state->user_program);

  if (pipeline->differences & COGL_PIPELINE_STATE_NEEDS_BIG_STATE)
    g_slice_free (CoglPipelineBigState, pipeline->big_state);

  if (pipeline->differences & COGL_PIPELINE_STATE_LAYERS)
    {
      g_list_foreach (pipeline->layer_differences,
                      (GFunc)cogl_object_unref, NULL);
      g_list_free (pipeline->layer_differences);
    }

  g_list_free (pipeline->deprecated_get_layers_list);

  recursively_free_layer_caches (pipeline);

  g_slice_free (CoglPipeline, pipeline);
}

gboolean
_cogl_pipeline_get_real_blend_enabled (CoglPipeline *pipeline)
{
  g_return_val_if_fail (cogl_is_pipeline (pipeline), FALSE);

  return pipeline->real_blend_enable;
}

CoglPipelineLayer *
_cogl_pipeline_layer_get_authority (CoglPipelineLayer *layer,
                                    unsigned long difference)
{
  CoglPipelineLayer *authority = layer;
  while (!(authority->differences & difference))
    authority = _cogl_pipeline_layer_get_parent (authority);
  return authority;
}

int
_cogl_pipeline_layer_get_unit_index (CoglPipelineLayer *layer)
{
  CoglPipelineLayer *authority =
    _cogl_pipeline_layer_get_authority (layer, COGL_PIPELINE_LAYER_STATE_UNIT);
  return authority->unit_index;
}

static void
_cogl_pipeline_update_layers_cache (CoglPipeline *pipeline)
{
  /* Note: we assume this pipeline is a _LAYERS authority */
  int n_layers;
  CoglPipeline *current;
  int layers_found;

  if (G_LIKELY (!pipeline->layers_cache_dirty) ||
      pipeline->n_layers == 0)
    return;

  pipeline->layers_cache_dirty = FALSE;

  n_layers = pipeline->n_layers;
  if (G_LIKELY (n_layers < G_N_ELEMENTS (pipeline->short_layers_cache)))
    {
      pipeline->layers_cache = pipeline->short_layers_cache;
      memset (pipeline->layers_cache, 0,
              sizeof (CoglPipelineLayer *) *
              G_N_ELEMENTS (pipeline->short_layers_cache));
    }
  else
    {
      pipeline->layers_cache =
        g_slice_alloc0 (sizeof (CoglPipelineLayer *) * n_layers);
    }

  /* Notes:
   *
   * Each pipeline doesn't have to contain a complete list of the layers
   * it depends on, some of them are indirectly referenced through the
   * pipeline's ancestors.
   *
   * pipeline->layer_differences only contains a list of layers that
   * have changed in relation to its parent.
   *
   * pipeline->layer_differences is not maintained sorted, but it
   * won't contain multiple layers corresponding to a particular
   * ->unit_index.
   *
   * Some of the ancestor pipelines may reference layers with
   * ->unit_index values >= n_layers so we ignore them.
   *
   * As we ascend through the ancestors we are searching for any
   * CoglPipelineLayers corresponding to the texture ->unit_index
   * values in the range [0,n_layers-1]. As soon as a pointer is found
   * we ignore layers of further ancestors with the same ->unit_index
   * values.
   */

  layers_found = 0;
  for (current = pipeline;
       _cogl_pipeline_get_parent (current);
       current = _cogl_pipeline_get_parent (current))
    {
      GList *l;

      if (!(current->differences & COGL_PIPELINE_STATE_LAYERS))
        continue;

      for (l = current->layer_differences; l; l = l->next)
        {
          CoglPipelineLayer *layer = l->data;
          int unit_index = _cogl_pipeline_layer_get_unit_index (layer);

          if (unit_index < n_layers && !pipeline->layers_cache[unit_index])
            {
              pipeline->layers_cache[unit_index] = layer;
              layers_found++;
              if (layers_found == n_layers)
                return;
            }
        }
    }

  g_warn_if_reached ();
}

/* XXX: Be carefull when using this API that the callback given doesn't result
 * in the layer cache being invalidated during the iteration! */
void
_cogl_pipeline_foreach_layer_internal (CoglPipeline *pipeline,
                                       CoglPipelineInternalLayerCallback callback,
                                       void *user_data)
{
  CoglPipeline *authority =
    _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_LAYERS);
  int n_layers;
  int i;
  gboolean cont;

  n_layers = authority->n_layers;
  if (n_layers == 0)
    return;

  _cogl_pipeline_update_layers_cache (authority);

  for (i = 0, cont = TRUE; i < n_layers && cont == TRUE; i++)
    {
      g_return_if_fail (authority->layers_cache_dirty == FALSE);
      cont = callback (authority->layers_cache[i], user_data);
    }
}

typedef struct
{
  int i;
  int *indices;
} AppendLayerIndexState;

static gboolean
append_layer_index_cb (CoglPipelineLayer *layer,
                       void *user_data)
{
  AppendLayerIndexState *state = user_data;
  state->indices[state->i++] = layer->index;
  return TRUE;
}

void
cogl_pipeline_foreach_layer (CoglPipeline *pipeline,
                             CoglPipelineLayerCallback callback,
                             void *user_data)
{
  CoglPipeline *authority =
    _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_LAYERS);
  AppendLayerIndexState state;
  gboolean cont;
  int i;

  /* XXX: We don't know what the user is going to want to do to the layers
   * but any modification of layers can result in the layer graph changing
   * which could confuse _cogl_pipeline_foreach_layer_internal(). We first
   * get a list of layer indices which will remain valid so long as the
   * user doesn't remove layers. */

  state.i = 0;
  state.indices = g_alloca (authority->n_layers * sizeof (int));

  _cogl_pipeline_foreach_layer_internal (pipeline,
                                         append_layer_index_cb,
                                         &state);

  for (i = 0, cont = TRUE; i < authority->n_layers && cont; i++)
    cont = callback (pipeline, state.indices[i], user_data);
}

static gboolean
layer_has_alpha_cb (CoglPipelineLayer *layer, void *data)
{
  CoglPipelineLayer *combine_authority =
    _cogl_pipeline_layer_get_authority (layer,
                                        COGL_PIPELINE_LAYER_STATE_COMBINE);
  CoglPipelineLayerBigState *big_state = combine_authority->big_state;
  CoglPipelineLayer *tex_authority;
  gboolean *has_alpha = data;

  /* has_alpha maintains the alpha status for the GL_PREVIOUS layer */

  /* For anything but the default texture combine we currently just
   * assume it may result in an alpha value < 1
   *
   * FIXME: we could do better than this. */
  if (big_state->texture_combine_alpha_func !=
      COGL_PIPELINE_COMBINE_FUNC_MODULATE ||
      big_state->texture_combine_alpha_src[0] !=
      COGL_PIPELINE_COMBINE_SOURCE_PREVIOUS ||
      big_state->texture_combine_alpha_op[0] !=
      COGL_PIPELINE_COMBINE_OP_SRC_ALPHA ||
      big_state->texture_combine_alpha_src[1] !=
      COGL_PIPELINE_COMBINE_SOURCE_TEXTURE ||
      big_state->texture_combine_alpha_op[1] !=
      COGL_PIPELINE_COMBINE_OP_SRC_ALPHA)
    {
      *has_alpha = TRUE;
      /* return FALSE to stop iterating layers... */
      return FALSE;
    }

  /* NB: A layer may have a combine mode set on it but not yet
   * have an associated texture which would mean we'd fallback
   * to the default texture which doesn't have an alpha component
   */
  tex_authority =
    _cogl_pipeline_layer_get_authority (layer,
                                        COGL_PIPELINE_LAYER_STATE_TEXTURE_DATA);
  if (tex_authority->texture &&
      cogl_texture_get_format (tex_authority->texture) & COGL_A_BIT)
    {
      *has_alpha = TRUE;
      /* return FALSE to stop iterating layers... */
      return FALSE;
    }

  *has_alpha = FALSE;
  /* return FALSE to continue iterating layers... */
  return TRUE;
}

static gboolean
_cogl_pipeline_needs_blending_enabled (CoglPipeline    *pipeline,
                                       unsigned long    changes,
                                       const CoglColor *override_color)
{
  CoglPipeline *enable_authority;
  CoglPipeline *blend_authority;
  CoglPipelineBlendState *blend_state;
  CoglPipelineBlendEnable enabled;
  unsigned long other_state;

  _COGL_GET_CONTEXT (ctx, FALSE);

  if (G_UNLIKELY (COGL_DEBUG_ENABLED (COGL_DEBUG_DISABLE_BLENDING)))
    return FALSE;

  enable_authority =
    _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_BLEND_ENABLE);

  enabled = enable_authority->blend_enable;
  if (enabled != COGL_PIPELINE_BLEND_ENABLE_AUTOMATIC)
    return enabled == COGL_PIPELINE_BLEND_ENABLE_ENABLED ? TRUE : FALSE;

  blend_authority =
    _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_BLEND);

  blend_state = &blend_authority->big_state->blend_state;

  /* We are trying to identify awkward cases that are equivalent to
   * blending being disable, where the output is simply GL_SRC_COLOR.
   *
   * Note: we assume that all OpenGL drivers will identify the simple
   * case of ADD (ONE, ZERO) as equivalent to blending being disabled.
   *
   * We should update this when we add support for more blend
   * functions...
   */

#if defined (HAVE_COGL_GLES2) || defined (HAVE_COGL_GL)
  if (ctx->driver != COGL_DRIVER_GLES1)
    {
      /* GLES 1 can't change the function or have separate alpha factors */
      if (blend_state->blend_equation_rgb != GL_FUNC_ADD ||
          blend_state->blend_equation_alpha != GL_FUNC_ADD)
        return TRUE;

      if (blend_state->blend_src_factor_alpha != GL_ONE ||
          blend_state->blend_dst_factor_alpha != GL_ONE_MINUS_SRC_ALPHA)
        return TRUE;
    }
#endif

  if (blend_state->blend_src_factor_rgb != GL_ONE ||
      blend_state->blend_dst_factor_rgb != GL_ONE_MINUS_SRC_ALPHA)
    return TRUE;

  /* Given the above constraints, it's now a case of finding any
   * SRC_ALPHA that != 1 */

  /* In the case of a layer state change we need to check everything
   * else first since they contribute to the has_alpha status of the
   * GL_PREVIOUS layer. */
  if (changes & COGL_PIPELINE_STATE_LAYERS)
    changes = COGL_PIPELINE_STATE_AFFECTS_BLENDING;

  if ((override_color && cogl_color_get_alpha_byte (override_color) != 0xff))
    return TRUE;

  if (changes & COGL_PIPELINE_STATE_COLOR)
    {
      CoglColor tmp;
      cogl_pipeline_get_color (pipeline, &tmp);
      if (cogl_color_get_alpha_byte (&tmp) != 0xff)
        return TRUE;
    }

  if (changes & COGL_PIPELINE_STATE_USER_SHADER)
    {
      /* We can't make any assumptions about the alpha channel if the user
       * is using an unknown fragment shader.
       *
       * TODO: check that it isn't just a vertex shader!
       */
      if (_cogl_pipeline_get_user_program (pipeline) != COGL_INVALID_HANDLE)
        return TRUE;
    }

  /* XXX: we should only need to look at these if lighting is enabled
   */
  if (changes & COGL_PIPELINE_STATE_LIGHTING)
    {
      /* XXX: This stuff is showing up in sysprof reports which is
       * silly because lighting isn't currently actually supported
       * by Cogl except for these token properties. When we actually
       * expose lighting support we can avoid these checks when
       * lighting is disabled. */
#if 0
      CoglColor tmp;
      cogl_pipeline_get_ambient (pipeline, &tmp);
      if (cogl_color_get_alpha_byte (&tmp) != 0xff)
        return TRUE;
      cogl_pipeline_get_diffuse (pipeline, &tmp);
      if (cogl_color_get_alpha_byte (&tmp) != 0xff)
        return TRUE;
      cogl_pipeline_get_specular (pipeline, &tmp);
      if (cogl_color_get_alpha_byte (&tmp) != 0xff)
        return TRUE;
      cogl_pipeline_get_emission (pipeline, &tmp);
      if (cogl_color_get_alpha_byte (&tmp) != 0xff)
        return TRUE;
#endif
    }

  if (changes & COGL_PIPELINE_STATE_LAYERS)
    {
      /* has_alpha tracks the alpha status of the GL_PREVIOUS layer.
       * To start with that's defined by the pipeline color which
       * must be fully opaque if we got this far. */
      gboolean has_alpha = FALSE;
      _cogl_pipeline_foreach_layer_internal (pipeline,
                                             layer_has_alpha_cb,
                                             &has_alpha);
      if (has_alpha)
        return TRUE;
    }

  /* At this point, considering just the state that has changed it
   * looks like blending isn't needed. If blending was previously
   * enabled though it could be that some other state still requires
   * that we have blending enabled. In this case we still need to
   * go and check the other state...
   *
   * FIXME: We should explicitly keep track of the mask of state
   * groups that are currently causing blending to be enabled so that
   * we never have to resort to checking *all* the state and can
   * instead always limit the check to those in the mask.
   */
  if (pipeline->real_blend_enable)
    {
      other_state = COGL_PIPELINE_STATE_AFFECTS_BLENDING & ~changes;
      if (other_state &&
          _cogl_pipeline_needs_blending_enabled (pipeline,
                                                 other_state,
                                                 NULL))
        return TRUE;
    }

  return FALSE;
}

void
_cogl_pipeline_set_fragend (CoglPipeline *pipeline, int fragend)
{
  pipeline->fragend = fragend;
}

void
_cogl_pipeline_set_vertend (CoglPipeline *pipeline, int vertend)
{
  pipeline->vertend = vertend;
}

static void
_cogl_pipeline_copy_differences (CoglPipeline *dest,
                                 CoglPipeline *src,
                                 unsigned long differences)
{
  CoglPipelineBigState *big_state;

  if (differences & COGL_PIPELINE_STATE_COLOR)
    dest->color = src->color;

  if (differences & COGL_PIPELINE_STATE_BLEND_ENABLE)
    dest->blend_enable = src->blend_enable;

  if (differences & COGL_PIPELINE_STATE_LAYERS)
    {
      GList *l;

      if (dest->differences & COGL_PIPELINE_STATE_LAYERS &&
          dest->layer_differences)
        {
          g_list_foreach (dest->layer_differences,
                          (GFunc)cogl_object_unref,
                          NULL);
          g_list_free (dest->layer_differences);
        }

      for (l = src->layer_differences; l; l = l->next)
        {
          /* NB: a layer can't have more than one ->owner so we can't
           * simply take a references on each of the original
           * layer_differences, we have to derive new layers from the
           * originals instead. */
          CoglPipelineLayer *copy = _cogl_pipeline_layer_copy (l->data);
          _cogl_pipeline_add_layer_difference (dest, copy, FALSE);
          cogl_object_unref (copy);
        }

      /* Note: we initialize n_layers after adding the layer differences
       * since the act of adding the layers will initialize n_layers to 0
       * because dest isn't initially a STATE_LAYERS authority. */
      dest->n_layers = src->n_layers;
    }

  if (differences & COGL_PIPELINE_STATE_NEEDS_BIG_STATE)
    {
      if (!dest->has_big_state)
        {
          dest->big_state = g_slice_new (CoglPipelineBigState);
          dest->has_big_state = TRUE;
        }
      big_state = dest->big_state;
    }
  else
    goto check_for_blending_change;

  if (differences & COGL_PIPELINE_STATE_LIGHTING)
    {
      memcpy (&big_state->lighting_state,
              &src->big_state->lighting_state,
              sizeof (CoglPipelineLightingState));
    }

  if (differences & COGL_PIPELINE_STATE_ALPHA_FUNC)
    big_state->alpha_state.alpha_func =
      src->big_state->alpha_state.alpha_func;

  if (differences & COGL_PIPELINE_STATE_ALPHA_FUNC_REFERENCE)
    big_state->alpha_state.alpha_func_reference =
      src->big_state->alpha_state.alpha_func_reference;

  if (differences & COGL_PIPELINE_STATE_BLEND)
    {
      memcpy (&big_state->blend_state,
              &src->big_state->blend_state,
              sizeof (CoglPipelineBlendState));
    }

  if (differences & COGL_PIPELINE_STATE_USER_SHADER)
    {
      if (src->big_state->user_program)
        big_state->user_program =
          cogl_handle_ref (src->big_state->user_program);
      else
        big_state->user_program = COGL_INVALID_HANDLE;
    }

  if (differences & COGL_PIPELINE_STATE_DEPTH)
    {
      memcpy (&big_state->depth_state,
              &src->big_state->depth_state,
              sizeof (CoglDepthState));
    }

  if (differences & COGL_PIPELINE_STATE_FOG)
    {
      memcpy (&big_state->fog_state,
              &src->big_state->fog_state,
              sizeof (CoglPipelineFogState));
    }

  if (differences & COGL_PIPELINE_STATE_POINT_SIZE)
    big_state->point_size = src->big_state->point_size;

  if (differences & COGL_PIPELINE_STATE_LOGIC_OPS)
    {
      memcpy (&big_state->logic_ops_state,
              &src->big_state->logic_ops_state,
              sizeof (CoglPipelineLogicOpsState));
    }

  if (differences & COGL_PIPELINE_STATE_CULL_FACE)
    {
      memcpy (&big_state->cull_face_state,
              &src->big_state->cull_face_state,
              sizeof (CoglPipelineCullFaceState));
    }

  /* XXX: we shouldn't bother doing this in most cases since
   * _copy_differences is typically used to initialize pipeline state
   * by copying it from the current authority, so it's not actually
   * *changing* anything.
   */
check_for_blending_change:
  if (differences & COGL_PIPELINE_STATE_AFFECTS_BLENDING)
    _cogl_pipeline_update_blend_enable (dest, differences);

  dest->differences |= differences;
}

static void
_cogl_pipeline_init_multi_property_sparse_state (CoglPipeline *pipeline,
                                                 CoglPipelineState change)
{
  CoglPipeline *authority;

  g_return_if_fail (change & COGL_PIPELINE_STATE_ALL_SPARSE);

  if (!(change & COGL_PIPELINE_STATE_MULTI_PROPERTY))
    return;

  authority = _cogl_pipeline_get_authority (pipeline, change);

  switch (change)
    {
    /* XXX: avoid using a default: label so we get a warning if we
     * don't explicitly handle a newly defined state-group here. */
    case COGL_PIPELINE_STATE_COLOR:
    case COGL_PIPELINE_STATE_BLEND_ENABLE:
    case COGL_PIPELINE_STATE_ALPHA_FUNC:
    case COGL_PIPELINE_STATE_ALPHA_FUNC_REFERENCE:
    case COGL_PIPELINE_STATE_POINT_SIZE:
    case COGL_PIPELINE_STATE_USER_SHADER:
    case COGL_PIPELINE_STATE_REAL_BLEND_ENABLE:
      g_return_if_reached ();

    case COGL_PIPELINE_STATE_LAYERS:
      pipeline->n_layers = authority->n_layers;
      pipeline->layer_differences = NULL;
      break;
    case COGL_PIPELINE_STATE_LIGHTING:
      {
        memcpy (&pipeline->big_state->lighting_state,
                &authority->big_state->lighting_state,
                sizeof (CoglPipelineLightingState));
        break;
      }
    case COGL_PIPELINE_STATE_BLEND:
      {
        memcpy (&pipeline->big_state->blend_state,
                &authority->big_state->blend_state,
                sizeof (CoglPipelineBlendState));
        break;
      }
    case COGL_PIPELINE_STATE_DEPTH:
      {
        memcpy (&pipeline->big_state->depth_state,
                &authority->big_state->depth_state,
                sizeof (CoglDepthState));
        break;
      }
    case COGL_PIPELINE_STATE_FOG:
      {
        memcpy (&pipeline->big_state->fog_state,
                &authority->big_state->fog_state,
                sizeof (CoglPipelineFogState));
        break;
      }
    case COGL_PIPELINE_STATE_LOGIC_OPS:
      {
        memcpy (&pipeline->big_state->logic_ops_state,
                &authority->big_state->logic_ops_state,
                sizeof (CoglPipelineLogicOpsState));
        break;
      }
    case COGL_PIPELINE_STATE_CULL_FACE:
      {
        memcpy (&pipeline->big_state->cull_face_state,
                &authority->big_state->cull_face_state,
                sizeof (CoglPipelineCullFaceState));
        break;
      }
    }
}

static gboolean
check_if_strong_cb (CoglPipelineNode *node, void *user_data)
{
  CoglPipeline *pipeline = COGL_PIPELINE (node);
  gboolean *has_strong_child = user_data;

  if (!_cogl_pipeline_is_weak (pipeline))
    {
      *has_strong_child = TRUE;
      return FALSE;
    }

  return TRUE;
}

static gboolean
has_strong_children (CoglPipeline *pipeline)
{
  gboolean has_strong_child = FALSE;
  _cogl_pipeline_node_foreach_child (COGL_PIPELINE_NODE (pipeline),
                                     check_if_strong_cb,
                                     &has_strong_child);
  return has_strong_child;
}

static gboolean
_cogl_pipeline_is_weak (CoglPipeline *pipeline)
{
  if (pipeline->is_weak && !has_strong_children (pipeline))
    return TRUE;
  else
    return FALSE;
}

static gboolean
reparent_children_cb (CoglPipelineNode *node,
                      void *user_data)
{
  CoglPipeline *pipeline = COGL_PIPELINE (node);
  CoglPipeline *parent = user_data;

  _cogl_pipeline_set_parent (pipeline, parent, TRUE);

  return TRUE;
}

void
_cogl_pipeline_pre_change_notify (CoglPipeline     *pipeline,
                                  CoglPipelineState change,
                                  const CoglColor  *new_color,
                                  gboolean          from_layer_change)
{
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  /* If primitives have been logged in the journal referencing the
   * current state of this pipeline we need to flush the journal
   * before we can modify it... */
  if (pipeline->journal_ref_count)
    {
      gboolean skip_journal_flush = FALSE;

      /* XXX: We don't usually need to flush the journal just due to
       * color changes since pipeline colors are logged in the
       * journal's vertex buffer. The exception is when the change in
       * color enables or disables the need for blending. */
      if (change == COGL_PIPELINE_STATE_COLOR)
        {
          gboolean will_need_blending =
            _cogl_pipeline_needs_blending_enabled (pipeline,
                                                   change,
                                                   new_color);
          gboolean blend_enable = pipeline->real_blend_enable ? TRUE : FALSE;

          if (will_need_blending == blend_enable)
            skip_journal_flush = TRUE;
        }

      if (!skip_journal_flush)
        {
          /* XXX: note we use cogl_flush() not _cogl_flush_journal() so
           * we will flush *all* known journals that might reference the
           * current pipeline. */
          cogl_flush ();
        }
    }

  /* The fixed function backend has no private state and can't
   * do anything special to handle small pipeline changes so we may as
   * well try to find a better backend whenever the pipeline changes.
   *
   * The programmable backends may be able to cache a lot of the code
   * they generate and only need to update a small section of that
   * code in response to a pipeline change therefore we don't want to
   * try searching for another backend when the pipeline changes.
   */
#ifdef COGL_PIPELINE_FRAGEND_FIXED
  if (pipeline->fragend == COGL_PIPELINE_FRAGEND_FIXED)
    _cogl_pipeline_set_fragend (pipeline, COGL_PIPELINE_FRAGEND_UNDEFINED);
#endif
#ifdef COGL_PIPELINE_VERTEND_FIXED
  if (pipeline->vertend == COGL_PIPELINE_VERTEND_FIXED)
    _cogl_pipeline_set_vertend (pipeline, COGL_PIPELINE_VERTEND_UNDEFINED);
#endif

  /* XXX:
   * To simplify things for the vertex, fragment and program backends
   * we are careful about how we report STATE_LAYERS changes.
   *
   * All STATE_LAYERS change notification with the exception of
   * ->n_layers will also result in layer_pre_change_notifications.
   *  For backends that perform code generation for fragment
   *  processing they typically need to understand the details of how
   *  layers get changed to determine if they need to repeat codegen.
   *  It doesn't help them to
   * report a pipeline STATE_LAYERS change for all layer changes since
   * it's so broad, they really need to wait for the specific layer
   * change to be notified.  What does help though is to report a
   * STATE_LAYERS change for a change in
   * ->n_layers because they typically do need to repeat codegen in
   *  that case.
   *
   * Here we ensure that change notifications against a pipeline or
   * against a layer are mutually exclusive as far as fragment, vertex
   * and program backends are concerned.
   */
  if (!from_layer_change)
    {
      int i;

      if (pipeline->fragend != COGL_PIPELINE_FRAGEND_UNDEFINED &&
          _cogl_pipeline_fragends[pipeline->fragend]->pipeline_pre_change_notify)
        {
          const CoglPipelineFragend *fragend =
            _cogl_pipeline_fragends[pipeline->fragend];
          fragend->pipeline_pre_change_notify (pipeline, change, new_color);
        }

      if (pipeline->vertend != COGL_PIPELINE_VERTEND_UNDEFINED &&
          _cogl_pipeline_vertends[pipeline->vertend]->pipeline_pre_change_notify)
        {
          const CoglPipelineVertend *vertend =
            _cogl_pipeline_vertends[pipeline->vertend];
          vertend->pipeline_pre_change_notify (pipeline, change, new_color);
        }

      for (i = 0; i < COGL_PIPELINE_N_PROGENDS; i++)
        if (_cogl_pipeline_progends[i]->pipeline_pre_change_notify)
          _cogl_pipeline_progends[i]->pipeline_pre_change_notify (pipeline,
                                                                  change,
                                                                  new_color);
    }

  /* There may be an arbitrary tree of descendants of this pipeline;
   * any of which may indirectly depend on this pipeline as the
   * authority for some set of properties. (Meaning for example that
   * one of its descendants derives its color or blending state from
   * this pipeline.)
   *
   * We can't modify any property that this pipeline is the authority
   * for unless we create another pipeline to take its place first and
   * make sure descendants reference this new pipeline instead.
   */

  /* The simplest descendants to handle are weak pipelines; we simply
   * destroy them if we are modifying a pipeline they depend on. This
   * means weak pipelines never cause us to do a copy-on-write. */
  _cogl_pipeline_node_foreach_child (COGL_PIPELINE_NODE (pipeline),
                                     destroy_weak_children_cb,
                                     NULL);

  /* If there are still children remaining though we'll need to
   * perform a copy-on-write and reparent the dependants as children
   * of the copy. */
  if (!COGL_LIST_EMPTY (&COGL_PIPELINE_NODE (pipeline)->children))
    {
      CoglPipeline *new_authority;

      COGL_STATIC_COUNTER (pipeline_copy_on_write_counter,
                           "pipeline copy on write counter",
                           "Increments each time a pipeline "
                           "must be copied to allow modification",
                           0 /* no application private data */);

      COGL_COUNTER_INC (_cogl_uprof_context, pipeline_copy_on_write_counter);

      new_authority =
        cogl_pipeline_copy (_cogl_pipeline_get_parent (pipeline));
      _cogl_pipeline_set_static_breadcrumb (new_authority,
                                            "pre_change_notify:copy-on-write");

      /* We could explicitly walk the descendants, OR together the set
       * of differences that we determine this pipeline is the
       * authority on and only copy those differences copied across.
       *
       * Or, if we don't explicitly walk the descendants we at least
       * know that pipeline->differences represents the largest set of
       * differences that this pipeline could possibly be an authority
       * on.
       *
       * We do the later just because it's simplest, but we might need
       * to come back to this later...
       */
      _cogl_pipeline_copy_differences (new_authority, pipeline,
                                       pipeline->differences);

      /* Reparent the dependants of pipeline to be children of
       * new_authority instead... */
      _cogl_pipeline_node_foreach_child (COGL_PIPELINE_NODE (pipeline),
                                         reparent_children_cb,
                                         new_authority);

      /* The children will keep the new authority alive so drop the
       * reference we got when copying... */
      cogl_object_unref (new_authority);
    }

  /* At this point we know we have a pipeline with no strong
   * dependants (though we may have some weak children) so we are now
   * free to modify the pipeline. */

  pipeline->age++;

  if (change & COGL_PIPELINE_STATE_NEEDS_BIG_STATE &&
      !pipeline->has_big_state)
    {
      pipeline->big_state = g_slice_new (CoglPipelineBigState);
      pipeline->has_big_state = TRUE;
    }

  /* Note: conceptually we have just been notified that a single
   * property value is about to change, but since some state-groups
   * contain multiple properties and 'pipeline' is about to take over
   * being the authority for the property's corresponding state-group
   * we need to maintain the integrity of the other property values
   * too.
   *
   * To ensure this we handle multi-property state-groups by copying
   * all the values from the old-authority to the new...
   *
   * We don't have to worry about non-sparse property groups since
   * we never take over being an authority for such properties so
   * they automatically maintain integrity.
   */
  if (change & COGL_PIPELINE_STATE_ALL_SPARSE &&
      !(pipeline->differences & change))
    {
      _cogl_pipeline_init_multi_property_sparse_state (pipeline, change);
      pipeline->differences |= change;
    }

  /* Each pipeline has a sorted cache of the layers it depends on
   * which will need updating via _cogl_pipeline_update_layers_cache
   * if a pipeline's layers are changed. */
  if (change == COGL_PIPELINE_STATE_LAYERS)
    recursively_free_layer_caches (pipeline);

  /* If the pipeline being changed is the same as the last pipeline we
   * flushed then we keep a track of the changes so we can try to
   * minimize redundant OpenGL calls if the same pipeline is flushed
   * again.
   */
  if (ctx->current_pipeline == pipeline)
    ctx->current_pipeline_changes_since_flush |= change;
}


static void
_cogl_pipeline_add_layer_difference (CoglPipeline *pipeline,
                                     CoglPipelineLayer *layer,
                                     gboolean inc_n_layers)
{
  g_return_if_fail (layer->owner == NULL);

  layer->owner = pipeline;
  cogl_object_ref (layer);

  /* - Flush journal primitives referencing the current state.
   * - Make sure the pipeline has no dependants so it may be modified.
   * - If the pipeline isn't currently an authority for the state being
   *   changed, then initialize that state from the current authority.
   */
  /* Note: the last argument to _cogl_pipeline_pre_change_notify is
   * needed to differentiate STATE_LAYER changes which don't affect
   * the number of layers from those that do. NB: Layer change
   * notifications that don't change the number of layers don't get
   * forwarded to the fragend. */
  _cogl_pipeline_pre_change_notify (pipeline,
                                    COGL_PIPELINE_STATE_LAYERS,
                                    NULL,
                                    !inc_n_layers);

  pipeline->differences |= COGL_PIPELINE_STATE_LAYERS;

  pipeline->layer_differences =
    g_list_prepend (pipeline->layer_differences, layer);

  if (inc_n_layers)
    pipeline->n_layers++;
}

static void
_cogl_pipeline_remove_layer_difference (CoglPipeline *pipeline,
                                        CoglPipelineLayer *layer,
                                        gboolean dec_n_layers)
{
  g_return_if_fail (layer->owner == pipeline);

  /* - Flush journal primitives referencing the current state.
   * - Make sure the pipeline has no dependants so it may be modified.
   * - If the pipeline isn't currently an authority for the state being
   *   changed, then initialize that state from the current authority.
   */
  /* Note: the last argument to _cogl_pipeline_pre_change_notify is
   * needed to differentiate STATE_LAYER changes which don't affect
   * the number of layers from those that do. NB: Layer change
   * notifications that don't change the number of layers don't get
   * forwarded to the fragend. */
  _cogl_pipeline_pre_change_notify (pipeline,
                                    COGL_PIPELINE_STATE_LAYERS,
                                    NULL,
                                    !dec_n_layers);

  layer->owner = NULL;
  cogl_object_unref (layer);

  pipeline->differences |= COGL_PIPELINE_STATE_LAYERS;

  pipeline->layer_differences =
    g_list_remove (pipeline->layer_differences, layer);

  if (dec_n_layers)
    pipeline->n_layers--;
}

static void
_cogl_pipeline_try_reverting_layers_authority (CoglPipeline *authority,
                                               CoglPipeline *old_authority)
{
  if (authority->layer_differences == NULL &&
      _cogl_pipeline_get_parent (authority))
    {
      /* If the previous _STATE_LAYERS authority has the same
       * ->n_layers then we can revert to that being the authority
       *  again. */
      if (!old_authority)
        {
          old_authority =
            _cogl_pipeline_get_authority (_cogl_pipeline_get_parent (authority),
                                          COGL_PIPELINE_STATE_LAYERS);
        }

      if (old_authority->n_layers == authority->n_layers)
        authority->differences &= ~COGL_PIPELINE_STATE_LAYERS;
    }
}


void
_cogl_pipeline_update_blend_enable (CoglPipeline *pipeline,
                                    CoglPipelineState change)
{
  gboolean blend_enable =
    _cogl_pipeline_needs_blending_enabled (pipeline, change, NULL);

  if (blend_enable != pipeline->real_blend_enable)
    {
      /* - Flush journal primitives referencing the current state.
       * - Make sure the pipeline has no dependants so it may be
       *   modified.
       * - If the pipeline isn't currently an authority for the state
       *   being changed, then initialize that state from the current
       *   authority.
       */
      _cogl_pipeline_pre_change_notify (pipeline,
                                        COGL_PIPELINE_STATE_REAL_BLEND_ENABLE,
                                        NULL,
                                        FALSE);
      pipeline->real_blend_enable = blend_enable;
    }
}

typedef struct
{
  int keep_n;
  int current_pos;
  int first_index_to_prune;
} CoglPipelinePruneLayersInfo;

static gboolean
update_prune_layers_info_cb (CoglPipelineLayer *layer, void *user_data)
{
  CoglPipelinePruneLayersInfo *state = user_data;

  if (state->current_pos == state->keep_n)
    {
      state->first_index_to_prune = layer->index;
      return FALSE;
    }
  state->current_pos++;
  return TRUE;
}

void
_cogl_pipeline_prune_to_n_layers (CoglPipeline *pipeline, int n)
{
  CoglPipeline *authority =
    _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_LAYERS);
  CoglPipelinePruneLayersInfo state;
  GList *l;
  GList *next;

  if (authority->n_layers <= n)
    return;

  _cogl_pipeline_pre_change_notify (pipeline,
                                    COGL_PIPELINE_STATE_LAYERS,
                                    NULL,
                                    FALSE);

  state.keep_n = n;
  state.current_pos = 0;
  _cogl_pipeline_foreach_layer_internal (pipeline,
                                         update_prune_layers_info_cb,
                                         &state);

  pipeline->differences |= COGL_PIPELINE_STATE_LAYERS;
  pipeline->n_layers = n;

  /* It's possible that this pipeline owns some of the layers being
   * discarded, so we'll need to unlink them... */
  for (l = pipeline->layer_differences; l; l = next)
    {
      CoglPipelineLayer *layer = l->data;
      next = l->next; /* we're modifying the list we're iterating */

      if (layer->index > state.first_index_to_prune)
        _cogl_pipeline_remove_layer_difference (pipeline, layer, FALSE);
    }

  pipeline->differences |= COGL_PIPELINE_STATE_LAYERS;
}

static void
_cogl_pipeline_fragend_layer_change_notify (CoglPipeline *owner,
                                            CoglPipelineLayer *layer,
                                            CoglPipelineLayerState change)
{
  /* NB: Although layers can have private state associated with them
   * by multiple backends we know that a layer can't be *changed* if
   * it has multiple dependants so if we reach here we know we only
   * have a single owner and can only be associated with a single
   * backend that needs to be notified of the layer change...
   */
  if (owner->fragend != COGL_PIPELINE_FRAGEND_UNDEFINED &&
      _cogl_pipeline_fragends[owner->fragend]->layer_pre_change_notify)
    {
      const CoglPipelineFragend *fragend =
        _cogl_pipeline_fragends[owner->fragend];
      fragend->layer_pre_change_notify (owner, layer, change);
    }
}

static void
_cogl_pipeline_vertend_layer_change_notify (CoglPipeline *owner,
                                            CoglPipelineLayer *layer,
                                            CoglPipelineLayerState change)
{
  /* NB: The comment in fragend_layer_change_notify applies here too */
  if (owner->vertend != COGL_PIPELINE_VERTEND_UNDEFINED &&
      _cogl_pipeline_vertends[owner->vertend]->layer_pre_change_notify)
    {
      const CoglPipelineVertend *vertend =
        _cogl_pipeline_vertends[owner->vertend];
      vertend->layer_pre_change_notify (owner, layer, change);
    }
}

static void
_cogl_pipeline_progend_layer_change_notify (CoglPipeline *owner,
                                            CoglPipelineLayer *layer,
                                            CoglPipelineLayerState change)
{
  int i;

  /* Give all of the progends a chance to notice that the layer has
     changed */
  for (i = 0; i < COGL_PIPELINE_N_PROGENDS; i++)
    if (_cogl_pipeline_progends[i]->layer_pre_change_notify)
      _cogl_pipeline_progends[i]->layer_pre_change_notify (owner,
                                                           layer,
                                                           change);
}

unsigned int
_cogl_get_n_args_for_combine_func (CoglPipelineCombineFunc func)
{
  switch (func)
    {
    case COGL_PIPELINE_COMBINE_FUNC_REPLACE:
      return 1;
    case COGL_PIPELINE_COMBINE_FUNC_MODULATE:
    case COGL_PIPELINE_COMBINE_FUNC_ADD:
    case COGL_PIPELINE_COMBINE_FUNC_ADD_SIGNED:
    case COGL_PIPELINE_COMBINE_FUNC_SUBTRACT:
    case COGL_PIPELINE_COMBINE_FUNC_DOT3_RGB:
    case COGL_PIPELINE_COMBINE_FUNC_DOT3_RGBA:
      return 2;
    case COGL_PIPELINE_COMBINE_FUNC_INTERPOLATE:
      return 3;
    }
  return 0;
}

static void
_cogl_pipeline_layer_init_multi_property_sparse_state (
                                                  CoglPipelineLayer *layer,
                                                  CoglPipelineLayerState change)
{
  CoglPipelineLayer *authority;

  /* Nothing to initialize in these cases since they are all comprised
   * of one member which we expect to immediately be overwritten. */
  if (!(change & COGL_PIPELINE_LAYER_STATE_MULTI_PROPERTY))
    return;

  authority = _cogl_pipeline_layer_get_authority (layer, change);

  switch (change)
    {
    /* XXX: avoid using a default: label so we get a warning if we
     * don't explicitly handle a newly defined state-group here. */
    case COGL_PIPELINE_LAYER_STATE_UNIT:
    case COGL_PIPELINE_LAYER_STATE_TEXTURE_TARGET:
    case COGL_PIPELINE_LAYER_STATE_TEXTURE_DATA:
    case COGL_PIPELINE_LAYER_STATE_POINT_SPRITE_COORDS:
    case COGL_PIPELINE_LAYER_STATE_USER_MATRIX:
    case COGL_PIPELINE_LAYER_STATE_COMBINE_CONSTANT:
      g_return_if_reached ();

    /* XXX: technically we could probably even consider these as
     * single property state-groups from the pov that currently the
     * corresponding property setters always update all of the values
     * at the same time. */
    case COGL_PIPELINE_LAYER_STATE_FILTERS:
      layer->min_filter = authority->min_filter;
      layer->mag_filter = authority->mag_filter;
      break;
    case COGL_PIPELINE_LAYER_STATE_WRAP_MODES:
      layer->wrap_mode_s = authority->wrap_mode_s;
      layer->wrap_mode_t = authority->wrap_mode_t;
      layer->wrap_mode_p = authority->wrap_mode_p;
      break;
    case COGL_PIPELINE_LAYER_STATE_COMBINE:
      {
        int n_args;
        int i;
        CoglPipelineLayerBigState *src_big_state = authority->big_state;
        CoglPipelineLayerBigState *dest_big_state = layer->big_state;
        GLint func = src_big_state->texture_combine_rgb_func;

        dest_big_state->texture_combine_rgb_func = func;
        n_args = _cogl_get_n_args_for_combine_func (func);
        for (i = 0; i < n_args; i++)
          {
            dest_big_state->texture_combine_rgb_src[i] =
              src_big_state->texture_combine_rgb_src[i];
            dest_big_state->texture_combine_rgb_op[i] =
              src_big_state->texture_combine_rgb_op[i];
          }

        func = src_big_state->texture_combine_alpha_func;
        dest_big_state->texture_combine_alpha_func = func;
        n_args = _cogl_get_n_args_for_combine_func (func);
        for (i = 0; i < n_args; i++)
          {
            dest_big_state->texture_combine_alpha_src[i] =
              src_big_state->texture_combine_alpha_src[i];
            dest_big_state->texture_combine_alpha_op[i] =
              src_big_state->texture_combine_alpha_op[i];
          }
        break;
      }
    }
}

/* NB: This function will allocate a new derived layer if you are
 * trying to change the state of a layer with dependants so you must
 * always check the return value.
 *
 * If a new layer is returned it will be owned by required_owner.
 *
 * required_owner can only by NULL for new, currently unowned layers
 * with no dependants.
 */
CoglPipelineLayer *
_cogl_pipeline_layer_pre_change_notify (CoglPipeline *required_owner,
                                        CoglPipelineLayer *layer,
                                        CoglPipelineLayerState change)
{
  CoglTextureUnit *unit;

  /* Identify the case where the layer is new with no owner or
   * dependants and so we don't need to do anything. */
  if (COGL_LIST_EMPTY (&COGL_PIPELINE_NODE (layer)->children) &&
      layer->owner == NULL)
    goto init_layer_state;

  /* We only allow a NULL required_owner for new layers */
  g_return_val_if_fail (required_owner != NULL, layer);

  /* Chain up:
   * A modification of a layer is indirectly also a modification of
   * its owner so first make sure to flush the journal of any
   * references to the current owner state and if necessary perform
   * a copy-on-write for the required_owner if it has dependants.
   */
  _cogl_pipeline_pre_change_notify (required_owner,
                                    COGL_PIPELINE_STATE_LAYERS,
                                    NULL,
                                    TRUE);

  /* Unlike pipelines; layers are simply considered immutable once
   * they have dependants - either direct children, or another
   * pipeline as an owner.
   */
  if (!COGL_LIST_EMPTY (&COGL_PIPELINE_NODE (layer)->children) ||
      layer->owner != required_owner)
    {
      CoglPipelineLayer *new = _cogl_pipeline_layer_copy (layer);
      if (layer->owner == required_owner)
        _cogl_pipeline_remove_layer_difference (required_owner, layer, FALSE);
      _cogl_pipeline_add_layer_difference (required_owner, new, FALSE);
      cogl_object_unref (new);
      layer = new;
      goto init_layer_state;
    }

  /* Note: At this point we know there is only one pipeline dependant on
   * this layer (required_owner), and there are no other layers
   * dependant on this layer so it's ok to modify it. */

  _cogl_pipeline_fragend_layer_change_notify (required_owner, layer, change);
  _cogl_pipeline_vertend_layer_change_notify (required_owner, layer, change);
  _cogl_pipeline_progend_layer_change_notify (required_owner, layer, change);

  /* If the layer being changed is the same as the last layer we
   * flushed to the corresponding texture unit then we keep a track of
   * the changes so we can try to minimize redundant OpenGL calls if
   * the same layer is flushed again.
   */
  unit = _cogl_get_texture_unit (_cogl_pipeline_layer_get_unit_index (layer));
  if (unit->layer == layer)
    unit->layer_changes_since_flush |= change;

init_layer_state:

  if (required_owner)
    required_owner->age++;

  if (change & COGL_PIPELINE_LAYER_STATE_NEEDS_BIG_STATE &&
      !layer->has_big_state)
    {
      layer->big_state = g_slice_new (CoglPipelineLayerBigState);
      layer->has_big_state = TRUE;
    }

  /* Note: conceptually we have just been notified that a single
   * property value is about to change, but since some state-groups
   * contain multiple properties and 'layer' is about to take over
   * being the authority for the property's corresponding state-group
   * we need to maintain the integrity of the other property values
   * too.
   *
   * To ensure this we handle multi-property state-groups by copying
   * all the values from the old-authority to the new...
   *
   * We don't have to worry about non-sparse property groups since
   * we never take over being an authority for such properties so
   * they automatically maintain integrity.
   */
  if (change & COGL_PIPELINE_LAYER_STATE_ALL_SPARSE &&
      !(layer->differences & change))
    {
      _cogl_pipeline_layer_init_multi_property_sparse_state (layer, change);
      layer->differences |= change;
    }

  return layer;
}

static void
_cogl_pipeline_layer_unparent (CoglPipelineNode *layer)
{
  /* Chain up */
  _cogl_pipeline_node_unparent_real (layer);
}

static void
_cogl_pipeline_layer_set_parent (CoglPipelineLayer *layer,
                                 CoglPipelineLayer *parent)
{
  /* Chain up */
  _cogl_pipeline_node_set_parent_real (COGL_PIPELINE_NODE (layer),
                                       COGL_PIPELINE_NODE (parent),
                                       _cogl_pipeline_layer_unparent,
                                       TRUE);
}

/* XXX: This is duplicated logic; the same as for
 * _cogl_pipeline_prune_redundant_ancestry it would be nice to find a
 * way to consolidate these functions! */
void
_cogl_pipeline_layer_prune_redundant_ancestry (CoglPipelineLayer *layer)
{
  CoglPipelineLayer *new_parent = _cogl_pipeline_layer_get_parent (layer);

  /* walk up past ancestors that are now redundant and potentially
   * reparent the layer. */
  while (_cogl_pipeline_layer_get_parent (new_parent) &&
         (new_parent->differences | layer->differences) ==
         layer->differences)
    new_parent = _cogl_pipeline_layer_get_parent (new_parent);

  _cogl_pipeline_layer_set_parent (layer, new_parent);
}

typedef struct
{
  /* The layer we are trying to find */
  int                         layer_index;

  /* The layer we find or untouched if not found */
  CoglPipelineLayer          *layer;

  /* If the layer can't be found then a new layer should be
   * inserted after this texture unit index... */
  int                         insert_after;

  /* When adding a layer we need the list of layers to shift up
   * to a new texture unit. When removing we need the list of
   * layers to shift down.
   *
   * Note: the list isn't sorted */
  CoglPipelineLayer         **layers_to_shift;
  int                         n_layers_to_shift;

  /* When adding a layer we don't need a complete list of
   * layers_to_shift if we find a layer already corresponding to the
   * layer_index.  */
  gboolean                    ignore_shift_layers_if_found;

} CoglPipelineLayerInfo;

/* Returns TRUE once we know there is nothing more to update */
static gboolean
update_layer_info (CoglPipelineLayer *layer,
                   CoglPipelineLayerInfo *layer_info)
{
  if (layer->index == layer_info->layer_index)
    {
      layer_info->layer = layer;
      if (layer_info->ignore_shift_layers_if_found)
        return TRUE;
    }
  else if (layer->index < layer_info->layer_index)
    {
      int unit_index = _cogl_pipeline_layer_get_unit_index (layer);
      layer_info->insert_after = unit_index;
    }
  else
    layer_info->layers_to_shift[layer_info->n_layers_to_shift++] =
      layer;

  return FALSE;
}

/* Returns FALSE to break out of a _foreach_layer () iteration */
static gboolean
update_layer_info_cb (CoglPipelineLayer *layer,
                      void *user_data)
{
  CoglPipelineLayerInfo *layer_info = user_data;

  if (update_layer_info (layer, layer_info))
    return FALSE; /* break */
  else
    return TRUE; /* continue */
}

static void
_cogl_pipeline_get_layer_info (CoglPipeline *pipeline,
                               CoglPipelineLayerInfo *layer_info)
{
  /* Note: we are assuming this pipeline is a _STATE_LAYERS authority */
  int n_layers = pipeline->n_layers;
  int i;

  /* FIXME: _cogl_pipeline_foreach_layer_internal now calls
   * _cogl_pipeline_update_layers_cache anyway so this codepath is
   * pointless! */
  if (layer_info->ignore_shift_layers_if_found &&
      pipeline->layers_cache_dirty)
    {
      /* The expectation is that callers of
       * _cogl_pipeline_get_layer_info are likely to be modifying the
       * list of layers associated with a pipeline so in this case
       * where we don't have a cache of the layers and we don't
       * necessarily have to iterate all the layers of the pipeline we
       * use a foreach_layer callback instead of updating the cache
       * and iterating that as below. */
      _cogl_pipeline_foreach_layer_internal (pipeline,
                                             update_layer_info_cb,
                                             layer_info);
      return;
    }

  _cogl_pipeline_update_layers_cache (pipeline);
  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = pipeline->layers_cache[i];

      if (update_layer_info (layer, layer_info))
        return;
    }
}

CoglPipelineLayer *
_cogl_pipeline_get_layer (CoglPipeline *pipeline,
                          int layer_index)
{
  CoglPipeline *authority =
    _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_LAYERS);
  CoglPipelineLayerInfo layer_info;
  CoglPipelineLayer *layer;
  int unit_index;
  int i;

  _COGL_GET_CONTEXT (ctx, NULL);

  /* The layer index of the layer we want info about */
  layer_info.layer_index = layer_index;

  /* If a layer already exists with the given index this will be
   * updated. */
  layer_info.layer = NULL;

  /* If a layer isn't found for the given index we'll need to know
   * where to insert a new layer. */
  layer_info.insert_after = -1;

  /* If a layer can't be found then we'll need to insert a new layer
   * and bump up the texture unit for all layers with an index
   * > layer_index. */
  layer_info.layers_to_shift =
    g_alloca (sizeof (CoglPipelineLayer *) * authority->n_layers);
  layer_info.n_layers_to_shift = 0;

  /* If an exact match is found though we don't need a complete
   * list of layers with indices > layer_index... */
  layer_info.ignore_shift_layers_if_found = TRUE;

  _cogl_pipeline_get_layer_info (authority, &layer_info);

  if (layer_info.layer)
    return layer_info.layer;

  unit_index = layer_info.insert_after + 1;
  if (unit_index == 0)
    layer = _cogl_pipeline_layer_copy (ctx->default_layer_0);
  else
    {
      CoglPipelineLayer *new;
      layer = _cogl_pipeline_layer_copy (ctx->default_layer_n);
      new = _cogl_pipeline_set_layer_unit (NULL, layer, unit_index);
      /* Since we passed a newly allocated layer we wouldn't expect
       * _set_layer_unit() to have to allocate *another* layer. */
      g_assert (new == layer);
    }
  layer->index = layer_index;

  for (i = 0; i < layer_info.n_layers_to_shift; i++)
    {
      CoglPipelineLayer *shift_layer = layer_info.layers_to_shift[i];

      unit_index = _cogl_pipeline_layer_get_unit_index (shift_layer);
      _cogl_pipeline_set_layer_unit (pipeline, shift_layer, unit_index + 1);
      /* NB: shift_layer may not be writeable so _set_layer_unit()
       * will allocate a derived layer internally which will become
       * owned by pipeline. Check the return value if we need to do
       * anything else with this layer. */
    }

  _cogl_pipeline_add_layer_difference (pipeline, layer, TRUE);

  cogl_object_unref (layer);

  return layer;
}

void
_cogl_pipeline_prune_empty_layer_difference (CoglPipeline *layers_authority,
                                             CoglPipelineLayer *layer)
{
  /* Find the GList link that references the empty layer */
  GList *link = g_list_find (layers_authority->layer_differences, layer);
  /* No pipeline directly owns the root node layer so this is safe... */
  CoglPipelineLayer *layer_parent = _cogl_pipeline_layer_get_parent (layer);
  CoglPipelineLayerInfo layer_info;
  CoglPipeline *old_layers_authority;

  g_return_if_fail (link != NULL);

  /* If the layer's parent doesn't have an owner then we can simply
   * take ownership ourselves and drop our reference on the empty
   * layer. We don't want to take ownership of the root node layer so
   * we also need to verify that the parent has a parent
   */
  if (layer_parent->index == layer->index && layer_parent->owner == NULL &&
      _cogl_pipeline_layer_get_parent (layer_parent) != NULL)
    {
      cogl_object_ref (layer_parent);
      layer_parent->owner = layers_authority;
      link->data = layer_parent;
      cogl_object_unref (layer);
      recursively_free_layer_caches (layers_authority);
      return;
    }

  /* Now we want to find the layer that would become the authority for
   * layer->index if we were to remove layer from
   * layers_authority->layer_differences
   */

  /* The layer index of the layer we want info about */
  layer_info.layer_index = layer->index;

  /* If a layer already exists with the given index this will be
   * updated. */
  layer_info.layer = NULL;

  /* If a layer can't be found then we'll need to insert a new layer
   * and bump up the texture unit for all layers with an index
   * > layer_index. */
  layer_info.layers_to_shift =
    g_alloca (sizeof (CoglPipelineLayer *) * layers_authority->n_layers);
  layer_info.n_layers_to_shift = 0;

  /* If an exact match is found though we don't need a complete
   * list of layers with indices > layer_index... */
  layer_info.ignore_shift_layers_if_found = TRUE;

  /* We know the default/root pipeline isn't a LAYERS authority so it's
   * safe to use the result of _cogl_pipeline_get_parent (layers_authority)
   * without checking it.
   */
  old_layers_authority =
    _cogl_pipeline_get_authority (_cogl_pipeline_get_parent (layers_authority),
                                  COGL_PIPELINE_STATE_LAYERS);

  _cogl_pipeline_get_layer_info (old_layers_authority, &layer_info);

  /* If layer is the defining layer for the corresponding ->index then
   * we can't get rid of it. */
  if (!layer_info.layer)
    return;

  /* If the layer that would become the authority for layer->index is
   * _cogl_pipeline_layer_get_parent (layer) then we can simply remove the
   * layer difference. */
  if (layer_info.layer == _cogl_pipeline_layer_get_parent (layer))
    {
      _cogl_pipeline_remove_layer_difference (layers_authority, layer, FALSE);
      _cogl_pipeline_try_reverting_layers_authority (layers_authority,
                                                     old_layers_authority);
    }
}

typedef struct
{
  int i;
  CoglPipeline *pipeline;
  unsigned long fallback_layers;
} CoglPipelineFallbackState;

static gboolean
fallback_layer_cb (CoglPipelineLayer *layer, void *user_data)
{
  CoglPipelineFallbackState *state = user_data;
  CoglPipeline *pipeline = state->pipeline;
  CoglHandle texture = _cogl_pipeline_layer_get_texture (layer);
  GLenum gl_target;
  COGL_STATIC_COUNTER (layer_fallback_counter,
                       "layer fallback counter",
                       "Increments each time a layer's texture is "
                       "forced to a fallback texture",
                       0 /* no application private data */);

  _COGL_GET_CONTEXT (ctx, FALSE);

  if (!(state->fallback_layers & 1<<state->i))
    return TRUE;

  COGL_COUNTER_INC (_cogl_uprof_context, layer_fallback_counter);

  if (G_LIKELY (texture != COGL_INVALID_HANDLE))
    cogl_texture_get_gl_texture (texture, NULL, &gl_target);
  else
    gl_target = GL_TEXTURE_2D;

  if (gl_target == GL_TEXTURE_2D)
    texture = ctx->default_gl_texture_2d_tex;
#ifdef HAVE_COGL_GL
  else if (gl_target == GL_TEXTURE_RECTANGLE_ARB)
    texture = ctx->default_gl_texture_rect_tex;
#endif
  else
    {
      g_warning ("We don't have a fallback texture we can use to fill "
                 "in for an invalid pipeline layer, since it was "
                 "using an unsupported texture target ");
      /* might get away with this... */
      texture = ctx->default_gl_texture_2d_tex;
    }

  cogl_pipeline_set_layer_texture (pipeline, layer->index, texture);

  state->i++;

  return TRUE;
}

typedef struct
{
  CoglPipeline *pipeline;
  CoglHandle texture;
} CoglPipelineOverrideLayerState;

static gboolean
override_layer_texture_cb (CoglPipelineLayer *layer, void *user_data)
{
  CoglPipelineOverrideLayerState *state = user_data;

  cogl_pipeline_set_layer_texture (state->pipeline,
                                   layer->index,
                                   state->texture);

  return TRUE;
}

void
_cogl_pipeline_apply_overrides (CoglPipeline *pipeline,
                                CoglPipelineFlushOptions *options)
{
  COGL_STATIC_COUNTER (apply_overrides_counter,
                       "pipeline overrides counter",
                       "Increments each time we have to apply "
                       "override options to a pipeline",
                       0 /* no application private data */);

  COGL_COUNTER_INC (_cogl_uprof_context, apply_overrides_counter);

  if (options->flags & COGL_PIPELINE_FLUSH_DISABLE_MASK)
    {
      int i;

      /* NB: we can assume that once we see one bit to disable
       * a layer, all subsequent layers are also disabled. */
      for (i = 0; i < 32 && options->disable_layers & (1<<i); i++)
        ;

      _cogl_pipeline_prune_to_n_layers (pipeline, i);
    }

  if (options->flags & COGL_PIPELINE_FLUSH_FALLBACK_MASK)
    {
      CoglPipelineFallbackState state;

      state.i = 0;
      state.pipeline = pipeline;
      state.fallback_layers = options->fallback_layers;

      _cogl_pipeline_foreach_layer_internal (pipeline,
                                             fallback_layer_cb,
                                             &state);
    }

  if (options->flags & COGL_PIPELINE_FLUSH_LAYER0_OVERRIDE)
    {
      CoglPipelineOverrideLayerState state;

      _cogl_pipeline_prune_to_n_layers (pipeline, 1);

      /* NB: we are overriding the first layer, but we don't know
       * the user's given layer_index, which is why we use
       * _cogl_pipeline_foreach_layer_internal() here even though we know
       * there's only one layer. */
      state.pipeline = pipeline;
      state.texture = options->layer0_override_texture;
      _cogl_pipeline_foreach_layer_internal (pipeline,
                                             override_layer_texture_cb,
                                             &state);
    }
}

/* Determine the mask of differences between two layers.
 *
 * XXX: If layers and pipelines could both be cast to a common Tree
 * type of some kind then we could have a unified
 * compare_differences() function.
 */
unsigned long
_cogl_pipeline_layer_compare_differences (CoglPipelineLayer *layer0,
                                          CoglPipelineLayer *layer1)
{
  CoglPipelineLayer *node0;
  CoglPipelineLayer *node1;
  int len0;
  int len1;
  int len0_index;
  int len1_index;
  int count;
  int i;
  CoglPipelineLayer *common_ancestor = NULL;
  unsigned long layers_difference = 0;

  _COGL_GET_CONTEXT (ctx, 0);

  /* Algorithm:
   *
   * 1) Walk the ancestors of each layer to the root node, adding a
   *    pointer to each ancester node to two GArrays:
   *    ctx->pipeline0_nodes, and ctx->pipeline1_nodes.
   *
   * 2) Compare the arrays to find the nodes where they stop to
   *    differ.
   *
   * 3) For each array now iterate from index 0 to the first node of
   *    difference ORing that nodes ->difference mask into the final
   *    pipeline_differences mask.
   */

  g_array_set_size (ctx->pipeline0_nodes, 0);
  g_array_set_size (ctx->pipeline1_nodes, 0);
  for (node0 = layer0; node0; node0 = _cogl_pipeline_layer_get_parent (node0))
    g_array_append_vals (ctx->pipeline0_nodes, &node0, 1);
  for (node1 = layer1; node1; node1 = _cogl_pipeline_layer_get_parent (node1))
    g_array_append_vals (ctx->pipeline1_nodes, &node1, 1);

  len0 = ctx->pipeline0_nodes->len;
  len1 = ctx->pipeline1_nodes->len;
  /* There's no point looking at the last entries since we know both
   * layers must have the same default layer as their root node. */
  len0_index = len0 - 2;
  len1_index = len1 - 2;
  count = MIN (len0, len1) - 1;
  for (i = 0; i < count; i++)
    {
      node0 = g_array_index (ctx->pipeline0_nodes,
                             CoglPipelineLayer *, len0_index--);
      node1 = g_array_index (ctx->pipeline1_nodes,
                             CoglPipelineLayer *, len1_index--);
      if (node0 != node1)
        {
          common_ancestor = _cogl_pipeline_layer_get_parent (node0);
          break;
        }
    }

  /* If we didn't already find the first the common_ancestor ancestor
   * that's because one pipeline is a direct descendant of the other
   * and in this case the first common ancestor is the last node we
   * looked at. */
  if (!common_ancestor)
    common_ancestor = node0;

  count = len0 - 1;
  for (i = 0; i < count; i++)
    {
      node0 = g_array_index (ctx->pipeline0_nodes, CoglPipelineLayer *, i);
      if (node0 == common_ancestor)
        break;
      layers_difference |= node0->differences;
    }

  count = len1 - 1;
  for (i = 0; i < count; i++)
    {
      node1 = g_array_index (ctx->pipeline1_nodes, CoglPipelineLayer *, i);
      if (node1 == common_ancestor)
        break;
      layers_difference |= node1->differences;
    }

  return layers_difference;
}

static gboolean
layer_state_equal (CoglPipelineLayerStateIndex state_index,
                   CoglPipelineLayer **authorities0,
                   CoglPipelineLayer **authorities1,
                   CoglPipelineLayerStateComparitor comparitor)
{
  return comparitor (authorities0[state_index], authorities1[state_index]);
}

static void
_cogl_pipeline_layer_resolve_authorities (CoglPipelineLayer *layer,
                                          unsigned long differences,
                                          CoglPipelineLayer **authorities)
{
  unsigned long remaining = differences;
  CoglPipelineLayer *authority = layer;

  do
    {
      unsigned long found = authority->differences & remaining;
      int i;

      if (found == 0)
        continue;

      for (i = 0; TRUE; i++)
        {
          unsigned long state = (1L<<i);

          if (state & found)
            authorities[i] = authority;
          else if (state > found)
            break;
        }

      remaining &= ~found;
      if (remaining == 0)
        return;
    }
  while ((authority = _cogl_pipeline_layer_get_parent (authority)));

  g_assert (remaining == 0);
}

static gboolean
_cogl_pipeline_layer_equal (CoglPipelineLayer *layer0,
                            CoglPipelineLayer *layer1,
                            unsigned long differences_mask,
                            CoglPipelineEvalFlags flags)
{
  unsigned long layers_difference;
  CoglPipelineLayer *authorities0[COGL_PIPELINE_LAYER_STATE_SPARSE_COUNT];
  CoglPipelineLayer *authorities1[COGL_PIPELINE_LAYER_STATE_SPARSE_COUNT];

  if (layer0 == layer1)
    return TRUE;

  layers_difference =
    _cogl_pipeline_layer_compare_differences (layer0, layer1);

  /* Only compare the sparse state groups requested by the caller... */
  layers_difference &= differences_mask;

  _cogl_pipeline_layer_resolve_authorities (layer0,
                                            layers_difference,
                                            authorities0);
  _cogl_pipeline_layer_resolve_authorities (layer1,
                                            layers_difference,
                                            authorities1);

  if (layers_difference & COGL_PIPELINE_LAYER_STATE_TEXTURE_TARGET)
    {
      CoglPipelineLayerStateIndex state_index =
        COGL_PIPELINE_LAYER_STATE_TEXTURE_TARGET_INDEX;
      if (!_cogl_pipeline_layer_texture_target_equal (authorities0[state_index],
                                                      authorities1[state_index],
                                                      flags))
        return FALSE;
    }

  if (layers_difference & COGL_PIPELINE_LAYER_STATE_TEXTURE_DATA)
    {
      CoglPipelineLayerStateIndex state_index =
        COGL_PIPELINE_LAYER_STATE_TEXTURE_DATA_INDEX;
      if (!_cogl_pipeline_layer_texture_data_equal (authorities0[state_index],
                                                    authorities1[state_index],
                                                    flags))
        return FALSE;
    }

  if (layers_difference & COGL_PIPELINE_LAYER_STATE_COMBINE &&
      !layer_state_equal (COGL_PIPELINE_LAYER_STATE_COMBINE_INDEX,
                          authorities0, authorities1,
                          _cogl_pipeline_layer_combine_state_equal))
    return FALSE;

  if (layers_difference & COGL_PIPELINE_LAYER_STATE_COMBINE_CONSTANT &&
      !layer_state_equal (COGL_PIPELINE_LAYER_STATE_COMBINE_CONSTANT_INDEX,
                          authorities0, authorities1,
                          _cogl_pipeline_layer_combine_constant_equal))
    return FALSE;

  if (layers_difference & COGL_PIPELINE_LAYER_STATE_FILTERS &&
      !layer_state_equal (COGL_PIPELINE_LAYER_STATE_FILTERS_INDEX,
                          authorities0, authorities1,
                          _cogl_pipeline_layer_filters_equal))
    return FALSE;

  if (layers_difference & COGL_PIPELINE_LAYER_STATE_WRAP_MODES &&
      !layer_state_equal (COGL_PIPELINE_LAYER_STATE_WRAP_MODES_INDEX,
                          authorities0, authorities1,
                          _cogl_pipeline_layer_wrap_modes_equal))
    return FALSE;

  if (layers_difference & COGL_PIPELINE_LAYER_STATE_USER_MATRIX &&
      !layer_state_equal (COGL_PIPELINE_LAYER_STATE_USER_MATRIX_INDEX,
                          authorities0, authorities1,
                          _cogl_pipeline_layer_user_matrix_equal))
    return FALSE;

  if (layers_difference & COGL_PIPELINE_LAYER_STATE_POINT_SPRITE_COORDS &&
      !layer_state_equal (COGL_PIPELINE_LAYER_STATE_POINT_SPRITE_COORDS_INDEX,
                          authorities0, authorities1,
                          _cogl_pipeline_layer_point_sprite_coords_equal))
    return FALSE;

  return TRUE;
}

static gboolean
_cogl_pipeline_layers_equal (CoglPipeline *authority0,
                             CoglPipeline *authority1,
                             unsigned long differences,
                             CoglPipelineEvalFlags flags)
{
  int i;

  if (authority0->n_layers != authority1->n_layers)
    return FALSE;

  _cogl_pipeline_update_layers_cache (authority0);
  _cogl_pipeline_update_layers_cache (authority1);

  for (i = 0; i < authority0->n_layers; i++)
    {
      if (!_cogl_pipeline_layer_equal (authority0->layers_cache[i],
                                       authority1->layers_cache[i],
                                       differences,
                                       flags))
        return FALSE;
    }
  return TRUE;
}

/* Determine the mask of differences between two pipelines */
unsigned long
_cogl_pipeline_compare_differences (CoglPipeline *pipeline0,
                                    CoglPipeline *pipeline1)
{
  CoglPipeline *node0;
  CoglPipeline *node1;
  int len0;
  int len1;
  int len0_index;
  int len1_index;
  int count;
  int i;
  CoglPipeline *common_ancestor = NULL;
  unsigned long pipelines_difference = 0;

  _COGL_GET_CONTEXT (ctx, 0);

  /* Algorithm:
   *
   * 1) Walk the ancestors of each layer to the root node, adding a
   *    pointer to each ancester node to two GArrays:
   *    ctx->pipeline0_nodes, and ctx->pipeline1_nodes.
   *
   * 2) Compare the arrays to find the nodes where they stop to
   *    differ.
   *
   * 3) For each array now iterate from index 0 to the first node of
   *    difference ORing that nodes ->difference mask into the final
   *    pipeline_differences mask.
   */

  g_array_set_size (ctx->pipeline0_nodes, 0);
  g_array_set_size (ctx->pipeline1_nodes, 0);
  for (node0 = pipeline0; node0; node0 = _cogl_pipeline_get_parent (node0))
    g_array_append_vals (ctx->pipeline0_nodes, &node0, 1);
  for (node1 = pipeline1; node1; node1 = _cogl_pipeline_get_parent (node1))
    g_array_append_vals (ctx->pipeline1_nodes, &node1, 1);

  len0 = ctx->pipeline0_nodes->len;
  len1 = ctx->pipeline1_nodes->len;
  /* There's no point looking at the last entries since we know both
   * layers must have the same default layer as their root node. */
  len0_index = len0 - 2;
  len1_index = len1 - 2;
  count = MIN (len0, len1) - 1;
  for (i = 0; i < count; i++)
    {
      node0 = g_array_index (ctx->pipeline0_nodes,
                             CoglPipeline *, len0_index--);
      node1 = g_array_index (ctx->pipeline1_nodes,
                             CoglPipeline *, len1_index--);
      if (node0 != node1)
        {
          common_ancestor = _cogl_pipeline_get_parent (node0);
          break;
        }
    }

  /* If we didn't already find the first the common_ancestor ancestor
   * that's because one pipeline is a direct descendant of the other
   * and in this case the first common ancestor is the last node we
   * looked at. */
  if (!common_ancestor)
    common_ancestor = node0;

  count = len0 - 1;
  for (i = 0; i < count; i++)
    {
      node0 = g_array_index (ctx->pipeline0_nodes, CoglPipeline *, i);
      if (node0 == common_ancestor)
        break;
      pipelines_difference |= node0->differences;
    }

  count = len1 - 1;
  for (i = 0; i < count; i++)
    {
      node1 = g_array_index (ctx->pipeline1_nodes, CoglPipeline *, i);
      if (node1 == common_ancestor)
        break;
      pipelines_difference |= node1->differences;
    }

  return pipelines_difference;

}

static gboolean
simple_property_equal (CoglPipeline **authorities0,
                       CoglPipeline **authorities1,
                       unsigned long pipelines_difference,
                       CoglPipelineStateIndex state_index,
                       CoglPipelineStateComparitor comparitor)
{
  if (pipelines_difference & (1L<<state_index))
    {
      if (!comparitor (authorities0[state_index], authorities1[state_index]))
        return FALSE;
    }
  return TRUE;
}

static void
_cogl_pipeline_resolve_authorities (CoglPipeline *pipeline,
                                    unsigned long differences,
                                    CoglPipeline **authorities)
{
  unsigned long remaining = differences;
  CoglPipeline *authority = pipeline;

  do
    {
      unsigned long found = authority->differences & remaining;
      int i;

      if (found == 0)
        continue;

      for (i = 0; TRUE; i++)
        {
          unsigned long state = (1L<<i);

          if (state & found)
            authorities[i] = authority;
          else if (state > found)
            break;
        }

      remaining &= ~found;
      if (remaining == 0)
        return;
    }
  while ((authority = _cogl_pipeline_get_parent (authority)));

  g_assert (remaining == 0);
}

/* Comparison of two arbitrary pipelines is done by:
 * 1) walking up the parents of each pipeline until a common
 *    ancestor is found, and at each step ORing together the
 *    difference masks.
 *
 * 2) using the final difference mask to determine which state
 *    groups to compare.
 *
 * This is used, for example, by the Cogl journal to compare pipelines so that
 * it can split up geometry that needs different OpenGL state.
 *
 * XXX: When comparing texture layers, _cogl_pipeline_equal will actually
 * compare the underlying GL texture handle that the Cogl texture uses so that
 * atlas textures and sub textures will be considered equal if they point to
 * the same texture. This is useful for comparing pipelines in the journal but
 * it means that _cogl_pipeline_equal doesn't strictly compare whether the
 * pipelines are the same. If we needed those semantics we could perhaps add
 * another function or some flags to control the behaviour.
 *
 * XXX: Similarly when comparing the wrap modes,
 * COGL_PIPELINE_WRAP_MODE_AUTOMATIC is considered to be the same as
 * COGL_PIPELINE_WRAP_MODE_CLAMP_TO_EDGE because once they get to the
 * journal stage they act exactly the same.
 */
gboolean
_cogl_pipeline_equal (CoglPipeline *pipeline0,
                      CoglPipeline *pipeline1,
                      unsigned long differences,
                      unsigned long layer_differences,
                      CoglPipelineEvalFlags flags)
{
  unsigned long pipelines_difference;
  CoglPipeline *authorities0[COGL_PIPELINE_STATE_SPARSE_COUNT];
  CoglPipeline *authorities1[COGL_PIPELINE_STATE_SPARSE_COUNT];
  gboolean ret;

  COGL_STATIC_TIMER (pipeline_equal_timer,
                     "Mainloop", /* parent */
                     "_cogl_pipeline_equal",
                     "The time spent comparing cogl pipelines",
                     0 /* no application private data */);

  COGL_TIMER_START (_cogl_uprof_context, pipeline_equal_timer);

  if (pipeline0 == pipeline1)
    {
      ret = TRUE;
      goto done;
    }

  ret = FALSE;

  /* First check non-sparse properties */

  if (differences & COGL_PIPELINE_STATE_REAL_BLEND_ENABLE &&
      pipeline0->real_blend_enable != pipeline1->real_blend_enable)
    goto done;

  /* Then check sparse properties */

  pipelines_difference =
    _cogl_pipeline_compare_differences (pipeline0, pipeline1);

  /* Only compare the sparse state groups requested by the caller... */
  pipelines_difference &= differences;

  _cogl_pipeline_resolve_authorities (pipeline0,
                                      pipelines_difference,
                                      authorities0);
  _cogl_pipeline_resolve_authorities (pipeline1,
                                      pipelines_difference,
                                      authorities1);

  /* FIXME: we should resolve all the required authorities up front since
   * that should reduce some repeat ancestor traversals. */

  if (pipelines_difference & COGL_PIPELINE_STATE_COLOR)
    {
      CoglPipeline *authority0 = authorities0[COGL_PIPELINE_STATE_COLOR_INDEX];
      CoglPipeline *authority1 = authorities1[COGL_PIPELINE_STATE_COLOR_INDEX];

      if (!cogl_color_equal (&authority0->color, &authority1->color))
        goto done;
    }

  if (!simple_property_equal (authorities0, authorities1,
                              pipelines_difference,
                              COGL_PIPELINE_STATE_LIGHTING_INDEX,
                              _cogl_pipeline_lighting_state_equal))
    goto done;

  if (!simple_property_equal (authorities0, authorities1,
                              pipelines_difference,
                              COGL_PIPELINE_STATE_ALPHA_FUNC_INDEX,
                              _cogl_pipeline_alpha_func_state_equal))
    goto done;

  if (!simple_property_equal (authorities0, authorities1,
                              pipelines_difference,
                              COGL_PIPELINE_STATE_ALPHA_FUNC_REFERENCE_INDEX,
                              _cogl_pipeline_alpha_func_reference_state_equal))
    goto done;

  /* We don't need to compare the detailed blending state if we know
   * blending is disabled for both pipelines. */
  if (pipeline0->real_blend_enable &&
      pipelines_difference & COGL_PIPELINE_STATE_BLEND)
    {
      CoglPipeline *authority0 = authorities0[COGL_PIPELINE_STATE_BLEND_INDEX];
      CoglPipeline *authority1 = authorities1[COGL_PIPELINE_STATE_BLEND_INDEX];

      if (!_cogl_pipeline_blend_state_equal (authority0, authority1))
        goto done;
    }

  /* XXX: we don't need to compare the BLEND_ENABLE state because it's
   * already reflected in ->real_blend_enable */
#if 0
  if (!simple_property_equal (authorities0, authorities1,
                              pipelines_difference,
                              COGL_PIPELINE_STATE_BLEND_INDEX,
                              _cogl_pipeline_blend_enable_equal))
    return FALSE;
#endif

  if (!simple_property_equal (authorities0, authorities1,
                              pipelines_difference,
                              COGL_PIPELINE_STATE_DEPTH_INDEX,
                              _cogl_pipeline_depth_state_equal))
    goto done;

  if (!simple_property_equal (authorities0, authorities1,
                              pipelines_difference,
                              COGL_PIPELINE_STATE_FOG_INDEX,
                              _cogl_pipeline_fog_state_equal))
    goto done;

  if (!simple_property_equal (authorities0, authorities1,
                              pipelines_difference,
                              COGL_PIPELINE_STATE_CULL_FACE_INDEX,
                              _cogl_pipeline_cull_face_state_equal))
    goto done;

  if (!simple_property_equal (authorities0, authorities1,
                              pipelines_difference,
                              COGL_PIPELINE_STATE_POINT_SIZE_INDEX,
                              _cogl_pipeline_point_size_equal))
    goto done;

  if (!simple_property_equal (authorities0, authorities1,
                              pipelines_difference,
                              COGL_PIPELINE_STATE_LOGIC_OPS_INDEX,
                              _cogl_pipeline_logic_ops_state_equal))
    goto done;

  if (!simple_property_equal (authorities0, authorities1,
                              pipelines_difference,
                              COGL_PIPELINE_STATE_USER_SHADER_INDEX,
                              _cogl_pipeline_user_shader_equal))
    goto done;

  if (pipelines_difference & COGL_PIPELINE_STATE_LAYERS)
    {
      CoglPipelineStateIndex state_index = COGL_PIPELINE_STATE_LAYERS_INDEX;
      if (!_cogl_pipeline_layers_equal (authorities0[state_index],
                                        authorities1[state_index],
                                        layer_differences,
                                        flags))
        goto done;
    }

  ret = TRUE;
done:
  COGL_TIMER_STOP (_cogl_uprof_context, pipeline_equal_timer);
  return ret;
}

void
_cogl_pipeline_prune_redundant_ancestry (CoglPipeline *pipeline)
{
  CoglPipeline *new_parent = _cogl_pipeline_get_parent (pipeline);

  /* Before considering pruning redundant ancestry we check if this
   * pipeline is an authority for layer state and if so only consider
   * reparenting if it *owns* all the layers it depends on. NB: A
   * pipeline can be be a STATE_LAYERS authority but it may still
   * defer to its ancestors to define the state for some of its
   * layers.
   *
   * For example a pipeline that derives from a parent with 5 layers
   * can become a STATE_LAYERS authority by simply changing it's
   * ->n_layers count to 4 and in that case it can still defer to its
   * ancestors to define the state of those 4 layers.
   *
   * If a pipeline depends on any ancestors for layer state then we
   * immediatly bail out.
   */
  if (pipeline->differences & COGL_PIPELINE_STATE_LAYERS)
    {
      if (pipeline->n_layers != g_list_length (pipeline->layer_differences))
        return;
    }

  /* walk up past ancestors that are now redundant and potentially
   * reparent the pipeline. */
  while (_cogl_pipeline_get_parent (new_parent) &&
         (new_parent->differences | pipeline->differences) ==
          pipeline->differences)
    new_parent = _cogl_pipeline_get_parent (new_parent);

  if (new_parent != _cogl_pipeline_get_parent (pipeline))
    {
      gboolean is_weak = _cogl_pipeline_is_weak (pipeline);
      _cogl_pipeline_set_parent (pipeline, new_parent, is_weak ? FALSE : TRUE);
    }
}

void
_cogl_pipeline_update_authority (CoglPipeline *pipeline,
                                 CoglPipeline *authority,
                                 CoglPipelineState state,
                                 CoglPipelineStateComparitor comparitor)
{
  /* If we are the current authority see if we can revert to one of
   * our ancestors being the authority */
  if (pipeline == authority &&
      _cogl_pipeline_get_parent (authority) != NULL)
    {
      CoglPipeline *parent = _cogl_pipeline_get_parent (authority);
      CoglPipeline *old_authority =
        _cogl_pipeline_get_authority (parent, state);

      if (comparitor (authority, old_authority))
        pipeline->differences &= ~state;
    }
  else if (pipeline != authority)
    {
      /* If we weren't previously the authority on this state then we
       * need to extended our differences mask and so it's possible
       * that some of our ancestry will now become redundant, so we
       * aim to reparent ourselves if that's true... */
      pipeline->differences |= state;
      _cogl_pipeline_prune_redundant_ancestry (pipeline);
    }
}

gboolean
_cogl_pipeline_get_fog_enabled (CoglPipeline *pipeline)
{
  CoglPipeline *authority;

  g_return_val_if_fail (cogl_is_pipeline (pipeline), FALSE);

  authority =
    _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_FOG);
  return authority->big_state->fog_state.enabled;
}

unsigned long
_cogl_pipeline_get_age (CoglPipeline *pipeline)
{
  g_return_val_if_fail (cogl_is_pipeline (pipeline), 0);

  return pipeline->age;
}

static CoglPipelineLayer *
_cogl_pipeline_layer_copy (CoglPipelineLayer *src)
{
  CoglPipelineLayer *layer = g_slice_new (CoglPipelineLayer);

  _cogl_pipeline_node_init (COGL_PIPELINE_NODE (layer));

  layer->owner = NULL;
  layer->index = src->index;
  layer->differences = 0;
  layer->has_big_state = FALSE;

  _cogl_pipeline_layer_set_parent (layer, src);

  return _cogl_pipeline_layer_object_new (layer);
}

static void
_cogl_pipeline_layer_free (CoglPipelineLayer *layer)
{
  _cogl_pipeline_layer_unparent (COGL_PIPELINE_NODE (layer));

  if (layer->differences & COGL_PIPELINE_LAYER_STATE_TEXTURE_DATA &&
      layer->texture != NULL)
    cogl_object_unref (layer->texture);

  if (layer->differences & COGL_PIPELINE_LAYER_STATE_NEEDS_BIG_STATE)
    g_slice_free (CoglPipelineLayerBigState, layer->big_state);

  g_slice_free (CoglPipelineLayer, layer);
}

  /* If a layer has descendants we can't modify it freely
   *
   * If the layer is owned and the owner has descendants we can't
   * modify it freely.
   *
   * In both cases when we can't freely modify a layer we can either:
   * - create a new layer; splice it in to replace the layer so it can
   *   be directly modified.
   *   XXX: disadvantage is that we have to invalidate the layers_cache
   *   for the owner and its descendants.
   * - create a new derived layer and modify that.
   */

  /* XXX: how is the caller expected to deal with ref-counting?
   *
   * If the layer can't be freely modified and we return a new layer
   * then that will effectively make the caller own a new reference
   * which doesn't happen if we simply modify the given layer.
   *
   * We could make it consistent by taking a reference on the layer if
   * we don't create a new one. At least this way the caller could
   * deal with it consistently, though the semantics are a bit
   * strange.
   *
   * Alternatively we could leave it to the caller to check
   * ...?
   */

void
_cogl_pipeline_init_default_layers (void)
{
  CoglPipelineLayer *layer = g_slice_new0 (CoglPipelineLayer);
  CoglPipelineLayerBigState *big_state =
    g_slice_new0 (CoglPipelineLayerBigState);
  CoglPipelineLayer *new;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  _cogl_pipeline_node_init (COGL_PIPELINE_NODE (layer));

  layer->index = 0;

  layer->differences = COGL_PIPELINE_LAYER_STATE_ALL_SPARSE;

  layer->unit_index = 0;

  layer->texture = NULL;
  layer->target = 0;

  layer->mag_filter = COGL_PIPELINE_FILTER_LINEAR;
  layer->min_filter = COGL_PIPELINE_FILTER_LINEAR;

  layer->wrap_mode_s = COGL_PIPELINE_WRAP_MODE_AUTOMATIC;
  layer->wrap_mode_t = COGL_PIPELINE_WRAP_MODE_AUTOMATIC;
  layer->wrap_mode_p = COGL_PIPELINE_WRAP_MODE_AUTOMATIC;

  layer->big_state = big_state;
  layer->has_big_state = TRUE;

  /* Choose the same default combine mode as OpenGL:
   * RGBA = MODULATE(PREVIOUS[RGBA],TEXTURE[RGBA]) */
  big_state->texture_combine_rgb_func =
    COGL_PIPELINE_COMBINE_FUNC_MODULATE;
  big_state->texture_combine_rgb_src[0] =
    COGL_PIPELINE_COMBINE_SOURCE_PREVIOUS;
  big_state->texture_combine_rgb_src[1] =
    COGL_PIPELINE_COMBINE_SOURCE_TEXTURE;
  big_state->texture_combine_rgb_op[0] =
    COGL_PIPELINE_COMBINE_OP_SRC_COLOR;
  big_state->texture_combine_rgb_op[1] =
    COGL_PIPELINE_COMBINE_OP_SRC_COLOR;
  big_state->texture_combine_alpha_func =
    COGL_PIPELINE_COMBINE_FUNC_MODULATE;
  big_state->texture_combine_alpha_src[0] =
    COGL_PIPELINE_COMBINE_SOURCE_PREVIOUS;
  big_state->texture_combine_alpha_src[1] =
    COGL_PIPELINE_COMBINE_SOURCE_TEXTURE;
  big_state->texture_combine_alpha_op[0] =
    COGL_PIPELINE_COMBINE_OP_SRC_ALPHA;
  big_state->texture_combine_alpha_op[1] =
    COGL_PIPELINE_COMBINE_OP_SRC_ALPHA;

  big_state->point_sprite_coords = FALSE;

  cogl_matrix_init_identity (&big_state->matrix);

  ctx->default_layer_0 = _cogl_pipeline_layer_object_new (layer);

  /* TODO: we should make default_layer_n comprise of two
   * descendants of default_layer_0:
   * - the first descendant should change the texture combine
   *   to what we expect is most commonly used for multitexturing
   * - the second should revert the above change.
   *
   * why? the documentation for how a new layer is initialized
   * doesn't say that layers > 0 have different defaults so unless
   * we change the documentation we can't use different defaults,
   * but if the user does what we expect and changes the
   * texture combine then we can revert the authority to the
   * first descendant which means we can maximize the number
   * of layers with a common ancestor.
   *
   * The main problem will be that we'll need to disable the
   * optimizations for flattening the ancestry when we make
   * the second descendant which reverts the state.
   */
  ctx->default_layer_n = _cogl_pipeline_layer_copy (layer);
  new = _cogl_pipeline_set_layer_unit (NULL, ctx->default_layer_n, 1);
  g_assert (new == ctx->default_layer_n);
  /* Since we passed a newly allocated layer we don't expect that
   * _set_layer_unit() will have to allocate *another* layer. */

  /* Finally we create a dummy dependant for ->default_layer_n which
   * effectively ensures that ->default_layer_n and ->default_layer_0
   * remain immutable.
   */
  ctx->dummy_layer_dependant =
    _cogl_pipeline_layer_copy (ctx->default_layer_n);
}

void
cogl_pipeline_remove_layer (CoglPipeline *pipeline, int layer_index)
{
  CoglPipeline         *authority;
  CoglPipelineLayerInfo layer_info;
  int                   i;

  g_return_if_fail (cogl_is_pipeline (pipeline));

  authority =
    _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_LAYERS);

  /* The layer index of the layer we want info about */
  layer_info.layer_index = layer_index;

  /* This will be updated with a reference to the layer being removed
   * if it can be found. */
  layer_info.layer = NULL;

  /* This will be filled in with a list of layers that need to be
   * dropped down to a lower texture unit to fill the gap of the
   * removed layer. */
  layer_info.layers_to_shift =
    g_alloca (sizeof (CoglPipelineLayer *) * authority->n_layers);
  layer_info.n_layers_to_shift = 0;

  /* Unlike when we query layer info when adding a layer we must
   * always have a complete layers_to_shift list... */
  layer_info.ignore_shift_layers_if_found = FALSE;

  _cogl_pipeline_get_layer_info (authority, &layer_info);

  if (layer_info.layer == NULL)
    return;

  for (i = 0; i < layer_info.n_layers_to_shift; i++)
    {
      CoglPipelineLayer *shift_layer = layer_info.layers_to_shift[i];
      int unit_index = _cogl_pipeline_layer_get_unit_index (shift_layer);
      _cogl_pipeline_set_layer_unit (pipeline, shift_layer, unit_index - 1);
      /* NB: shift_layer may not be writeable so _set_layer_unit()
       * will allocate a derived layer internally which will become
       * owned by pipeline. Check the return value if we need to do
       * anything else with this layer. */
    }

  _cogl_pipeline_remove_layer_difference (pipeline, layer_info.layer, TRUE);
  _cogl_pipeline_try_reverting_layers_authority (pipeline, NULL);

  _cogl_pipeline_update_blend_enable (pipeline, COGL_PIPELINE_STATE_LAYERS);
}

static gboolean
prepend_layer_to_list_cb (CoglPipelineLayer *layer,
                          void *user_data)
{
  GList **layers = user_data;

  *layers = g_list_prepend (*layers, layer);
  return TRUE;
}

/* TODO: deprecate this API and replace it with
 * cogl_pipeline_foreach_layer
 * TODO: update the docs to note that if the user modifies any layers
 * then the list may become invalid.
 */
const GList *
_cogl_pipeline_get_layers (CoglPipeline *pipeline)
{
  g_return_val_if_fail (cogl_is_pipeline (pipeline), NULL);

  if (!pipeline->deprecated_get_layers_list_dirty)
    g_list_free (pipeline->deprecated_get_layers_list);

  pipeline->deprecated_get_layers_list = NULL;

  _cogl_pipeline_foreach_layer_internal (pipeline,
                                         prepend_layer_to_list_cb,
                                         &pipeline->deprecated_get_layers_list);
  pipeline->deprecated_get_layers_list =
    g_list_reverse (pipeline->deprecated_get_layers_list);

  pipeline->deprecated_get_layers_list_dirty = 0;

  return pipeline->deprecated_get_layers_list;
}

int
cogl_pipeline_get_n_layers (CoglPipeline *pipeline)
{
  CoglPipeline *authority;

  g_return_val_if_fail (cogl_is_pipeline (pipeline), 0);

  authority =
    _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_LAYERS);

  return authority->n_layers;
}

void
_cogl_pipeline_layer_pre_paint (CoglPipelineLayer *layer)
{
  CoglPipelineLayer *texture_authority;

  texture_authority =
    _cogl_pipeline_layer_get_authority (layer,
                                        COGL_PIPELINE_LAYER_STATE_TEXTURE_DATA);

  if (texture_authority->texture != COGL_INVALID_HANDLE)
    {
      CoglTexturePrePaintFlags flags = 0;
      CoglPipelineFilter min_filter;
      CoglPipelineFilter mag_filter;

      _cogl_pipeline_layer_get_filters (layer, &min_filter, &mag_filter);

      if (min_filter == COGL_PIPELINE_FILTER_NEAREST_MIPMAP_NEAREST
          || min_filter == COGL_PIPELINE_FILTER_LINEAR_MIPMAP_NEAREST
          || min_filter == COGL_PIPELINE_FILTER_NEAREST_MIPMAP_LINEAR
          || min_filter == COGL_PIPELINE_FILTER_LINEAR_MIPMAP_LINEAR)
        flags |= COGL_TEXTURE_NEEDS_MIPMAP;

      _cogl_texture_pre_paint (texture_authority->texture, flags);
    }
}

void
_cogl_pipeline_pre_paint_for_layer (CoglPipeline *pipeline,
                                    int layer_id)
{
  CoglPipelineLayer *layer = _cogl_pipeline_get_layer (pipeline, layer_id);
  _cogl_pipeline_layer_pre_paint (layer);
}

/* While a pipeline is referenced by the Cogl journal we can not allow
 * modifications, so this gives us a mechanism to track journal
 * references separately */
CoglPipeline *
_cogl_pipeline_journal_ref (CoglPipeline *pipeline)
{
  pipeline->journal_ref_count++;
  return cogl_object_ref (pipeline);
}

void
_cogl_pipeline_journal_unref (CoglPipeline *pipeline)
{
  pipeline->journal_ref_count--;
  cogl_object_unref (pipeline);
}

void
_cogl_pipeline_apply_legacy_state (CoglPipeline *pipeline)
{
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  /* It was a mistake that we ever copied the OpenGL style API for
   * associating these things directly with the context when we
   * originally wrote Cogl. Until the corresponding deprecated APIs
   * can be removed though we now shoehorn the state changes through
   * the cogl_pipeline API instead.
   */

  /* A program explicitly set on the pipeline has higher precedence than
   * one associated with the context using cogl_program_use() */
  if (ctx->current_program &&
      cogl_pipeline_get_user_program (pipeline) == COGL_INVALID_HANDLE)
    cogl_pipeline_set_user_program (pipeline, ctx->current_program);

  if (ctx->legacy_depth_test_enabled)
    {
      CoglDepthState depth_state;
      cogl_depth_state_init (&depth_state);
      cogl_depth_state_set_test_enabled (&depth_state, TRUE);
      cogl_pipeline_set_depth_state (pipeline, &depth_state, NULL);
    }

  if (ctx->legacy_fog_state.enabled)
    _cogl_pipeline_set_fog_state (pipeline, &ctx->legacy_fog_state);

  if (ctx->legacy_backface_culling_enabled)
    {
      CoglPipelineCullFaceState state;
      state.mode = COGL_PIPELINE_CULL_FACE_MODE_BACK;
      state.front_winding = COGL_WINDING_COUNTER_CLOCKWISE;
      _cogl_pipeline_set_cull_face_state (pipeline, &state);
    }
}

void
_cogl_pipeline_set_static_breadcrumb (CoglPipeline *pipeline,
                                      const char *breadcrumb)
{
  pipeline->has_static_breadcrumb = TRUE;
  pipeline->static_breadcrumb = breadcrumb;
}

typedef void (*LayerStateHashFunction) (CoglPipelineLayer *authority,
                                        CoglPipelineLayer **authorities,
                                        CoglPipelineHashState *state);

static LayerStateHashFunction
layer_state_hash_functions[COGL_PIPELINE_LAYER_STATE_SPARSE_COUNT];

/* XXX: We don't statically initialize the array of hash functions, so
 * we won't get caught out by later re-indexing the groups for some
 * reason. */
void
_cogl_pipeline_init_layer_state_hash_functions (void)
{
  CoglPipelineLayerStateIndex _index;
  layer_state_hash_functions[COGL_PIPELINE_LAYER_STATE_UNIT_INDEX] =
    _cogl_pipeline_layer_hash_unit_state;
  layer_state_hash_functions[COGL_PIPELINE_LAYER_STATE_TEXTURE_TARGET_INDEX] =
    _cogl_pipeline_layer_hash_texture_target_state;
  layer_state_hash_functions[COGL_PIPELINE_LAYER_STATE_TEXTURE_DATA_INDEX] =
    _cogl_pipeline_layer_hash_texture_data_state;
  layer_state_hash_functions[COGL_PIPELINE_LAYER_STATE_FILTERS_INDEX] =
    _cogl_pipeline_layer_hash_filters_state;
  layer_state_hash_functions[COGL_PIPELINE_LAYER_STATE_WRAP_MODES_INDEX] =
    _cogl_pipeline_layer_hash_wrap_modes_state;
  layer_state_hash_functions[COGL_PIPELINE_LAYER_STATE_COMBINE_INDEX] =
    _cogl_pipeline_layer_hash_combine_state;
  layer_state_hash_functions[COGL_PIPELINE_LAYER_STATE_COMBINE_CONSTANT_INDEX] =
    _cogl_pipeline_layer_hash_combine_constant_state;
  layer_state_hash_functions[COGL_PIPELINE_LAYER_STATE_USER_MATRIX_INDEX] =
    _cogl_pipeline_layer_hash_user_matrix_state;
  _index = COGL_PIPELINE_LAYER_STATE_POINT_SPRITE_COORDS_INDEX;
  layer_state_hash_functions[_index] =
    _cogl_pipeline_layer_hash_point_sprite_state;

  /* So we get a big error if we forget to update this code! */
  g_assert (COGL_PIPELINE_LAYER_STATE_SPARSE_COUNT == 9);
}

static gboolean
_cogl_pipeline_hash_layer_cb (CoglPipelineLayer *layer,
                              void *user_data)
{
  CoglPipelineHashState *state = user_data;
  unsigned long differences = state->layer_differences;
  CoglPipelineLayer *authorities[COGL_PIPELINE_LAYER_STATE_COUNT];
  unsigned long mask;
  int i;

  /* Theoretically we would hash non-sparse layer state here but
   * currently layers don't have any. */

  /* XXX: we resolve all the authorities here - not just those
   * corresponding to hash_state->layer_differences - because
   * the hashing of some state groups actually depends on the values
   * in other groups. For example we don't hash layer combine
   * constants if they are aren't referenced by the current layer
   * combine function.
   */
  mask = COGL_PIPELINE_LAYER_STATE_ALL_SPARSE;
  _cogl_pipeline_layer_resolve_authorities (layer,
                                            mask,
                                            authorities);

  /* So we go right ahead and hash the sparse state... */
  for (i = 0; i < COGL_PIPELINE_LAYER_STATE_COUNT; i++)
    {
      unsigned long current_state = (1L<<i);

      /* XXX: we are hashing the un-mixed hash values of all the
       * individual state groups; we should provide a means to test
       * the quality of the final hash values we are getting with this
       * approach... */
      if (differences & current_state)
        {
          CoglPipelineLayer *authority = authorities[i];
          layer_state_hash_functions[i] (authority, authorities, state);
        }

      if (current_state > differences)
        break;
    }

  return TRUE;
}

void
_cogl_pipeline_hash_layers_state (CoglPipeline *authority,
                                  CoglPipelineHashState *state)
{
  state->hash =
    _cogl_util_one_at_a_time_hash (state->hash, &authority->n_layers,
                                   sizeof (authority->n_layers));
  _cogl_pipeline_foreach_layer_internal (authority,
                                         _cogl_pipeline_hash_layer_cb,
                                         state);
}

typedef void (*StateHashFunction) (CoglPipeline *authority, CoglPipelineHashState *state);

static StateHashFunction
state_hash_functions[COGL_PIPELINE_STATE_SPARSE_COUNT];

/* We don't statically initialize the array of hash functions
 * so we won't get caught out by later re-indexing the groups for
 * some reason. */
void
_cogl_pipeline_init_state_hash_functions (void)
{
  state_hash_functions[COGL_PIPELINE_STATE_COLOR_INDEX] =
    _cogl_pipeline_hash_color_state;
  state_hash_functions[COGL_PIPELINE_STATE_BLEND_ENABLE_INDEX] =
    _cogl_pipeline_hash_blend_enable_state;
  state_hash_functions[COGL_PIPELINE_STATE_LAYERS_INDEX] =
    _cogl_pipeline_hash_layers_state;
  state_hash_functions[COGL_PIPELINE_STATE_LIGHTING_INDEX] =
    _cogl_pipeline_hash_lighting_state;
  state_hash_functions[COGL_PIPELINE_STATE_ALPHA_FUNC_INDEX] =
    _cogl_pipeline_hash_alpha_func_state;
  state_hash_functions[COGL_PIPELINE_STATE_ALPHA_FUNC_REFERENCE_INDEX] =
    _cogl_pipeline_hash_alpha_func_reference_state;
  state_hash_functions[COGL_PIPELINE_STATE_BLEND_INDEX] =
    _cogl_pipeline_hash_blend_state;
  state_hash_functions[COGL_PIPELINE_STATE_USER_SHADER_INDEX] =
    _cogl_pipeline_hash_user_shader_state;
  state_hash_functions[COGL_PIPELINE_STATE_DEPTH_INDEX] =
    _cogl_pipeline_hash_depth_state;
  state_hash_functions[COGL_PIPELINE_STATE_FOG_INDEX] =
    _cogl_pipeline_hash_fog_state;
  state_hash_functions[COGL_PIPELINE_STATE_CULL_FACE_INDEX] =
    _cogl_pipeline_hash_cull_face_state;
  state_hash_functions[COGL_PIPELINE_STATE_POINT_SIZE_INDEX] =
    _cogl_pipeline_hash_point_size_state;
  state_hash_functions[COGL_PIPELINE_STATE_LOGIC_OPS_INDEX] =
    _cogl_pipeline_hash_logic_ops_state;

  /* So we get a big error if we forget to update this code! */
  g_assert (COGL_PIPELINE_STATE_SPARSE_COUNT == 13);
}

unsigned int
_cogl_pipeline_hash (CoglPipeline *pipeline,
                     unsigned long differences,
                     unsigned long layer_differences,
                     CoglPipelineEvalFlags flags)
{
  CoglPipeline *authorities[COGL_PIPELINE_STATE_SPARSE_COUNT];
  unsigned long mask;
  int i;
  CoglPipelineHashState state;
  unsigned int final_hash = 0;

  state.hash = 0;
  state.layer_differences = layer_differences;
  state.flags = flags;

  /* hash non-sparse state */

  if (differences & COGL_PIPELINE_STATE_REAL_BLEND_ENABLE)
    {
      gboolean enable = pipeline->real_blend_enable;
      state.hash =
        _cogl_util_one_at_a_time_hash (state.hash, &enable, sizeof (enable));
    }

  /* hash sparse state */

  mask = differences & COGL_PIPELINE_STATE_ALL_SPARSE;
  _cogl_pipeline_resolve_authorities (pipeline, mask, authorities);

  for (i = 0; i < COGL_PIPELINE_STATE_SPARSE_COUNT; i++)
    {
      unsigned long current_state = (1L<<i);

      /* XXX: we are hashing the un-mixed hash values of all the
       * individual state groups; we should provide a means to test
       * the quality of the final hash values we are getting with this
       * approach... */
      if (differences & current_state)
        {
          CoglPipeline *authority = authorities[i];
          state_hash_functions[i] (authority, &state);
          final_hash = _cogl_util_one_at_a_time_hash (final_hash, &state.hash,
                                                      sizeof (state.hash));
        }

      if (current_state > differences)
        break;
    }

  return _cogl_util_one_at_a_time_mix (final_hash);
}

typedef struct
{
  int parent_id;
  int *node_id_ptr;
  GString *graph;
  int indent;
} PrintDebugState;

static gboolean
dump_layer_cb (CoglPipelineNode *node, void *user_data)
{
  CoglPipelineLayer *layer = COGL_PIPELINE_LAYER (node);
  PrintDebugState *state = user_data;
  int layer_id = *state->node_id_ptr;
  PrintDebugState state_out;
  GString *changes_label;
  gboolean changes = FALSE;

  if (state->parent_id >= 0)
    g_string_append_printf (state->graph, "%*slayer%p -> layer%p;\n",
                            state->indent, "",
                            layer->_parent.parent,
                            layer);

  g_string_append_printf (state->graph,
                          "%*slayer%p [label=\"layer=0x%p\\n"
                          "ref count=%d\" "
                          "color=\"blue\"];\n",
                          state->indent, "",
                          layer,
                          layer,
                          COGL_OBJECT (layer)->ref_count);

  changes_label = g_string_new ("");
  g_string_append_printf (changes_label,
                          "%*slayer%p -> layer_state%d [weight=100];\n"
                          "%*slayer_state%d [shape=box label=\"",
                          state->indent, "",
                          layer,
                          layer_id,
                          state->indent, "",
                          layer_id);

  if (layer->differences & COGL_PIPELINE_LAYER_STATE_UNIT)
    {
      changes = TRUE;
      g_string_append_printf (changes_label,
                              "\\lunit=%u\\n",
                              layer->unit_index);
    }

  if (layer->differences & COGL_PIPELINE_LAYER_STATE_TEXTURE_DATA)
    {
      changes = TRUE;
      g_string_append_printf (changes_label,
                              "\\ltexture=%p\\n",
                              layer->texture);
    }

  if (changes)
    {
      g_string_append_printf (changes_label, "\"];\n");
      g_string_append (state->graph, changes_label->str);
      g_string_free (changes_label, TRUE);
    }

  state_out.parent_id = layer_id;

  state_out.node_id_ptr = state->node_id_ptr;
  (*state_out.node_id_ptr)++;

  state_out.graph = state->graph;
  state_out.indent = state->indent + 2;

  _cogl_pipeline_node_foreach_child (COGL_PIPELINE_NODE (layer),
                                     dump_layer_cb,
                                     &state_out);

  return TRUE;
}

static gboolean
dump_layer_ref_cb (CoglPipelineLayer *layer, void *data)
{
  PrintDebugState *state = data;
  int pipeline_id = *state->node_id_ptr;

  g_string_append_printf (state->graph,
                          "%*spipeline_state%d -> layer%p;\n",
                          state->indent, "",
                          pipeline_id,
                          layer);

  return TRUE;
}

static gboolean
dump_pipeline_cb (CoglPipelineNode *node, void *user_data)
{
  CoglPipeline *pipeline = COGL_PIPELINE (node);
  PrintDebugState *state = user_data;
  int pipeline_id = *state->node_id_ptr;
  PrintDebugState state_out;
  GString *changes_label;
  gboolean changes = FALSE;
  gboolean layers = FALSE;

  if (state->parent_id >= 0)
    g_string_append_printf (state->graph, "%*spipeline%d -> pipeline%d;\n",
                            state->indent, "",
                            state->parent_id,
                            pipeline_id);

  g_string_append_printf (state->graph,
                          "%*spipeline%d [label=\"pipeline=0x%p\\n"
                          "ref count=%d\\n"
                          "breadcrumb=\\\"%s\\\"\" color=\"red\"];\n",
                          state->indent, "",
                          pipeline_id,
                          pipeline,
                          COGL_OBJECT (pipeline)->ref_count,
                          pipeline->has_static_breadcrumb ?
                          pipeline->static_breadcrumb : "NULL");

  changes_label = g_string_new ("");
  g_string_append_printf (changes_label,
                          "%*spipeline%d -> pipeline_state%d [weight=100];\n"
                          "%*spipeline_state%d [shape=box label=\"",
                          state->indent, "",
                          pipeline_id,
                          pipeline_id,
                          state->indent, "",
                          pipeline_id);


  if (pipeline->differences & COGL_PIPELINE_STATE_COLOR)
    {
      changes = TRUE;
      g_string_append_printf (changes_label,
                              "\\lcolor=0x%02X%02X%02X%02X\\n",
                              cogl_color_get_red_byte (&pipeline->color),
                              cogl_color_get_green_byte (&pipeline->color),
                              cogl_color_get_blue_byte (&pipeline->color),
                              cogl_color_get_alpha_byte (&pipeline->color));
    }

  if (pipeline->differences & COGL_PIPELINE_STATE_BLEND)
    {
      const char *blend_enable_name;

      changes = TRUE;

      switch (pipeline->blend_enable)
        {
        case COGL_PIPELINE_BLEND_ENABLE_AUTOMATIC:
          blend_enable_name = "AUTO";
          break;
        case COGL_PIPELINE_BLEND_ENABLE_ENABLED:
          blend_enable_name = "ENABLED";
          break;
        case COGL_PIPELINE_BLEND_ENABLE_DISABLED:
          blend_enable_name = "DISABLED";
          break;
        default:
          blend_enable_name = "UNKNOWN";
        }
      g_string_append_printf (changes_label,
                              "\\lblend=%s\\n",
                              blend_enable_name);
    }

  if (pipeline->differences & COGL_PIPELINE_STATE_LAYERS)
    {
      changes = TRUE;
      layers = TRUE;
      g_string_append_printf (changes_label, "\\ln_layers=%d\\n",
                              pipeline->n_layers);
    }

  if (changes)
    {
      g_string_append_printf (changes_label, "\"];\n");
      g_string_append (state->graph, changes_label->str);
      g_string_free (changes_label, TRUE);
    }

  if (layers)
    {
      g_list_foreach (pipeline->layer_differences,
                      (GFunc)dump_layer_ref_cb,
                      state);
    }

  state_out.parent_id = pipeline_id;

  state_out.node_id_ptr = state->node_id_ptr;
  (*state_out.node_id_ptr)++;

  state_out.graph = state->graph;
  state_out.indent = state->indent + 2;

  _cogl_pipeline_node_foreach_child (COGL_PIPELINE_NODE (pipeline),
                                     dump_pipeline_cb,
                                     &state_out);

  return TRUE;
}

void
_cogl_debug_dump_pipelines_dot_file (const char *filename)
{
  GString *graph;
  PrintDebugState layer_state;
  PrintDebugState pipeline_state;
  int layer_id = 0;
  int pipeline_id = 0;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  if (!ctx->default_pipeline)
    return;

  graph = g_string_new ("");
  g_string_append_printf (graph, "digraph {\n");

  layer_state.graph = graph;
  layer_state.parent_id = -1;
  layer_state.node_id_ptr = &layer_id;
  layer_state.indent = 0;
  dump_layer_cb (ctx->default_layer_0, &layer_state);

  pipeline_state.graph = graph;
  pipeline_state.parent_id = -1;
  pipeline_state.node_id_ptr = &pipeline_id;
  pipeline_state.indent = 0;
  dump_pipeline_cb (ctx->default_pipeline, &pipeline_state);

  g_string_append_printf (graph, "}\n");

  if (filename)
    g_file_set_contents (filename, graph->str, -1, NULL);
  else
    g_print ("%s", graph->str);

  g_string_free (graph, TRUE);
}

typedef struct
{
  int i;
  CoglPipelineLayer **layers;
} AddLayersToArrayState;

static gboolean
add_layer_to_array_cb (CoglPipelineLayer *layer,
                       void *user_data)
{
  AddLayersToArrayState *state = user_data;
  state->layers[state->i++] = layer;
  return TRUE;
}

/* Determines if we need to handle the RGB and A texture combining
 * separately or is the same function used for both channel masks and
 * with the same arguments...
 */
gboolean
_cogl_pipeline_need_texture_combine_separate
                                       (CoglPipelineLayer *combine_authority)
{
  CoglPipelineLayerBigState *big_state = combine_authority->big_state;
  int n_args;
  int i;

  if (big_state->texture_combine_rgb_func !=
      big_state->texture_combine_alpha_func)
    return TRUE;

  n_args = _cogl_get_n_args_for_combine_func (big_state->texture_combine_rgb_func);

  for (i = 0; i < n_args; i++)
    {
      if (big_state->texture_combine_rgb_src[i] !=
          big_state->texture_combine_alpha_src[i])
        return TRUE;

      /*
       * We can allow some variation of the source operands without
       * needing a separation...
       *
       * "A = REPLACE (CONSTANT[A])" + either of the following...
       * "RGB = REPLACE (CONSTANT[RGB])"
       * "RGB = REPLACE (CONSTANT[A])"
       *
       * can be combined as:
       * "RGBA = REPLACE (CONSTANT)" or
       * "RGBA = REPLACE (CONSTANT[A])" or
       *
       * And "A = REPLACE (1-CONSTANT[A])" + either of the following...
       * "RGB = REPLACE (1-CONSTANT)" or
       * "RGB = REPLACE (1-CONSTANT[A])"
       *
       * can be combined as:
       * "RGBA = REPLACE (1-CONSTANT)" or
       * "RGBA = REPLACE (1-CONSTANT[A])"
       */
      switch (big_state->texture_combine_alpha_op[i])
        {
        case GL_SRC_ALPHA:
          switch (big_state->texture_combine_rgb_op[i])
            {
            case GL_SRC_COLOR:
            case GL_SRC_ALPHA:
              break;
            default:
              return FALSE;
            }
          break;
        case GL_ONE_MINUS_SRC_ALPHA:
          switch (big_state->texture_combine_rgb_op[i])
            {
            case GL_ONE_MINUS_SRC_COLOR:
            case GL_ONE_MINUS_SRC_ALPHA:
              break;
            default:
              return FALSE;
            }
          break;
        default:
          return FALSE;	/* impossible */
        }
    }

   return FALSE;
}

/* This tries to find the oldest ancestor whose pipeline and layer
   state matches the given flags. This is mostly used to detect code
   gen authorities so that we can reduce the numer of programs
   generated */
CoglPipeline *
_cogl_pipeline_find_equivalent_parent (CoglPipeline *pipeline,
                                       CoglPipelineState pipeline_state,
                                       CoglPipelineLayerState layer_state)
{
  CoglPipeline *authority0;
  CoglPipeline *authority1;
  int n_layers;
  CoglPipelineLayer **authority0_layers;
  CoglPipelineLayer **authority1_layers;

  /* Find the first pipeline that modifies state that affects the
   * state or any layer state... */
  authority0 = _cogl_pipeline_get_authority (pipeline,
                                             pipeline_state |
                                             COGL_PIPELINE_STATE_LAYERS);

  /* Find the next ancestor after that, that also modifies the
   * state... */
  if (_cogl_pipeline_get_parent (authority0))
    {
      authority1 =
        _cogl_pipeline_get_authority (_cogl_pipeline_get_parent (authority0),
                                      pipeline_state |
                                      COGL_PIPELINE_STATE_LAYERS);
    }
  else
    return authority0;

  n_layers = cogl_pipeline_get_n_layers (authority0);

  for (;;)
    {
      AddLayersToArrayState state;
      int i;

      if (n_layers != cogl_pipeline_get_n_layers (authority1))
        return authority0;

      /* If the programs differ by anything that isn't part of the
         layer state then we can't continue */
      if (pipeline_state &&
          (_cogl_pipeline_compare_differences (authority0, authority1) &
           pipeline_state))
        return authority0;

      authority0_layers =
        g_alloca (sizeof (CoglPipelineLayer *) * n_layers);
      state.i = 0;
      state.layers = authority0_layers;
      _cogl_pipeline_foreach_layer_internal (authority0,
                                             add_layer_to_array_cb,
                                             &state);

      authority1_layers =
        g_alloca (sizeof (CoglPipelineLayer *) * n_layers);
      state.i = 0;
      state.layers = authority1_layers;
      _cogl_pipeline_foreach_layer_internal (authority1,
                                             add_layer_to_array_cb,
                                             &state);

      for (i = 0; i < n_layers; i++)
        {
          unsigned long layer_differences;

          if (authority0_layers[i] == authority1_layers[i])
            continue;

          layer_differences =
            _cogl_pipeline_layer_compare_differences (authority0_layers[i],
                                                      authority1_layers[i]);

          if (layer_differences & layer_state)
            return authority0;
        }

      /* Find the next ancestor after that, that also modifies state
       * affecting codegen... */

      if (!_cogl_pipeline_get_parent (authority1))
        break;

      authority0 = authority1;
      authority1 =
        _cogl_pipeline_get_authority (_cogl_pipeline_get_parent (authority1),
                                      pipeline_state |
                                      COGL_PIPELINE_STATE_LAYERS);
      if (authority1 == authority0)
        break;
    }

  return authority1;
}

CoglPipelineLayerState
_cogl_pipeline_get_layer_state_for_fragment_codegen (CoglContext *context)
{
  CoglPipelineLayerState state =
    (COGL_PIPELINE_LAYER_STATE_COMBINE |
     COGL_PIPELINE_LAYER_STATE_TEXTURE_TARGET |
     COGL_PIPELINE_LAYER_STATE_POINT_SPRITE_COORDS |
     COGL_PIPELINE_LAYER_STATE_UNIT);

  if (context->driver == COGL_DRIVER_GLES2)
    state |= COGL_PIPELINE_LAYER_STATE_POINT_SPRITE_COORDS;

  return state;
}

CoglPipelineState
_cogl_pipeline_get_state_for_fragment_codegen (CoglContext *context)
{
  CoglPipelineState state = (COGL_PIPELINE_STATE_LAYERS |
                             COGL_PIPELINE_STATE_USER_SHADER);

  if (context->driver == COGL_DRIVER_GLES2)
    state |= COGL_PIPELINE_STATE_ALPHA_FUNC;

  return state;
}
