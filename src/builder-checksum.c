/* builder-checksum.c
 *
 * Copyright (C) 2018 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include "builder-checksum.h"

/* Only add to cache if non-empty. This means we can add
   these things compatibly without invalidating the cache.
   This is useful if empty means no change from what was
   before */
void
builder_checksum_compat_str (GChecksum *checksum,
                             const char   *str)
{
  if (str)
    builder_checksum_str (checksum, str);
}

void
builder_checksum_str (GChecksum *checksum,
                      const char   *str)
{
  /* We include the terminating zero so that we make
   * a difference between NULL and "". */

  if (str)
    g_checksum_update (checksum, (const guchar *) str, strlen (str) + 1);
  else
    /* Always add something so we can't be fooled by a sequence like
       NULL, "a" turning into "a", NULL. */
    g_checksum_update (checksum, (const guchar *) "\1", 1);
}

/* Only add to cache if non-empty. This means we can add
   these things compatibly without invalidating the cache.
   This is useful if empty means no change from what was
   before */
void
builder_checksum_compat_strv (GChecksum *checksum,
                              char        **strv)
{
  if (strv != NULL && strv[0] != NULL)
    builder_checksum_strv (checksum, strv);
}


void
builder_checksum_strv (GChecksum *checksum,
                       char        **strv)
{
  int i;

  if (strv)
    {
      g_checksum_update (checksum, (const guchar *) "\1", 1);
      for (i = 0; strv[i] != NULL; i++)
        builder_checksum_str (checksum, strv[i]);
    }
  else
    {
      g_checksum_update (checksum, (const guchar *) "\2", 1);
    }
}

void
builder_checksum_boolean (GChecksum *checksum,
                          gboolean      val)
{
  if (val)
    g_checksum_update (checksum, (const guchar *) "\1", 1);
  else
    g_checksum_update (checksum, (const guchar *) "\0", 1);
}

/* Only add to cache if true. This means we can add
   these things compatibly without invalidating the cache.
   This is useful if false means no change from what was
   before */
void
builder_checksum_compat_boolean (GChecksum *checksum,
                                 gboolean      val)
{
  if (val)
    builder_checksum_boolean (checksum, val);
}

void
builder_checksum_uint32 (GChecksum *checksum,
                         guint32       val)
{
  guchar v[4];

  v[0] = (val >> 0) & 0xff;
  v[1] = (val >> 8) & 0xff;
  v[2] = (val >> 16) & 0xff;
  v[3] = (val >> 24) & 0xff;
  g_checksum_update (checksum, v, 4);
}

void
builder_checksum_random (GChecksum *checksum)
{
  builder_checksum_uint32 (checksum, g_random_int ());
  builder_checksum_uint32 (checksum, g_random_int ());
}

void
builder_checksum_uint64 (GChecksum *checksum,
                         guint64       val)
{
  guchar v[8];

  v[0] = (val >> 0) & 0xff;
  v[1] = (val >> 8) & 0xff;
  v[2] = (val >> 16) & 0xff;
  v[3] = (val >> 24) & 0xff;
  v[4] = (val >> 32) & 0xff;
  v[5] = (val >> 40) & 0xff;
  v[6] = (val >> 48) & 0xff;
  v[7] = (val >> 56) & 0xff;

  g_checksum_update (checksum, v, 8);
}

void
builder_checksum_data (GChecksum *checksum,
                       guint8       *data,
                       gsize         len)
{
  g_checksum_update (checksum, data, len);
}
