/* builder-manifest.c
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

#include "builder-manifest-private.h"
#include "builder-utils.h"
#include "builder-flatpak-utils.h"
#include "builder-post-process.h"
#include "builder-extension.h"
#include "builder-checksum.h"
#include "builder-cache.h"

#define LOCALES_SEPARATE_DIR "share/runtime/locale"

gboolean
builder_manifest_init_app_dir (BuilderManifest *self,
                               BuilderCache    *cache,
                               BuilderContext  *context,
                               GError         **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);

  g_autoptr(GSubprocess) subp = NULL;
  g_autoptr(GPtrArray) args = NULL;
  g_autofree char *commandline = NULL;
  GList *l;
  int i;

  g_print ("Initializing build dir\n");

  if (self->id == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "id not specified");
      return FALSE;
    }

  if (self->runtime == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "runtime not specified");
      return FALSE;
    }

  if (self->sdk == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "sdk not specified");
      return FALSE;
    }

  args = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build-init"));
  if (self->writable_sdk || self->build_runtime)
    {
      if (self->build_runtime)
        g_ptr_array_add (args, g_strdup ("--type=runtime"));
      else
        g_ptr_array_add (args, g_strdup ("--writable-sdk"));
    }

  for (l = self->add_build_extensions; l != NULL; l = l->next)
    builder_extension_add_finish_args (l->data, args);

  for (i = 0; self->sdk_extensions != NULL && self->sdk_extensions[i] != NULL; i++)
    {
      const char *ext = self->sdk_extensions[i];
      g_ptr_array_add (args, g_strdup_printf ("--sdk-extension=%s", ext));
    }

  if (self->build_extension)
    {
      g_ptr_array_add (args, g_strdup ("--type=extension"));
    }
  if (self->tags)
    {
      for (i = 0; self->tags[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup_printf ("--tag=%s", self->tags[i]));
    }
  if (self->var)
    g_ptr_array_add (args, g_strdup_printf ("--var=%s", self->var));

  if (self->base)
    {
      g_ptr_array_add (args, g_strdup_printf ("--base=%s", self->base));
      g_ptr_array_add (args, g_strdup_printf ("--base-version=%s", builder_manifest_get_base_version (self)));

      for (i = 0; self->base_extensions != NULL && self->base_extensions[i] != NULL; i++)
        {
          const char *ext = self->base_extensions[i];
          g_ptr_array_add (args, g_strdup_printf ("--base-extension=%s", ext));
        }
    }

  if (self->extension_tag != NULL)
    g_ptr_array_add (args, g_strdup_printf ("--extension-tag=%s", self->extension_tag));

  g_ptr_array_add (args, g_strdup_printf ("--arch=%s", builder_context_get_arch (context)));
  g_ptr_array_add (args, g_file_get_path (app_dir));
  g_ptr_array_add (args, g_strdup (self->id));
  g_ptr_array_add (args, g_strdup (self->sdk));
  g_ptr_array_add (args, g_strdup (self->runtime));
  g_ptr_array_add (args, g_strdup (builder_manifest_get_runtime_version (self)));
  g_ptr_array_add (args, NULL);

  commandline = flatpak_quote_argv ((const char **) args->pdata);
  g_debug ("Running '%s'", commandline);

  subp =
    g_subprocess_newv ((const gchar * const *) args->pdata,
                       G_SUBPROCESS_FLAGS_NONE,
                       error);

  if (subp == NULL ||
      !g_subprocess_wait_check (subp, NULL, error))
    return FALSE;

  if (self->build_runtime && self->separate_locales)
    {
      g_autoptr(GFile) root_dir = NULL;

      root_dir = g_file_get_child (app_dir, "usr");

      if (!builder_migrate_locale_dirs (root_dir, error))
        return FALSE;
    }

  /* Fix up any python timestamps from base */
  if (!builder_post_process (BUILDER_POST_PROCESS_FLAGS_PYTHON_TIMESTAMPS, app_dir,
                             cache, self, context, error))
    return FALSE;

  return TRUE;
}

static gboolean
should_delete_build_dir (BuilderContext  *context,
                         gboolean build_succeeded)
{
  if (builder_context_get_keep_build_dirs (context))
    return FALSE;

  if (builder_context_get_delete_build_dirs (context))
    return TRUE;

  /* Unless otherwise specified, keep failed builds */
  return build_succeeded;
}

static gboolean
do_build_module_steps (BuilderManifest *self,
                       BuilderModule   *module,
                       BuilderCache    *cache,
                       BuilderContext  *context,
                       GFile           *build_dir,
                       gboolean         run_shell,
                       GError         **error)
{
  if (!builder_module_extract_sources (module, build_dir, context, error))
    return FALSE;

  if (!builder_module_configure (module, context, build_dir, error))
    return FALSE;

  if (run_shell)
    {
      if (!builder_module_run_shell (module, context, build_dir, error))
        return FALSE;
      return TRUE;
    }

  if (!builder_module_build (module, context, build_dir, error))
    return FALSE;

  if (builder_manifest_get_separate_locales (self) &&
      !builder_module_separate_locales (module, context, error))
    return FALSE;

  if (builder_context_get_run_tests (context) &&
      !builder_module_run_tests (module, context, build_dir, error))
    return FALSE;

  if (!builder_module_post_process (module, cache, context, error))
    return FALSE;

  return TRUE;
}


static gboolean
do_build_module (BuilderManifest *self,
                 BuilderModule   *module,
                 BuilderCache    *cache,
                 BuilderContext  *context,
                 gboolean         run_shell,
                 GError         **error)
{
  g_autoptr(GFile) build_dir = NULL;
  const char *name = builder_module_get_name (module);
  gboolean res = FALSE;

  build_dir = builder_context_allocate_build_subdir (context, name, error);
  if (build_dir == NULL)
    return FALSE;

  if (!builder_context_enable_rofiles (context, error))
    error = NULL; /* Don't report errors from cleanups */
  else
    {
      g_print ("========================================================================\n");
      g_print ("Building module %s in %s\n", name, flatpak_file_get_path_cached (build_dir));
      g_print ("========================================================================\n");

      builder_set_term_title (_("Building %s"), name);

      if (!do_build_module_steps (self, module, cache, context, build_dir,
                                  run_shell, error))
        error = NULL; /* Don't report errors from cleanups */
      else
        res = TRUE; /* Build succeeded */

      if (!builder_context_disable_rofiles (context, error))
        {
          error = NULL;
          res = FALSE;
        }
    }

  /* Keep build dir if requested or if the build failed and we didn't override deletions */
  if (should_delete_build_dir (context, res) &&
      !builder_context_delete_build_dir (context, build_dir, name, error))
    {
      error = NULL; /* Don't report errors from cleanups */
      res = FALSE;
    }

  return res;
}

gboolean
builder_manifest_build (BuilderManifest *self,
                        BuilderCache    *cache,
                        BuilderContext  *context,
                        GError         **error)
{
  g_autoptr(GList) enabled_modules = NULL;
  const char *stop_at = builder_context_get_stop_at (context);
  GList *l;

  g_print ("Starting build of %s\n", self->id ? self->id : "app");
  enabled_modules = builder_manifest_get_enabled_modules (self, context);
  for (l = enabled_modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;
      g_autoptr(GPtrArray) changes = NULL;
      const char *name = builder_module_get_name (m);

      g_autofree char *stage = g_strdup_printf ("build-%s", name);

      if (stop_at != NULL && strcmp (name, stop_at) == 0)
        {
          g_print ("Stopping at module %s\n", stop_at);
          return TRUE;
        }

      if (!builder_module_should_build (m))
        {
          g_print ("Skipping module %s (no sources)\n", name);
          continue;
        }

      builder_module_checksum (m, builder_cache_get_checksum (cache) , context);

      if (!builder_cache_lookup (cache, stage))
        {
          g_autofree char *body = g_strdup_printf ("Built %s\n", builder_module_get_name (m));
          if (!builder_module_ensure_writable (m, cache, context, error))
            return FALSE;
          if (!do_build_module (self, m, cache, context, FALSE, error))
            return FALSE;
          if (!builder_cache_commit (cache, body, error))
            return FALSE;
        }
      else
        {
          g_print ("Cache hit for %s, skipping build\n", name);
        }

      changes = builder_cache_get_changes (cache, error);
      if (changes == NULL)
        return FALSE;

      builder_module_set_changes (m, changes);

      if (!builder_module_update (m, context, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
builder_manifest_build_shell (BuilderManifest *self,
                              BuilderContext  *context,
                              const char      *modulename,
                              GError         **error)
{
  BuilderModule *found = NULL;

  found = builder_manifest_get_module (self, modulename);
  if (found == NULL)
    return flatpak_fail (error, "Can't find module %s", modulename);

  if (!do_build_module (self, found, NULL, context, TRUE, error))
    return FALSE;

  return TRUE;
}

static gboolean
command (GFile      *app_dir,
         char      **env_vars,
         char      **extra_args,
         const char *commandline,
         GError    **error)
{
  g_autoptr(GPtrArray) args = NULL;
  int i;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build"));

  g_ptr_array_add (args, g_strdup ("--die-with-parent"));
  g_ptr_array_add (args, g_strdup ("--nofilesystem=host"));
  if (extra_args)
    {
      for (i = 0; extra_args[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup (extra_args[i]));
    }

  if (env_vars)
    {
      for (i = 0; env_vars[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup_printf ("--env=%s", env_vars[i]));
    }

  g_ptr_array_add (args, g_file_get_path (app_dir));

  g_ptr_array_add (args, g_strdup ("/bin/sh"));
  g_ptr_array_add (args, g_strdup ("-c"));
  g_ptr_array_add (args, g_strdup (commandline));
  g_ptr_array_add (args, NULL);

  return builder_maybe_host_spawnv (NULL, NULL, 0, error, (const char * const *)args->pdata);
}

typedef gboolean (*ForeachFileFunc) (BuilderManifest *self,
                                     int              source_parent_fd,
                                     const char      *source_name,
                                     const char      *full_dir,
                                     const char      *rel_dir,
                                     struct stat     *stbuf,
                                     gboolean        *found,
                                     int              depth,
                                     GError         **error);

static gboolean
foreach_file_helper (BuilderManifest *self,
                     ForeachFileFunc  func,
                     int              source_parent_fd,
                     const char      *source_name,
                     const char      *full_dir,
                     const char      *rel_dir,
                     gboolean        *found,
                     int              depth,
                     GError         **error)
{
  g_auto(GLnxDirFdIterator) source_iter = {0};
  struct dirent *dent;
  g_autoptr(GError) my_error = NULL;

  if (!glnx_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, &my_error))
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return TRUE;

      g_propagate_error (error, g_steal_pointer (&my_error));
      return FALSE;
    }

  while (TRUE)
    {
      struct stat stbuf;

      if (!glnx_dirfd_iterator_next_dent (&source_iter, &dent, NULL, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          if (errno == ENOENT)
            {
              continue;
            }
          else
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
        }

      if (S_ISDIR (stbuf.st_mode))
        {
          g_autofree char *child_dir = g_build_filename (full_dir, dent->d_name, NULL);
          g_autofree char *child_rel_dir = g_build_filename (rel_dir, dent->d_name, NULL);
          if (!foreach_file_helper (self, func, source_iter.fd, dent->d_name, child_dir, child_rel_dir, found, depth + 1, error))
            return FALSE;
        }

      if (!func (self, source_iter.fd, dent->d_name, full_dir, rel_dir, &stbuf, found, depth, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
foreach_file (BuilderManifest *self,
              ForeachFileFunc  func,
              gboolean        *found,
              GFile           *root,
              GError         **error)
{
  return foreach_file_helper (self, func, AT_FDCWD,
                              flatpak_file_get_path_cached (root),
                              flatpak_file_get_path_cached (root),
                              "",
                              found, 0,
                              error);
}

static gboolean
rename_icon_cb (BuilderManifest *self,
                int              source_parent_fd,
                const char      *source_name,
                const char      *full_dir,
                const char      *rel_dir,
                struct stat     *stbuf,
                gboolean        *found,
                int              depth,
                GError         **error)
{
  if (g_str_has_prefix (source_name, self->rename_icon))
    {
      if (S_ISREG (stbuf->st_mode) &&
          depth == 3 &&
          (g_str_has_prefix (source_name + strlen (self->rename_icon), ".") ||
           g_str_has_prefix (source_name + strlen (self->rename_icon), "-symbolic.")))
        {
          const char *extension = source_name + strlen (self->rename_icon);
          g_autofree char *new_name = g_strconcat (self->id, extension, NULL);
          int res;

          *found = TRUE;

          g_print ("%s icon %s/%s to %s/%s\n", self->copy_icon ? "Copying" : "Renaming", rel_dir, source_name, rel_dir, new_name);

          if (self->copy_icon)
            res = linkat (source_parent_fd, source_name, source_parent_fd, new_name, AT_SYMLINK_FOLLOW);
          else
            res = renameat (source_parent_fd, source_name, source_parent_fd, new_name);

          if (res != 0)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Can't rename icon %s/%s", rel_dir, source_name);
              return FALSE;
            }
        }
      else
        {
          if (!S_ISREG (stbuf->st_mode))
            g_debug ("%s/%s matches 'rename-icon', but not a regular file", full_dir, source_name);
          else if (depth != 3)
            g_debug ("%s/%s matches 'rename-icon', but not at depth 3", full_dir, source_name);
          else
            g_debug ("%s/%s matches 'rename-icon', but name does not continue with '.' or '-symbolic.'", full_dir, source_name);
        }
    }

  return TRUE;
}

static int
cmpstringp (const void *p1, const void *p2)
{
  return strcmp (*(char * const *) p1, *(char * const *) p2);
}

static gboolean
appstream_compose (GFile   *app_dir,
                   GError **error,
                   ...)
{
  g_autoptr(GPtrArray) args = NULL;
  const gchar *arg;
  va_list ap;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build"));
  g_ptr_array_add (args, g_strdup ("--die-with-parent"));
  g_ptr_array_add (args, g_strdup ("--nofilesystem=host"));
  g_ptr_array_add (args, g_file_get_path (app_dir));
  g_ptr_array_add (args, g_strdup ("appstream-compose"));

  va_start (ap, error);
  while ((arg = va_arg (ap, const gchar *)))
    g_ptr_array_add (args, g_strdup (arg));
  g_ptr_array_add (args, NULL);
  va_end (ap);

  if (!builder_maybe_host_spawnv (NULL, NULL, 0, error, (const char * const *)args->pdata))
    {
      g_prefix_error (error, "ERROR: appstream-compose failed: ");
      return FALSE;
    }

  return TRUE;
}

static char **
strcatv (char **strv1,
         char **strv2)
{
    if (strv1 == NULL && strv2 == NULL)
        return NULL;
    if (strv1 == NULL)
        return g_strdupv (strv2);
    if (strv2 == NULL)
        return g_strdupv (strv1);

    unsigned len1 = g_strv_length (strv1);
    unsigned len2 = g_strv_length (strv2);
    char **retval = g_new (char *, len1 + len2 + 1);
    unsigned ix;

    for (ix = 0; ix < len1; ix++)
        retval[ix] = g_strdup (strv1[ix]);
    for (ix = 0; ix < len2; ix++)
        retval[len1 + ix] = g_strdup (strv2[ix]);
    retval[len1 + len2] = NULL;

    return retval;
}

static gboolean
rewrite_appdata (GFile *file,
                 const char *license,
                 GError **error)
{
  g_autofree gchar *data = NULL;
  gsize data_len;
  g_autoptr(xmlDoc) doc = NULL;
  xml_autofree xmlChar *xmlbuff = NULL;
  int buffersize;
  xmlNode *root_element, *component_node;

  if (!g_file_load_contents (file, NULL, &data, &data_len, NULL, error))
    return FALSE;

  doc = xmlReadMemory (data, data_len, NULL, NULL,  0);
  if (doc == NULL)
    return flatpak_fail (error, _("Error parsing appstream"));

  root_element = xmlDocGetRootElement (doc);

  for (component_node = root_element; component_node; component_node = component_node->next)
    {
      xmlNode *sub_node = NULL;
      xmlNode *license_node = NULL;

      if (component_node->type != XML_ELEMENT_NODE ||
          strcmp ((char *)component_node->name, "component") != 0)
        continue;

      for (sub_node = component_node->children; sub_node; sub_node = sub_node->next)
        {
          if (sub_node->type != XML_ELEMENT_NODE ||
              strcmp ((char *)sub_node->name, "project_license") != 0)
            continue;

          license_node = sub_node;
          break;
        }

      if (license_node)
        xmlNodeSetContent(license_node, (xmlChar *)license);
      else
        xmlNewChild(component_node, NULL, (xmlChar *)"project_license", (xmlChar *)license);
    }

  xmlDocDumpFormatMemory (doc, &xmlbuff, &buffersize, 1);

  if (!g_file_set_contents (flatpak_file_get_path_cached (file),
                            (gchar *)xmlbuff, buffersize,
                            error))
    return FALSE;

  return TRUE;
}

static GFile *
builder_manifest_find_appdata_file (BuilderManifest *self,
				    GFile *app_root)
{
  /* We order these so that share/appdata/XXX.appdata.xml if found
     first, as this is the target name, and apps may have both, which will
     cause issues with the rename. */
  const char *extensions[] = {
    ".appdata.xml",
    ".metainfo.xml",
  };
  const char *dirs[] = {
    "share/appdata",
    "share/metainfo",
  };
  g_autoptr(GFile) source = NULL;

  int i, j;
  for (j = 0; j < G_N_ELEMENTS (dirs); j++)
    {
      g_autoptr(GFile) appdata_dir = g_file_resolve_relative_path (app_root, dirs[j]);
      for (i = 0; i < G_N_ELEMENTS (extensions); i++)
	{
	  g_autofree char *basename = NULL;

	  if (self->rename_appdata_file != NULL)
	    basename = g_strdup (self->rename_appdata_file);
	  else
	    basename = g_strconcat (self->id, extensions[i], NULL);

	  source = g_file_get_child (appdata_dir, basename);
	  if (g_file_query_exists (source, NULL))
	    return g_steal_pointer (&source);
	}
    }
  return NULL;
}

gboolean
builder_manifest_cleanup (BuilderManifest *self,
                          BuilderCache    *cache,
                          BuilderContext  *context,
                          GError         **error)
{
  g_autoptr(GFile) app_root = NULL;
  GList *l;
  g_auto(GStrv) env = NULL;
  g_autoptr(GFile) appdata_file = NULL;
  g_autoptr(GFile) appdata_source = NULL;
  int i;

  builder_manifest_checksum_for_cleanup (self, builder_cache_get_checksum (cache), context);
  if (!builder_cache_lookup (cache, "cleanup"))
    {
      g_autoptr(GList) enabled_modules = NULL;
      g_autoptr(GHashTable) to_remove_ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_autofree char **keys = NULL;
      GFile *app_dir = NULL;
      guint n_keys;

      g_print ("Cleaning up\n");

      if (!builder_context_enable_rofiles (context, error))
        return FALSE;

      /* Call after enabling rofiles */
      app_dir = builder_context_get_app_dir (context);

      if (self->cleanup_commands)
        {
          g_auto(GStrv) build_args = builder_options_get_build_args (self->build_options, self, context, error);
          if (!build_args)
            return FALSE;
          env = builder_options_get_env (self->build_options, self, context);
          for (i = 0; self->cleanup_commands[i] != NULL; i++)
            {
              if (!command (app_dir, env, build_args, self->cleanup_commands[i], error))
                return FALSE;
            }
        }

      enabled_modules = builder_manifest_get_enabled_modules (self, context);
      for (l = enabled_modules; l != NULL; l = l->next)
        {
          BuilderModule *m = l->data;

          builder_module_cleanup_collect (m, FALSE, context, to_remove_ht);
        }

      keys = (char **) g_hash_table_get_keys_as_array (to_remove_ht, &n_keys);

      qsort (keys, n_keys, sizeof (char *), cmpstringp);
      /* Iterate in reverse to remove leafs first */
      for (i = n_keys - 1; i >= 0; i--)
        {
          g_autoptr(GError) my_error = NULL;
          g_autoptr(GFile) f = g_file_resolve_relative_path (app_dir, keys[i]);
          g_print ("Removing %s\n", keys[i]);
          if (!g_file_delete (f, NULL, &my_error))
            {
              if (!g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
                  !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
                {
                  g_propagate_error (error, g_steal_pointer (&my_error));
                  return FALSE;
                }
            }
        }

      app_root = g_file_get_child (app_dir, "files");

      appdata_source = builder_manifest_find_appdata_file (self, app_root);
      if (appdata_source)
	{
	  /* We always use the old name / dir, in case the runtime has older appdata tools */
	  g_autoptr(GFile) appdata_dir = g_file_resolve_relative_path (app_root, "share/appdata");
	  g_autofree char *appdata_basename = g_strdup_printf ("%s.appdata.xml", self->id);

	  appdata_file = g_file_get_child (appdata_dir, appdata_basename);

	  if (!g_file_equal (appdata_source, appdata_file))
	    {
	      g_autofree char *src_basename = g_file_get_basename (appdata_source);
	      g_print ("Renaming %s to share/appdata/%s\n", src_basename, appdata_basename);

              if (!flatpak_mkdir_p (appdata_dir, NULL, error))
                return FALSE;
	      if (!g_file_move (appdata_source, appdata_file, 0, NULL, NULL, NULL, error))
		return FALSE;
	    }

	  if (self->appdata_license != NULL && self->appdata_license[0] != 0)
	    {
	      if (!rewrite_appdata (appdata_file, self->appdata_license, error))
		return FALSE;
	    }
	}

      if (self->rename_desktop_file != NULL)
        {
          g_autoptr(GFile) applications_dir = g_file_resolve_relative_path (app_root, "share/applications");
          g_autoptr(GFile) src = g_file_get_child (applications_dir, self->rename_desktop_file);
          g_autofree char *desktop_basename = g_strdup_printf ("%s.desktop", self->id);
          g_autoptr(GFile) dest = g_file_get_child (applications_dir, desktop_basename);

          g_print ("Renaming %s to %s\n", self->rename_desktop_file, desktop_basename);
          if (!g_file_move (src, dest, 0, NULL, NULL, NULL, error))
            return FALSE;

          if (appdata_file != NULL)
            {
              FlatpakXml *n_id;
              FlatpakXml *n_root;
              FlatpakXml *n_text;
              g_autoptr(FlatpakXml) xml_root = NULL;
              g_autoptr(GInputStream) in = NULL;
              g_autoptr(GString) new_contents = NULL;

              in = (GInputStream *) g_file_read (appdata_file, NULL, error);
              if (!in)
                return FALSE;
              xml_root = flatpak_xml_parse (in, FALSE, NULL, error);
              if (!xml_root)
                return FALSE;

              /* replace component/id */
              n_root = flatpak_xml_find (xml_root, "component", NULL);
              if (!n_root)
                n_root = flatpak_xml_find (xml_root, "application", NULL);
              if (!n_root)
                {
                  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "no <component> node");
                  return FALSE;
                }
              n_id = flatpak_xml_find (n_root, "id", NULL);
              if (n_id)
                {
                  n_text = n_id->first_child;
                  if (n_text && g_strcmp0 (n_text->text, self->rename_desktop_file) == 0)
                    {
                      g_free (n_text->text);
                      n_text->text = g_strdup (self->id);
                    }
                }

              /* replace any optional launchable */
              n_id = flatpak_xml_find (n_root, "launchable", NULL);
              if (n_id)
                {
                  n_text = n_id->first_child;
                  if (n_text && g_strcmp0 (n_text->text, self->rename_desktop_file) == 0)
                    {
                      g_free (n_text->text);
                      n_text->text = g_strdup (desktop_basename);
                    }
                }

              new_contents = g_string_new ("");
              flatpak_xml_to_string (xml_root, new_contents);
              if (!g_file_set_contents (flatpak_file_get_path_cached (appdata_file),
                                        new_contents->str,
                                        new_contents->len,
                                        error))
                return FALSE;
            }
        }

      if (self->rename_icon)
        {
          gboolean found_icon = FALSE;
          g_autoptr(GFile) icons_dir = g_file_resolve_relative_path (app_root, "share/icons");

          if (!foreach_file (self, rename_icon_cb, &found_icon, icons_dir, error))
            return FALSE;

          if (!found_icon)
            {
              g_autofree char *icon_path = g_file_get_path (icons_dir);
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "icon %s not found below %s",
                           self->rename_icon, icon_path);
              return FALSE;
            }
        }

      if (self->rename_icon ||
          self->desktop_file_name_prefix ||
          self->desktop_file_name_suffix ||
          self->rename_desktop_file)
        {
          g_autoptr(GFile) applications_dir = g_file_resolve_relative_path (app_root, "share/applications");
          g_autofree char *desktop_basename = g_strdup_printf ("%s.desktop", self->id);
          g_autoptr(GFile) desktop = g_file_get_child (applications_dir, desktop_basename);
          g_autoptr(GKeyFile) keyfile = g_key_file_new ();
          g_autofree char *desktop_contents = NULL;
          gsize desktop_size;
          g_auto(GStrv) desktop_keys = NULL;

          g_print ("Rewriting contents of %s\n", desktop_basename);
          if (!g_file_load_contents (desktop, NULL,
                                     &desktop_contents, &desktop_size, NULL, error))
            {
              g_autofree char *desktop_path = g_file_get_path (desktop);
              g_prefix_error (error, "Can't load desktop file %s: ", desktop_path);
              return FALSE;
            }

          if (!g_key_file_load_from_data (keyfile,
                                          desktop_contents, desktop_size,
                                          G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                          error))
            return FALSE;

          if (self->rename_desktop_file)
            {
              g_auto(GStrv) old_renames = g_key_file_get_string_list (keyfile,
                                                                      G_KEY_FILE_DESKTOP_GROUP,
                                                                      "X-Flatpak-RenamedFrom",
                                                                      NULL, NULL);
              const char **new_renames = NULL;
              int old_rename_len = 0;
              int new_rename_len = 0;

              if (old_renames)
                old_rename_len = g_strv_length (old_renames);

              new_renames = g_new (const char *, old_rename_len + 2);
              for (i = 0; i < old_rename_len; i++)
                new_renames[new_rename_len++] = old_renames[i];
              new_renames[new_rename_len++] = self->rename_desktop_file;
              new_renames[new_rename_len] = NULL;

              g_key_file_set_string_list (keyfile,
                                          G_KEY_FILE_DESKTOP_GROUP,
                                          "X-Flatpak-RenamedFrom",
                                          new_renames, new_rename_len);
            }

          desktop_keys = g_key_file_get_keys (keyfile,
                                              G_KEY_FILE_DESKTOP_GROUP,
                                              NULL, NULL);
          if (self->rename_icon)
            {
              g_autofree char *original_icon_name = g_key_file_get_string (keyfile,
                                                                           G_KEY_FILE_DESKTOP_GROUP,
                                                                           G_KEY_FILE_DESKTOP_KEY_ICON,
                                                                           NULL);

              g_key_file_set_string (keyfile,
                                     G_KEY_FILE_DESKTOP_GROUP,
                                     G_KEY_FILE_DESKTOP_KEY_ICON,
                                     self->id);

              /* Also rename localized version of the Icon= field */
              for (i = 0; desktop_keys[i]; i++)
                {
                  /* Only rename untranslated icon names */
                  if (g_str_has_prefix (desktop_keys[i], "Icon["))
                    {
                      g_autofree char *icon_name = g_key_file_get_string (keyfile,
                                                                          G_KEY_FILE_DESKTOP_GROUP,
                                                                          desktop_keys[i], NULL);

                      if (strcmp (icon_name, original_icon_name) == 0)
                        g_key_file_set_string (keyfile,
                                               G_KEY_FILE_DESKTOP_GROUP,
                                               desktop_keys[i],
                                               self->id);
                    }
                }
            }

          if (self->desktop_file_name_suffix ||
              self->desktop_file_name_prefix)
            {
              for (i = 0; desktop_keys[i]; i++)
                {
                  if (strcmp (desktop_keys[i], "Name") == 0 ||
                      g_str_has_prefix (desktop_keys[i], "Name["))
                    {
                      g_autofree char *name = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, desktop_keys[i], NULL);
                      if (name)
                        {
                          g_autofree char *new_name =
                            g_strdup_printf ("%s%s%s",
                                             self->desktop_file_name_prefix ? self->desktop_file_name_prefix : "",
                                             name,
                                             self->desktop_file_name_suffix ? self->desktop_file_name_suffix : "");
                          g_key_file_set_string (keyfile,
                                                 G_KEY_FILE_DESKTOP_GROUP,
                                                 desktop_keys[i],
                                                 new_name);
                        }
                    }
                }
            }

          g_free (desktop_contents);
          desktop_contents = g_key_file_to_data (keyfile, &desktop_size, error);
          if (desktop_contents == NULL)
            return FALSE;

          if (!g_file_set_contents (flatpak_file_get_path_cached (desktop),
                                    desktop_contents, desktop_size, error))
            return FALSE;
        }

      if (self->appstream_compose && appdata_file != NULL)
        {
          g_autofree char *basename_arg = g_strdup_printf ("--basename=%s", self->id);
          g_print ("Running appstream-compose\n");
          if (!appstream_compose (app_dir, error,
                                  self->build_runtime ?  "--prefix=/usr" : "--prefix=/app",
                                  "--origin=flatpak",
                                  basename_arg,
                                  self->id,
                                  NULL))
            return FALSE;
        }

      if (!builder_context_disable_rofiles (context, error))
        return FALSE;

      if (!builder_cache_commit (cache, "Cleanup", error))
        return FALSE;
    }
  else
    {
      g_print ("Cache hit for cleanup, skipping\n");
    }

  return TRUE;
}

static char *
maybe_format_extension_tag (const char *extension_tag)
{
  if (extension_tag != NULL)
    return g_strdup_printf ("tag=%s\n", extension_tag);

  return g_strdup ("");
}


gboolean
builder_manifest_finish (BuilderManifest *self,
                         BuilderCache    *cache,
                         BuilderContext  *context,
                         GError         **error)
{
  g_autoptr(GFile) manifest_file = NULL;
  g_autoptr(GFile) debuginfo_dir = NULL;
  g_autoptr(GFile) sources_dir = NULL;
  g_autoptr(GFile) locale_parent_dir = NULL;
  g_autofree char *json = NULL;
  g_autofree char *commandline = NULL;
  g_autoptr(GPtrArray) args = NULL;
  g_autoptr(GPtrArray) inherit_extensions = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  g_autoptr(GList) enabled_modules = NULL;
  int i;
  GList *l;

  builder_manifest_checksum_for_finish (self, builder_cache_get_checksum (cache), context);
  if (!builder_cache_lookup (cache, "finish"))
    {
      GFile *app_dir = NULL;
      g_autoptr(GPtrArray) sub_ids = g_ptr_array_new_with_free_func (g_free);
      g_autofree char *ref = NULL;
      g_print ("Finishing app\n");

      builder_set_term_title (_("Finishing %s"), self->id);

      if (!builder_context_enable_rofiles (context, error))
        return FALSE;

      /* Call after enabling rofiles */
      app_dir = builder_context_get_app_dir (context);

      ref = flatpak_compose_ref (!self->build_runtime && !self->build_extension,
                                 builder_manifest_get_id (self),
                                 builder_manifest_get_branch (self),
                                 builder_context_get_arch (context));

      if (self->metadata)
        {
          GFile *base_dir = builder_context_get_base_dir (context);
          g_autoptr(GFile) dest_metadata = g_file_get_child (app_dir, "metadata");
          g_autoptr(GFile) src_metadata = g_file_resolve_relative_path (base_dir, self->metadata);
          g_autofree char *contents = NULL;
          gsize length;

          if (!g_file_get_contents (flatpak_file_get_path_cached (src_metadata),
                                    &contents, &length, error))
            return FALSE;

          if (!g_file_set_contents (flatpak_file_get_path_cached (dest_metadata),
                                    contents, length, error))
            return FALSE;
        }

      if ((self->inherit_extensions && self->inherit_extensions[0] != NULL) ||
          (self->inherit_sdk_extensions && self->inherit_sdk_extensions[0] != NULL))
        {
          g_autoptr(GFile) metadata = g_file_get_child (app_dir, "metadata");
          g_autoptr(GKeyFile) keyfile = g_key_file_new ();
          g_autoptr(GKeyFile) base_keyfile = g_key_file_new ();
          g_autofree char *arch_option = NULL;
          const char *parent_id = NULL;
          const char *parent_version = NULL;
          g_autofree char *base_metadata = NULL;

          arch_option = g_strdup_printf ("--arch=%s", builder_context_get_arch (context));

          if (self->base != NULL && *self->base != 0)
            {
              parent_id = self->base;
              parent_version = builder_manifest_get_base_version (self);
            }
          else
            {
              parent_id = self->sdk;
              parent_version = builder_manifest_get_runtime_version (self);
            }

          base_metadata = flatpak (NULL, "info", arch_option, "--show-metadata", parent_id, parent_version, NULL);
          if (base_metadata == NULL)
            return flatpak_fail (error, "Inherit extensions specified, but could not get metadata for parent %s version %s", parent_id, parent_version);

          if (!g_key_file_load_from_data (base_keyfile,
                                          base_metadata, -1,
                                          G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                          error))
            {
              g_prefix_error (error, "Can't load metadata file: ");
              return FALSE;
            }

          if (!g_key_file_load_from_file (keyfile,
                                          flatpak_file_get_path_cached (metadata),
                                          G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS,
                                          error))
            {
              g_prefix_error (error, "Can't load metadata file: ");
              return FALSE;
            }

          inherit_extensions = g_ptr_array_new ();

          for (i = 0; self->inherit_extensions != NULL && self->inherit_extensions[i] != NULL; i++)
            g_ptr_array_add (inherit_extensions, self->inherit_extensions[i]);

          for (i = 0; self->inherit_sdk_extensions != NULL && self->inherit_sdk_extensions[i] != NULL; i++)
            g_ptr_array_add (inherit_extensions, self->inherit_sdk_extensions[i]);

          for (i = 0; i < inherit_extensions->len; i++)
            {
              const char *extension = inherit_extensions->pdata[i];
              g_autofree char *group = g_strconcat (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION,
                                                    extension,
                                                    NULL);
              g_auto(GStrv) keys = NULL;
              int j;

              if (!g_key_file_has_group (base_keyfile, group))
                return flatpak_fail (error, "Can't find inherited extension point %s", extension);

              keys = g_key_file_get_keys (base_keyfile, group, NULL, error);
              if (keys == NULL)
                return FALSE;

              for (j = 0; keys[j] != NULL; j++)
                {
                  g_autofree char *value = g_key_file_get_value (base_keyfile, group, keys[j], error);
                  if (value == NULL)
                    return FALSE;
                  g_key_file_set_value (keyfile, group, keys[j], value);
                }

              if (!g_key_file_has_key (keyfile, group,
                                       FLATPAK_METADATA_KEY_VERSION, NULL) &&
                  !g_key_file_has_key (keyfile, group,
                                       FLATPAK_METADATA_KEY_VERSIONS, NULL))
                g_key_file_set_value (keyfile, group,
                                      FLATPAK_METADATA_KEY_VERSION,
                                      parent_version);
            }

          if (!g_key_file_save_to_file (keyfile,
                                        flatpak_file_get_path_cached (metadata),
                                        error))
            {
              g_prefix_error (error, "Can't save metadata.platform: ");
              return FALSE;
            }
        }

      if (self->command)
        {
          g_autoptr(GFile) files_dir = g_file_resolve_relative_path (app_dir, "files");
          g_autoptr(GFile) command_file = NULL;

          if (!g_path_is_absolute (self->command))
            {
              g_autoptr(GFile) bin_dir = g_file_resolve_relative_path (files_dir, "bin");
              command_file = g_file_get_child (bin_dir, self->command);
            }
          else if (g_str_has_prefix (self->command, "/app/"))
            command_file = g_file_resolve_relative_path (files_dir, self->command + strlen ("/app/"));

          if (command_file != NULL &&
              !g_file_query_exists (command_file, NULL))
            {
              const char *help = "";

              if (strchr (self->command, ' '))
                help = ". Use a shell wrapper for passing arguments";

              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Command '%s' not found%s", self->command, help);

              return FALSE;
            }
        }

      args = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (args, g_strdup ("flatpak"));
      g_ptr_array_add (args, g_strdup ("build-finish"));
      if (self->command)
        g_ptr_array_add (args, g_strdup_printf ("--command=%s", self->command));

      if (self->finish_args)
        {
          for (i = 0; self->finish_args[i] != NULL; i++)
            g_ptr_array_add (args, g_strdup (self->finish_args[i]));
        }

      for (l = self->add_build_extensions; l != NULL; l = l->next)
        builder_extension_add_remove_args (l->data, args);

      for (l = self->add_extensions; l != NULL; l = l->next)
        builder_extension_add_finish_args (l->data, args);

      enabled_modules = builder_manifest_get_enabled_modules (self, context);
      for (l = enabled_modules; l != NULL; l = l->next)
        {
          BuilderModule *m = l->data;
          builder_module_finish_sources (m, args, context);
        }

      g_ptr_array_add (args, g_file_get_path (app_dir));
      g_ptr_array_add (args, NULL);

      commandline = flatpak_quote_argv ((const char **) args->pdata);
      g_debug ("Running '%s'", commandline);

      subp =
        g_subprocess_newv ((const gchar * const *) args->pdata,
                           G_SUBPROCESS_FLAGS_NONE,
                           error);

      if (subp == NULL ||
          !g_subprocess_wait_check (subp, NULL, error))
        return FALSE;

      json = builder_manifest_serialize (self);

      if (self->build_runtime)
        manifest_file = g_file_resolve_relative_path (app_dir, "usr/manifest.json");
      else
        manifest_file = g_file_resolve_relative_path (app_dir, "files/manifest.json");

      if (g_file_query_exists (manifest_file, NULL))
        {
          /* Move existing base manifest aside */
          g_autoptr(GFile) manifest_dir = g_file_get_parent (manifest_file);
          g_autoptr(GFile) old_manifest = NULL;
          int ver = 0;

          do
            {
              g_autofree char *basename = g_strdup_printf ("manifest-base-%d.json", ++ver);
              g_clear_object (&old_manifest);
              old_manifest = g_file_get_child (manifest_dir, basename);
            }
          while (g_file_query_exists (old_manifest, NULL));

          if (!g_file_move (manifest_file, old_manifest, 0,
                            NULL, NULL, NULL, error))
            return FALSE;
        }

      if (!g_file_set_contents (flatpak_file_get_path_cached (manifest_file),
                                json, strlen (json), error))
        return FALSE;

      if (self->build_runtime)
        {
          debuginfo_dir = g_file_resolve_relative_path (app_dir, "usr/lib/debug");
          locale_parent_dir = g_file_resolve_relative_path (app_dir, "usr/" LOCALES_SEPARATE_DIR);
        }
      else
        {
          debuginfo_dir = g_file_resolve_relative_path (app_dir, "files/lib/debug");
          locale_parent_dir = g_file_resolve_relative_path (app_dir, "files/" LOCALES_SEPARATE_DIR);
        }
      sources_dir = g_file_resolve_relative_path (app_dir, "sources");

      if (self->separate_locales && g_file_query_exists (locale_parent_dir, NULL))
        {
          g_autoptr(GFile) metadata_file = NULL;
          g_autofree char *extension_contents = NULL;
          g_autoptr(GFileOutputStream) output = NULL;
          g_autoptr(GFile) metadata_locale_file = NULL;
          g_autofree char *metadata_contents = NULL;
          g_autofree char *locale_id = builder_manifest_get_locale_id (self);

          metadata_file = g_file_get_child (app_dir, "metadata");

          extension_contents = g_strdup_printf ("\n"
                                                "[Extension %s]\n"
                                                "directory=%s\n"
                                                "autodelete=true\n"
                                                "locale-subset=true\n",
                                                locale_id,
                                                LOCALES_SEPARATE_DIR);

          output = g_file_append_to (metadata_file, G_FILE_CREATE_NONE, NULL, error);
          if (output == NULL)
            return FALSE;

          if (!g_output_stream_write_all (G_OUTPUT_STREAM (output),
                                          extension_contents, strlen (extension_contents),
                                          NULL, NULL, error))
            return FALSE;


          metadata_locale_file = g_file_get_child (app_dir, "metadata.locale");
          metadata_contents = g_strdup_printf ("[Runtime]\n"
                                               "name=%s\n"
                                               "\n"
                                               "[ExtensionOf]\n"
                                               "ref=%s\n",
                                               locale_id, ref);
          if (!g_file_set_contents (flatpak_file_get_path_cached (metadata_locale_file),
                                    metadata_contents, strlen (metadata_contents),
                                    error))
            return FALSE;

          g_ptr_array_add (sub_ids, g_strdup (locale_id));
        }

      if (g_file_query_exists (debuginfo_dir, NULL))
        {
          g_autoptr(GFile) metadata_file = NULL;
          g_autoptr(GFile) metadata_debuginfo_file = NULL;
          g_autofree char *metadata_contents = NULL;
          g_autofree char *extension_contents = NULL;
          g_autoptr(GFileOutputStream) output = NULL;
          g_autofree char *debug_id = builder_manifest_get_debug_id (self);

          metadata_file = g_file_get_child (app_dir, "metadata");
          metadata_debuginfo_file = g_file_get_child (app_dir, "metadata.debuginfo");

          extension_contents = g_strdup_printf ("\n"
                                                "[Extension %s]\n"
                                                "directory=lib/debug\n"
                                                "autodelete=true\n"
                                                "no-autodownload=true\n",
                                                debug_id);

          output = g_file_append_to (metadata_file, G_FILE_CREATE_NONE, NULL, error);
          if (output == NULL)
            return FALSE;

          if (!g_output_stream_write_all (G_OUTPUT_STREAM (output), extension_contents, strlen (extension_contents),
                                          NULL, NULL, error))
            return FALSE;

          metadata_contents = g_strdup_printf ("[Runtime]\n"
                                               "name=%s\n"
                                               "\n"
                                               "[ExtensionOf]\n"
                                               "ref=%s\n",
                                               debug_id, ref);
          if (!g_file_set_contents (flatpak_file_get_path_cached (metadata_debuginfo_file),
                                    metadata_contents, strlen (metadata_contents), error))
            return FALSE;

          g_ptr_array_add (sub_ids, g_strdup (debug_id));
        }

      for (l = self->add_extensions; l != NULL; l = l->next)
        {
          BuilderExtension *e = l->data;
          g_autofree char *extension_metadata_name = NULL;
          g_autoptr(GFile) metadata_extension_file = NULL;
          g_autofree char *metadata_contents = NULL;
          g_autofree char *extension_tag_opt = NULL;

          if (!builder_extension_is_bundled (e))
            continue;

          extension_tag_opt = maybe_format_extension_tag (builder_manifest_get_extension_tag (self));
          extension_metadata_name = g_strdup_printf ("metadata.%s", builder_extension_get_name (e));
          metadata_extension_file = g_file_get_child (app_dir, extension_metadata_name);
          metadata_contents = g_strdup_printf ("[Runtime]\n"
                                               "name=%s\n"
                                               "\n"
                                               "[ExtensionOf]\n"
                                               "ref=%s\n"
                                               "%s",
                                               builder_extension_get_name (e),
                                               ref,
                                               extension_tag_opt);
          if (!g_file_set_contents (flatpak_file_get_path_cached (metadata_extension_file),
                                    metadata_contents, strlen (metadata_contents), error))
            return FALSE;

          g_ptr_array_add (sub_ids, g_strdup (builder_extension_get_name (e)));
        }

      if (sub_ids->len > 0)
        {
          g_autoptr(GFile) metadata_file = NULL;
          g_autoptr(GFileOutputStream) output = NULL;
          g_autoptr(GString) extension_contents = g_string_new ("\n"
                                                                "[Build]\n");

          g_string_append (extension_contents, FLATPAK_METADATA_KEY_BUILD_EXTENSIONS"=");
          for (i = 0; i < sub_ids->len; i++)
            {
              g_string_append (extension_contents, (const char *)sub_ids->pdata[i]);
              g_string_append (extension_contents, ";");
            }

          metadata_file = g_file_get_child (app_dir, "metadata");
          output = g_file_append_to (metadata_file, G_FILE_CREATE_NONE, NULL, error);
          if (output == NULL)
            return FALSE;

          if (!g_output_stream_write_all (G_OUTPUT_STREAM (output),
                                          extension_contents->str, extension_contents->len,
                                          NULL, NULL, error))
            return FALSE;
        }

      if (!builder_context_disable_rofiles (context, error))
        return FALSE;

      if (!builder_cache_commit (cache, "Finish", error))
        return FALSE;
    }
  else
    {
      g_print ("Cache hit for finish, skipping\n");
    }

  return TRUE;
}

gboolean
builder_manifest_create_platform (BuilderManifest *self,
                                  BuilderCache    *cache,
                                  BuilderContext  *context,
                                  GError         **error)
{
  g_autofree char *commandline = NULL;

  g_autoptr(GFile) locale_dir = NULL;
  int i;

  if (!self->build_runtime ||
      self->id_platform == NULL)
    return TRUE;

  builder_manifest_checksum_for_platform (self, builder_cache_get_checksum (cache), context);
  if (!builder_cache_lookup (cache, "platform"))
    {
      g_autoptr(GHashTable) to_remove_ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_autoptr(GPtrArray) changes = NULL;
      GList *l;
      g_autoptr(GFile) platform_dir = NULL;
      g_autoptr(GSubprocess) subp = NULL;
      g_autoptr(GPtrArray) args = NULL;
      GFile *app_dir = NULL;
      g_autofree char *ref = NULL;
      g_autoptr(GPtrArray) sub_ids = g_ptr_array_new_with_free_func (g_free);
      g_autoptr(GList) enabled_modules = NULL;

      g_print ("Creating platform based on %s\n", self->runtime);

      builder_set_term_title (_("Creating platform for %s"), self->id);

      if (!builder_context_enable_rofiles (context, error))
        return FALSE;

      /* Call after enabling rofiles */
      app_dir = builder_context_get_app_dir (context);

      ref = flatpak_compose_ref (!self->build_runtime && !self->build_extension,
                                 builder_manifest_get_id_platform (self),
                                 builder_manifest_get_branch (self),
                                 builder_context_get_arch (context));

      platform_dir = g_file_get_child (app_dir, "platform");

      args = g_ptr_array_new_with_free_func (g_free);

      g_ptr_array_add (args, g_strdup ("flatpak"));
      g_ptr_array_add (args, g_strdup ("build-init"));
      g_ptr_array_add (args, g_strdup ("--update"));
      g_ptr_array_add (args, g_strdup ("--writable-sdk"));
      g_ptr_array_add (args, g_strdup ("--sdk-dir=platform"));
      g_ptr_array_add (args, g_strdup_printf ("--arch=%s", builder_context_get_arch (context)));

      for (i = 0; self->platform_extensions != NULL && self->platform_extensions[i] != NULL; i++)
        {
          const char *ext = self->platform_extensions[i];
          g_ptr_array_add (args, g_strdup_printf ("--sdk-extension=%s", ext));
        }

      g_ptr_array_add (args, g_file_get_path (app_dir));
      g_ptr_array_add (args, g_strdup (self->id));
      g_ptr_array_add (args, g_strdup (self->runtime));
      g_ptr_array_add (args, g_strdup (self->runtime));
      g_ptr_array_add (args, g_strdup (builder_manifest_get_runtime_version (self)));

      g_ptr_array_add (args, NULL);

      commandline = flatpak_quote_argv ((const char **) args->pdata);
      g_debug ("Running '%s'", commandline);

      subp =
        g_subprocess_newv ((const gchar * const *) args->pdata,
                           G_SUBPROCESS_FLAGS_NONE,
                           error);

      if (subp == NULL ||
          !g_subprocess_wait_check (subp, NULL, error))
        return FALSE;

      if (self->separate_locales)
        {
          g_autoptr(GFile) root_dir = NULL;

          root_dir = g_file_get_child (app_dir, "platform");

          if (!builder_migrate_locale_dirs (root_dir, error))
            return FALSE;

          locale_dir = g_file_resolve_relative_path (root_dir, LOCALES_SEPARATE_DIR);
        }

      if (self->metadata_platform)
        {
          GFile *base_dir = builder_context_get_base_dir (context);
          g_autoptr(GFile) dest_metadata = g_file_get_child (app_dir, "metadata.platform");
          g_autoptr(GFile) src_metadata = g_file_resolve_relative_path (base_dir, self->metadata_platform);
          g_autofree char *contents = NULL;
          gsize length;

          if (!g_file_get_contents (flatpak_file_get_path_cached (src_metadata),
                                    &contents, &length, error))
            return FALSE;

          if (!g_file_set_contents (flatpak_file_get_path_cached (dest_metadata),
                                    contents, length, error))
            return FALSE;
        }
      else
        {
          g_autoptr(GFile) metadata = g_file_get_child (app_dir, "metadata");
          g_autoptr(GFile) dest_metadata = g_file_get_child (app_dir, "metadata.platform");
          g_autoptr(GKeyFile) keyfile = g_key_file_new ();
          g_auto(GStrv) groups = NULL;
          int j;

          if (!g_key_file_load_from_file (keyfile,
                                          flatpak_file_get_path_cached (metadata),
                                          G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS,
                                          error))
            {
              g_prefix_error (error, "Can't load metadata file: ");
              return FALSE;
            }

          g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_RUNTIME,
                                 FLATPAK_METADATA_KEY_NAME, self->id_platform);

          groups = g_key_file_get_groups (keyfile, NULL);
          for (j = 0; groups[j] != NULL; j++)
            {
              const char *ext;

              if (!g_str_has_prefix (groups[j], FLATPAK_METADATA_GROUP_PREFIX_EXTENSION))
                continue;

              ext = groups[j] + strlen (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION);

              if (g_str_has_prefix (ext, self->id) ||
                  (self->inherit_sdk_extensions &&
                   g_strv_contains ((const char * const *)self->inherit_sdk_extensions, ext)))
                {
                  g_key_file_remove_group (keyfile, groups[j], NULL);
                }
            }

          if (!g_key_file_save_to_file (keyfile,
                                        flatpak_file_get_path_cached (dest_metadata),
                                        error))
            {
              g_prefix_error (error, "Can't save metadata.platform: ");
              return FALSE;
            }
        }

      if (self->prepare_platform_commands)
        {
          g_auto(GStrv) env = builder_options_get_env (self->build_options, self, context);
          g_auto(GStrv) build_args = builder_options_get_build_args (self->build_options, self, context, error);
          if (!build_args)
            return FALSE;
          char *platform_args[] = { "--sdk-dir=platform", "--metadata=metadata.platform", NULL };
          g_auto(GStrv) extra_args = strcatv (build_args, platform_args);

          for (i = 0; self->prepare_platform_commands[i] != NULL; i++)
            {
              if (!command (app_dir, env, extra_args, self->prepare_platform_commands[i], error))
                return FALSE;
            }
        }

      enabled_modules = builder_manifest_get_enabled_modules (self, context);
      for (l = enabled_modules; l != NULL; l = l->next)
        {
          BuilderModule *m = l->data;

          builder_module_cleanup_collect (m, TRUE, context, to_remove_ht);
        }

      /* This returns both additiona and removals */
      changes = builder_cache_get_all_changes (cache, error);
      if (changes == NULL)
        return FALSE;

      for (i = 0; i < changes->len; i++)
        {
          const char *changed = g_ptr_array_index (changes, i);
          g_autoptr(GFile) src = NULL;
          g_autoptr(GFile) dest = NULL;
          g_autoptr(GFileInfo) info = NULL;
          g_autoptr(GError) my_error = NULL;

          if (!g_str_has_prefix (changed, "usr/"))
            continue;

          if (g_str_has_prefix (changed, "usr/lib/debug/") &&
              !g_str_equal (changed, "usr/lib/debug/app"))
            continue;

          src = g_file_resolve_relative_path (app_dir, changed);
          dest = g_file_resolve_relative_path (platform_dir, changed + strlen ("usr/"));

          info = g_file_query_info (src, "standard::type,standard::symlink-target",
                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                    NULL, &my_error);
          if (info == NULL &&
              !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_propagate_error (error, g_steal_pointer (&my_error));
              return FALSE;
            }
          g_clear_error (&my_error);

          if (info == NULL)
            {
              /* File was removed from sdk, remove from platform also if it exists there */

              if (!g_file_delete (dest, NULL, &my_error) &&
                  !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                {
                  g_propagate_error (error, g_steal_pointer (&my_error));
                  return FALSE;
                }

              continue;
            }

          if (g_hash_table_contains (to_remove_ht, changed))
            {
              g_print ("Ignoring %s\n", changed);
              continue;
            }

          if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
            {
              if (!flatpak_mkdir_p (dest, NULL, error))
                return FALSE;
            }
          else
            {
              g_autoptr(GFile) dest_parent = g_file_get_parent (dest);

              if (!flatpak_mkdir_p (dest_parent, NULL, error))
                return FALSE;

              if (!g_file_delete (dest, NULL, &my_error) &&
                  !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                {
                  g_propagate_error (error, g_steal_pointer (&my_error));
                  return FALSE;
                }
              g_clear_error (&my_error);

              if (g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK)
                {
                  if (!g_file_make_symbolic_link (dest,
                                                  g_file_info_get_symlink_target (info),
                                                  NULL, error))
                    return FALSE;
                }
              else
                {
                  g_autofree char *src_path = g_file_get_path (src);
                  g_autofree char *dest_path = g_file_get_path (dest);

                  if (link (src_path, dest_path))
                    {
                      glnx_set_error_from_errno (error);
                      return FALSE;
                    }
                }
            }
        }

      if (self->cleanup_platform_commands)
        {
          g_auto(GStrv) env = builder_options_get_env (self->build_options, self, context);
          g_auto(GStrv) build_args = builder_options_get_build_args (self->build_options, self, context, error);
          if (!build_args)
            return FALSE;
          char *platform_args[] = { "--sdk-dir=platform", "--metadata=metadata.platform", NULL };
          g_auto(GStrv) extra_args = strcatv (build_args, platform_args);

          for (i = 0; self->cleanup_platform_commands[i] != NULL; i++)
            {
              if (!command (app_dir, env, extra_args, self->cleanup_platform_commands[i], error))
                return FALSE;
            }
        }

      if (self->separate_locales && locale_dir && g_file_query_exists (locale_dir, NULL))
        {
          g_autoptr(GFile) metadata_file = NULL;
          g_autofree char *extension_contents = NULL;
          g_autoptr(GFileOutputStream) output = NULL;
          g_autoptr(GFile) metadata_locale_file = NULL;
          g_autofree char *metadata_contents = NULL;
          g_autofree char *locale_id = builder_manifest_get_locale_id_platform (self);

          metadata_file = g_file_get_child (app_dir, "metadata.platform");

          extension_contents = g_strdup_printf ("\n"
                                                "[Extension %s]\n"
                                                "directory=%s\n"
                                                "autodelete=true\n"
                                                "locale-subset=true\n",
                                                locale_id,
                                                LOCALES_SEPARATE_DIR);

          output = g_file_append_to (metadata_file, G_FILE_CREATE_NONE, NULL, error);
          if (output == NULL)
            return FALSE;

          if (!g_output_stream_write_all (G_OUTPUT_STREAM (output),
                                          extension_contents, strlen (extension_contents),
                                          NULL, NULL, error))
            return FALSE;


          metadata_locale_file = g_file_get_child (app_dir, "metadata.platform.locale");
          metadata_contents = g_strdup_printf ("[Runtime]\n"
                                               "name=%s\n"
                                               "\n"
                                               "[ExtensionOf]\n"
                                               "ref=%s\n", locale_id, ref);
          if (!g_file_set_contents (flatpak_file_get_path_cached (metadata_locale_file),
                                    metadata_contents, strlen (metadata_contents),
                                    error))
            return FALSE;

          g_ptr_array_add (sub_ids, g_strdup (locale_id));
        }

      if (sub_ids->len > 0)
        {
          g_autoptr(GFile) metadata_file = NULL;
          g_autoptr(GFileOutputStream) output = NULL;
          g_autoptr(GString) extension_contents = g_string_new ("\n"
                                                                "[Build]\n");

          g_string_append (extension_contents, FLATPAK_METADATA_KEY_BUILD_EXTENSIONS"=");
          for (i = 0; i < sub_ids->len; i++)
            {
              g_string_append (extension_contents, (const char *)sub_ids->pdata[i]);
              g_string_append (extension_contents, ";");
            }

          metadata_file = g_file_get_child (app_dir, "metadata.platform");
          output = g_file_append_to (metadata_file, G_FILE_CREATE_NONE, NULL, error);
          if (output == NULL)
            return FALSE;

          if (!g_output_stream_write_all (G_OUTPUT_STREAM (output),
                                          extension_contents->str, extension_contents->len,
                                          NULL, NULL, error))
            return FALSE;
        }

      if (!builder_context_disable_rofiles (context, error))
        return FALSE;

      if (!builder_cache_commit (cache, "Created platform", error))
        return FALSE;
    }
  else
    {
      g_print ("Cache hit for create platform, skipping\n");
    }

  return TRUE;
}

gboolean
builder_manifest_bundle_sources (BuilderManifest *self,
                                 BuilderCache    *cache,
                                 BuilderContext  *context,
                                 GError         **error)
{

  builder_manifest_checksum_for_bundle_sources (self, builder_cache_get_checksum (cache), context);
  if (!builder_cache_lookup (cache, "bundle-sources"))
    {
      g_autofree char *sources_id = builder_manifest_get_sources_id (self);
      GFile *app_dir;
      g_autoptr(GFile) metadata_sources_file = NULL;
      g_autoptr(GFile) metadata = NULL;
      g_autoptr(GFile) json_dir = NULL;
      g_autofree char *manifest_filename = NULL;
      g_autoptr(GFile) manifest_file = NULL;
      g_autofree char *metadata_contents = NULL;
      g_autoptr(GKeyFile) metadata_keyfile = g_key_file_new ();
      g_autoptr(GPtrArray) subs = g_ptr_array_new ();
      g_auto(GStrv) old_subs = NULL;
      g_autoptr(GList) enabled_modules = NULL;
      gsize i;
      GList *l;

      g_print ("Bundling sources\n");

      builder_set_term_title (_("Bunding sources for %s"), self->id);

      if (!builder_context_enable_rofiles (context, error))
        return FALSE;

      app_dir = builder_context_get_app_dir (context);
      metadata_sources_file = g_file_get_child (app_dir, "metadata.sources");
      metadata_contents = g_strdup_printf ("[Runtime]\n"
                                           "name=%s\n", sources_id);
      if (!g_file_set_contents (flatpak_file_get_path_cached (metadata_sources_file),
                                metadata_contents, strlen (metadata_contents), error))
        return FALSE;

      json_dir = g_file_resolve_relative_path (app_dir, "sources/manifest");
      if (!flatpak_mkdir_p (json_dir, NULL, error))
        return FALSE;

      manifest_filename = g_strconcat (self->id, ".json", NULL);
      manifest_file = g_file_get_child (json_dir, manifest_filename);
      if (!g_file_set_contents (flatpak_file_get_path_cached (manifest_file),
                                self->manifest_contents, strlen (self->manifest_contents), error))
        return FALSE;


      enabled_modules = builder_manifest_get_enabled_modules (self, context);
      for (l = enabled_modules; l != NULL; l = l->next)
        {
          BuilderModule *m = l->data;

          if (!builder_module_bundle_sources (m, context, error))
            return FALSE;
        }


      metadata = g_file_get_child (app_dir, "metadata");
      if (!g_key_file_load_from_file (metadata_keyfile,
                                      flatpak_file_get_path_cached (metadata),
                                      G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS,
                                      error))
        {
          g_prefix_error (error, "Can't load main metadata file: ");
          return FALSE;
        }

      old_subs = g_key_file_get_string_list (metadata_keyfile, "Build", "built-extensions", NULL, NULL);
      for (i = 0; old_subs != NULL && old_subs[i] != NULL; i++)
        g_ptr_array_add (subs, old_subs[i]);
      g_ptr_array_add (subs, sources_id);

      g_key_file_set_string_list (metadata_keyfile, FLATPAK_METADATA_GROUP_BUILD,
                                  FLATPAK_METADATA_KEY_BUILD_EXTENSIONS,
                                  (const char * const *)subs->pdata, subs->len);

      if (!g_key_file_save_to_file (metadata_keyfile,
                                    flatpak_file_get_path_cached (metadata),
                                    error))
        {
          g_prefix_error (error, "Can't save metadata.platform: ");
          return FALSE;
        }

      if (!builder_context_disable_rofiles (context, error))
        return FALSE;

      if (!builder_cache_commit (cache, "Bundled sources", error))
        return FALSE;
    }
  else
    {
      g_print ("Cache hit for bundle-sources, skipping\n");
    }

  return TRUE;
}
