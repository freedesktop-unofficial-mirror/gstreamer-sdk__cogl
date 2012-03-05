/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2010 Intel Corporation.
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
 *
 * Authors:
 *   Neil Roberts <neil@linux.intel.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <string.h>

#include "cogl-bitmask.h"

gboolean
_cogl_bitmask_get_from_array (const CoglBitmask *bitmask,
                              unsigned int bit_num)
{
  GArray *array = (GArray *) *bitmask;

  /* If the index is off the end of the array then assume the bit is
     not set */
  if (bit_num >= sizeof (unsigned int) * 8 * array->len)
    return FALSE;
  else
    return !!(g_array_index (array, unsigned int,
                             bit_num / (sizeof (unsigned int) * 8)) &
              (1 << (bit_num % (sizeof (unsigned int) * 8))));
}

static void
_cogl_bitmask_convert_to_array (CoglBitmask *bitmask)
{
  GArray *array;
  /* Fetch the old values, ignoring the least significant bit */
  unsigned int old_values = GPOINTER_TO_UINT (*bitmask) >> 1;

  array = g_array_new (FALSE, TRUE, sizeof (unsigned int));
  /* Copy the old values back in */
  g_array_append_val (array, old_values);

  *bitmask = (struct _CoglBitmaskImaginaryType *) array;
}

void
_cogl_bitmask_set_in_array (CoglBitmask *bitmask,
                            unsigned int bit_num,
                            gboolean value)
{
  GArray *array;
  unsigned int array_index, new_value_mask;

  /* If the bitmask is not already an array then we need to allocate one */
  if (!_cogl_bitmask_has_array (bitmask))
    _cogl_bitmask_convert_to_array (bitmask);

  array = (GArray *) *bitmask;

  array_index = bit_num / (sizeof (unsigned int) * 8);
  /* Grow the array if necessary. This will clear the new data */
  if (array_index >= array->len)
    g_array_set_size (array, array_index + 1);

  new_value_mask = 1 << (bit_num % (sizeof (unsigned int) * 8));

  if (value)
    g_array_index (array, unsigned int, array_index) |= new_value_mask;
  else
    g_array_index (array, unsigned int, array_index) &= ~new_value_mask;
}

void
_cogl_bitmask_set_bits (CoglBitmask *dst,
                        const CoglBitmask *src)
{
  if (_cogl_bitmask_has_array (src))
    {
      GArray *src_array, *dst_array;
      int i;

      if (!_cogl_bitmask_has_array (dst))
        _cogl_bitmask_convert_to_array (dst);

      dst_array = (GArray *) *dst;
      src_array = (GArray *) *src;

      if (dst_array->len < src_array->len)
        g_array_set_size (dst_array, src_array->len);

      for (i = 0; i < src_array->len; i++)
        g_array_index (dst_array, unsigned int, i) |=
          g_array_index (src_array, unsigned int, i);
    }
  else if (_cogl_bitmask_has_array (dst))
    {
      GArray *dst_array;

      dst_array = (GArray *) *dst;

      g_array_index (dst_array, unsigned int, 0) |=
        (GPOINTER_TO_UINT (*src) >> 1);
    }
  else
    *dst = GUINT_TO_POINTER (GPOINTER_TO_UINT (*dst) |
                             GPOINTER_TO_UINT (*src));
}

void
_cogl_bitmask_set_range_in_array (CoglBitmask *bitmask,
                                  unsigned int n_bits,
                                  gboolean value)
{
  GArray *array;
  unsigned int array_index, bit_index;

  if (n_bits == 0)
    return;

  /* If the bitmask is not already an array then we need to allocate one */
  if (!_cogl_bitmask_has_array (bitmask))
    _cogl_bitmask_convert_to_array (bitmask);

  array = (GArray *) *bitmask;

  /* Get the array index of the top most value that will be touched */
  array_index = (n_bits - 1) / (sizeof (unsigned int) * 8);
  /* Get the bit index of the top most value */
  bit_index = (n_bits - 1) % (sizeof (unsigned int) * 8);
  /* Grow the array if necessary. This will clear the new data */
  if (array_index >= array->len)
    g_array_set_size (array, array_index + 1);

  if (value)
    {
      /* Set the bits that are touching this index */
      g_array_index (array, unsigned int, array_index) |=
        ~(unsigned int) 0 >> (sizeof (unsigned int) * 8 - 1 - bit_index);

      /* Set all of the bits in any lesser indices */
      memset (array->data, 0xff, sizeof (unsigned int) * array_index);
    }
  else
    {
      /* Clear the bits that are touching this index */
      g_array_index (array, unsigned int, array_index) &=
        ~(unsigned int) 1 << bit_index;

      /* Clear all of the bits in any lesser indices */
      memset (array->data, 0x00, sizeof (unsigned int) * array_index);
    }
}

void
_cogl_bitmask_xor_bits (CoglBitmask *dst,
                        const CoglBitmask *src)
{
  if (_cogl_bitmask_has_array (src))
    {
      GArray *src_array, *dst_array;
      int i;

      if (!_cogl_bitmask_has_array (dst))
        _cogl_bitmask_convert_to_array (dst);

      dst_array = (GArray *) *dst;
      src_array = (GArray *) *src;

      if (dst_array->len < src_array->len)
        g_array_set_size (dst_array, src_array->len);

      for (i = 0; i < src_array->len; i++)
        g_array_index (dst_array, unsigned int, i) ^=
          g_array_index (src_array, unsigned int, i);
    }
  else if (_cogl_bitmask_has_array (dst))
    {
      GArray *dst_array;

      dst_array = (GArray *) *dst;

      g_array_index (dst_array, unsigned int, 0) ^=
        (GPOINTER_TO_UINT (*src) >> 1);
    }
  else
    *dst = GUINT_TO_POINTER ((GPOINTER_TO_UINT (*dst) ^
                              GPOINTER_TO_UINT (*src)) | 1);
}

void
_cogl_bitmask_clear_all_in_array (CoglBitmask *bitmask)
{
  GArray *array = (GArray *) *bitmask;

  memset (array->data, 0, sizeof (unsigned int) * array->len);
}

void
_cogl_bitmask_foreach (const CoglBitmask *bitmask,
                       CoglBitmaskForeachFunc func,
                       gpointer user_data)
{
  if (_cogl_bitmask_has_array (bitmask))
    {
      GArray *array = (GArray *) *bitmask;
      int array_index;

      for (array_index = 0; array_index < array->len; array_index++)
        {
          unsigned int mask = g_array_index (array, unsigned int, array_index);
          int bit = 0;

          while (mask)
            {
              if (mask & 1)
                func (array_index * sizeof (unsigned int) * 8 + bit, user_data);

              bit++;
              mask >>= 1;
            }
        }
    }
  else
    {
      unsigned int mask = GPOINTER_TO_UINT (*bitmask) >> 1;
      int bit = 0;

      while (mask)
        {
          if (mask & 1)
            func (bit, user_data);

          bit++;
          mask >>= 1;
        }
    }
}
