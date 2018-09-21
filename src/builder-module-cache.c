/* builder-module.c
 *
 * Copyright (C) 2015 Red Hat, Inc
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

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/statfs.h>

#include <glib/gi18n.h>
#include <gio/gio.h>
#include "libglnx/libglnx.h"

#include "builder-flatpak-utils.h"
#include "builder-utils.h"
#include "builder-module-private.h"
#include "builder-cache.h"
#include "builder-post-process.h"
#include "builder-manifest-private.h"

static void
collect_cleanup_for_path (const char **patterns,
                          const char  *path,
                          const char  *add_prefix,
                          GHashTable  *to_remove_ht)
{
  int i;

  if (patterns == NULL)
    return;

  for (i = 0; patterns[i] != NULL; i++)
    flatpak_collect_matches_for_path_pattern (path, patterns[i], add_prefix, to_remove_ht);
}

gboolean
builder_module_ensure_writable (BuilderModule  *self,
                                BuilderCache   *cache,
                                BuilderContext *context,
                                GError        **error)
{
  g_autoptr(GPtrArray) changes = NULL;
  g_autoptr(GHashTable) matches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GFile *app_dir = builder_context_get_app_dir (context);
  GHashTableIter iter;
  gpointer key, value;
  int i;

  if (cache == NULL)
    return TRUE;

  if (self->ensure_writable == NULL ||
      self->ensure_writable[0] == NULL)
    return TRUE;

  changes = builder_cache_get_files (cache, error);
  if (changes == NULL)
    return FALSE;

  for (i = 0; i < changes->len; i++)
    {
      const char *path = g_ptr_array_index (changes, i);
      const char *unprefixed_path;
      const char *prefix;

      if (g_str_has_prefix (path, "files/"))
        prefix = "files/";
      else if (g_str_has_prefix (path, "usr/"))
        prefix = "usr/";
      else
        continue;

      unprefixed_path = path + strlen (prefix);

      collect_cleanup_for_path ((const char **)self->ensure_writable, unprefixed_path, prefix, matches);
    }

  g_hash_table_iter_init (&iter, matches);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *path = key;
      g_autoptr(GFile) dest = g_file_resolve_relative_path (app_dir, path);

      g_debug ("Breaking hardlink %s", path);
      if (!flatpak_break_hardlink (dest, error))
        return FALSE;
    }


  return TRUE;
}

gboolean
builder_module_post_process (BuilderModule  *self,
                             BuilderCache   *cache,
                             BuilderContext *context,
                             GError        **error)
{
  BuilderPostProcessFlags post_process_flags = 0;
  GFile *app_dir = builder_context_get_app_dir (context);

  if (!self->no_python_timestamp_fix)
    post_process_flags |= BUILDER_POST_PROCESS_FLAGS_PYTHON_TIMESTAMPS;

  if (builder_options_get_strip (self->build_options, self->manifest, context))
    post_process_flags |= BUILDER_POST_PROCESS_FLAGS_STRIP;
  else if (!builder_options_get_no_debuginfo (self->build_options, self->manifest, context) &&
           /* No support for debuginfo for extensions atm */
           !builder_manifest_get_build_extension (self->manifest))
    {
      post_process_flags |= BUILDER_POST_PROCESS_FLAGS_DEBUGINFO;
      if (!builder_options_get_no_debuginfo_compression (self->build_options, self->manifest, context))
        post_process_flags |= BUILDER_POST_PROCESS_FLAGS_DEBUGINFO_COMPRESSION;
    }

  if (!builder_post_process (post_process_flags, app_dir,
                             cache, self->manifest, context, error))
    {
      g_prefix_error (error, "module %s: ", self->name);
      return FALSE;
    }

  return TRUE;
}
