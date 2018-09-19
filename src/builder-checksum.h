/*
 * Copyright © 2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#ifndef __BUILDER_CHECKSUM_H__
#define __BUILDER_CHECKSUM_H__

#include <gio/gio.h>
#include <libglnx/libglnx.h>

G_BEGIN_DECLS

void builder_checksum_str            (GChecksum  *checksum,
                                      const char    *str);
void builder_checksum_compat_str     (GChecksum  *checksum,
                                      const char    *str);
void builder_checksum_strv           (GChecksum  *checksum,
                                      char         **strv);
void builder_checksum_compat_strv    (GChecksum  *checksum,
                                      char         **strv);
void builder_checksum_boolean        (GChecksum  *checksum,
                                      gboolean       val);
void builder_checksum_compat_boolean (GChecksum  *checksum,
                                      gboolean       val);
void builder_checksum_uint32         (GChecksum  *checksum,
                                      guint32        val);
void builder_checksum_uint64         (GChecksum  *checksum,
                                      guint64        val);
void builder_checksum_data           (GChecksum  *checksum,
                                      guint8        *data,
                                      gsize          len);
void builder_checksum_random         (GChecksum  *checksum);

G_END_DECLS

#endif /* __BUILDER_CACHE_H__ */
