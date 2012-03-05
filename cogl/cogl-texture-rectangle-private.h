/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2009 Intel Corporation.
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
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#ifndef __COGL_TEXTURE_RECTANGLE_H
#define __COGL_TEXTURE_RECTANGLE_H

#include "cogl-handle.h"
#include "cogl-pipeline-private.h"
#include "cogl-texture-private.h"

#define COGL_TEXTURE_RECTANGLE(tex) ((CoglTextureRectangle *) tex)

typedef struct _CoglTextureRectangle CoglTextureRectangle;

struct _CoglTextureRectangle
{
  CoglTexture     _parent;

  /* The internal format of the GL texture represented as a
     CoglPixelFormat */
  CoglPixelFormat format;
  /* The internal format of the GL texture represented as a GL enum */
  GLenum          gl_format;
  /* The texture object number */
  GLuint          gl_texture;
  int             width;
  int             height;
  GLenum          min_filter;
  GLenum          mag_filter;
  GLint           wrap_mode_s;
  GLint           wrap_mode_t;
  gboolean        is_foreign;
};

GQuark
_cogl_handle_texture_rectangle_get_type (void);

CoglHandle
_cogl_texture_rectangle_new_with_size (unsigned int     width,
                                       unsigned int     height,
                                       CoglTextureFlags flags,
                                       CoglPixelFormat  internal_format);

CoglHandle
_cogl_texture_rectangle_new_from_bitmap (CoglBitmap      *bmp,
                                         CoglTextureFlags flags,
                                         CoglPixelFormat  internal_format);

CoglHandle
_cogl_texture_rectangle_new_from_foreign (GLuint gl_handle,
                                          GLuint width,
                                          GLuint height,
                                          CoglPixelFormat format);

#endif /* __COGL_TEXTURE_RECTANGLE_H */
