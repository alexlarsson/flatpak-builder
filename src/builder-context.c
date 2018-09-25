/* builder-context.c
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
#include <sys/prctl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <glib/gi18n.h>
#include "builder-flatpak-utils.h"
#include "builder-context.h"
#include "builder-utils.h"

struct BuilderContext
{
  GObject         parent;

  GFile          *app_dir;
  GFile          *run_dir; /* directory flatpak-builder was started from */
  GFile          *base_dir; /* directory with json manifest, origin for source files */
  char           *state_subdir;
  SoupSession    *soup_session;
  CURL           *curl_session;
  char           *arch;
  char           *stop_at;

  GFile          *download_dir;
  GPtrArray      *sources_dirs;
  GPtrArray      *sources_urls;
  GFile          *state_dir;
  GFile          *build_dir;
  GFile          *cache_dir;
  GFile          *checksums_dir;
  GFile          *ccache_dir;
  GFile          *rofiles_dir;
  GFile          *rofiles_allocated_dir;
  GLnxLockFile   rofiles_file_lock;

  gboolean        keep_build_dirs;
  gboolean        delete_build_dirs;
  int             jobs;
  gboolean        use_ccache;
  gboolean        bundle_sources;
  gboolean        sandboxed;
  gboolean        rebuild_on_sdk_change;
  gboolean        use_rofiles;
  gboolean        have_rofiles;
  gboolean        run_tests;
  gboolean        no_shallow_clone;

  BuilderSdkConfig *sdk_config;
};

typedef struct
{
  GObjectClass parent_class;
} BuilderContextClass;

G_DEFINE_TYPE (BuilderContext, builder_context, G_TYPE_OBJECT);

enum {
  PROP_0,
  PROP_APP_DIR,
  PROP_RUN_DIR,
  PROP_STATE_SUBDIR,
  LAST_PROP
};


static void
builder_context_finalize (GObject *object)
{
  BuilderContext *self = (BuilderContext *) object;

  g_clear_object (&self->state_dir);
  g_clear_object (&self->download_dir);
  g_clear_object (&self->build_dir);
  g_clear_object (&self->cache_dir);
  g_clear_object (&self->checksums_dir);
  g_clear_object (&self->rofiles_dir);
  g_clear_object (&self->ccache_dir);
  g_clear_object (&self->app_dir);
  g_clear_object (&self->run_dir);
  g_clear_object (&self->base_dir);
  g_clear_object (&self->soup_session);
  g_clear_object (&self->sdk_config);
  g_free (self->arch);
  g_free (self->state_subdir);
  g_free (self->stop_at);
  glnx_release_lock_file(&self->rofiles_file_lock);

  g_clear_pointer (&self->sources_dirs, g_ptr_array_unref);
  g_clear_pointer (&self->sources_urls, g_ptr_array_unref);

  curl_easy_cleanup (self->curl_session);
  self->curl_session = NULL;
  curl_global_cleanup ();

  G_OBJECT_CLASS (builder_context_parent_class)->finalize (object);
}

static void
builder_context_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  BuilderContext *self = BUILDER_CONTEXT (object);

  switch (prop_id)
    {
    case PROP_RUN_DIR:
      g_value_set_object (value, self->run_dir);
      break;

    case PROP_APP_DIR:
      g_value_set_object (value, self->app_dir);
      break;

    case PROP_STATE_SUBDIR:
      g_value_set_string (value, self->state_subdir);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_context_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  BuilderContext *self = BUILDER_CONTEXT (object);

  switch (prop_id)
    {
    case PROP_RUN_DIR:
      g_set_object (&self->run_dir, g_value_get_object (value));
      break;

    case PROP_APP_DIR:
      g_set_object (&self->app_dir, g_value_get_object (value));
      break;

    case PROP_STATE_SUBDIR:
      g_free (self->state_subdir);
      self->state_subdir = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_context_constructed (GObject *object)
{
  BuilderContext *self = BUILDER_CONTEXT (object);

  self->state_dir = g_file_resolve_relative_path (self->run_dir, self->state_subdir ? self->state_subdir : ".flatpak-builder");
  self->download_dir = g_file_get_child (self->state_dir, "downloads");
  self->build_dir = g_file_get_child (self->state_dir, "build");
  self->cache_dir = g_file_get_child (self->state_dir, "cache");
  self->checksums_dir = g_file_get_child (self->state_dir, "checksums");
  self->ccache_dir = g_file_get_child (self->state_dir, "ccache");
}

static void
builder_context_class_init (BuilderContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = builder_context_constructed;
  object_class->finalize = builder_context_finalize;
  object_class->get_property = builder_context_get_property;
  object_class->set_property = builder_context_set_property;

  g_object_class_install_property (object_class,
                                   PROP_APP_DIR,
                                   g_param_spec_object ("app-dir",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
  g_object_class_install_property (object_class,
                                   PROP_RUN_DIR,
                                   g_param_spec_object ("run-dir",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
  g_object_class_install_property (object_class,
                                   PROP_STATE_SUBDIR,
                                   g_param_spec_string ("state-subdir",
                                                        "",
                                                        "",
                                                        "",
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
}

static void
builder_context_init (BuilderContext *self)
{
  GLnxLockFile init = { 0, };
  g_autofree char *path = NULL;

  self->rofiles_file_lock = init;
  path = g_find_program_in_path ("rofiles-fuse");
  self->have_rofiles = path != NULL;
}

GFile *
builder_context_get_run_dir (BuilderContext *self)
{
  return self->run_dir;
}

GFile *
builder_context_get_base_dir (BuilderContext *self)
{
  return self->base_dir;
}

void
builder_context_set_base_dir (BuilderContext *self,
                              GFile          *base_dir)
{
  g_set_object (&self->base_dir, base_dir);
}

GFile *
builder_context_get_state_dir (BuilderContext *self)
{
  return self->state_dir;
}

GFile *
builder_context_get_app_dir_raw (BuilderContext *self)
{
  return self->app_dir;
}

GFile *
builder_context_get_app_dir (BuilderContext *self)
{
  if (self->rofiles_dir)
    return self->rofiles_dir;
  return self->app_dir;
}

GFile *
builder_context_get_download_dir (BuilderContext *self)
{
  return self->download_dir;
}

GPtrArray *
builder_context_get_sources_dirs (BuilderContext *self)
{
  return self->sources_dirs;
}

void
builder_context_set_sources_dirs (BuilderContext *self,
                                  GPtrArray      *sources_dirs)
{
  g_clear_pointer (&self->sources_dirs, g_ptr_array_unref);
  self->sources_dirs = g_ptr_array_ref (sources_dirs);
}

GFile *
builder_context_find_in_sources_dirs_va (BuilderContext *self,
                                         va_list args)
{
  int i;

  if (self->sources_dirs == NULL)
    return NULL;

  for (i = 0; i < self->sources_dirs->len; i++)
    {
      GFile *dir = g_ptr_array_index (self->sources_dirs, i);
      g_autoptr(GFile) local_file = NULL;
      va_list args2;

      va_copy (args2, args);
      local_file = flatpak_build_file_va (dir, args2);
      va_end (args2);

      if (g_file_query_exists (local_file, NULL))
        return g_steal_pointer (&local_file);
    }

  return NULL;
}

GFile *
builder_context_find_in_sources_dirs (BuilderContext *self,
                                      ...)
{
  GFile *res;
  va_list args;

  va_start (args, self);
  res = builder_context_find_in_sources_dirs_va (self, args);
  va_end (args);

  return res;
}

GPtrArray *
builder_context_get_sources_urls (BuilderContext *self)
{
  return self->sources_urls;
}

void
builder_context_set_sources_urls (BuilderContext *self,
                                  GPtrArray      *sources_urls)
{
  g_clear_pointer (&self->sources_urls, g_ptr_array_unref);
  self->sources_urls = g_ptr_array_ref (sources_urls);
}

gboolean
builder_context_download_uri (BuilderContext *self,
                              const char     *url,
                              const char    **mirrors,
                              GFile          *dest,
                              const char     *checksums[BUILDER_CHECKSUMS_LEN],
                              GChecksumType   checksums_type[BUILDER_CHECKSUMS_LEN],
                              GError        **error)
{
  int i;
  g_autoptr(SoupURI) original_uri = soup_uri_new (url);
  g_autoptr(GError) first_error = NULL;

  if (original_uri == NULL)
    return flatpak_fail (error, _("Could not parse URI “%s”"), url);

  g_print ("Downloading %s\n", url);

  if (self->sources_urls != NULL)
    {
      g_autofree char *base_name = g_path_get_basename (soup_uri_get_path (original_uri));
      g_autofree char *rel = g_build_filename ("downloads", checksums[0], base_name, NULL);

      for (i = 0; i < self->sources_urls->len; i++)
        {
          SoupURI *base_uri = g_ptr_array_index (self->sources_urls, i);
          g_autoptr(SoupURI) mirror_uri = soup_uri_new_with_base (base_uri, rel);
          g_autofree char *mirror_uri_str = soup_uri_to_string (mirror_uri, FALSE);
          g_print ("Trying mirror %s\n", mirror_uri_str);
          g_autoptr(GError) my_error = NULL;

          if (builder_download_uri (mirror_uri,
                                    dest,
                                    checksums, checksums_type,
                                    builder_context_get_curl_session (self),
                                    &my_error))
            return TRUE;

          if (!g_error_matches (my_error, BUILDER_CURL_ERROR, CURLE_REMOTE_FILE_NOT_FOUND))
            g_warning ("Error downloading from mirror: %s\n", my_error->message);
        }
    }

  if (!builder_download_uri (original_uri,
                             dest,
                             checksums, checksums_type,
                             builder_context_get_curl_session (self),
                             &first_error))
    {
      gboolean mirror_ok = FALSE;

      if (mirrors != NULL && mirrors[0] != NULL)
        {
          g_print ("Error downloading, trying mirrors\n");
          for (i = 0; mirrors[i] != NULL; i++)
            {
              g_autoptr(GError) mirror_error = NULL;
              g_autoptr(SoupURI) mirror_uri = soup_uri_new (mirrors[i]);
              g_print ("Trying mirror %s\n", mirrors[i]);
              if (!builder_download_uri (mirror_uri,
                                         dest,
                                         checksums, checksums_type,
                                         builder_context_get_curl_session (self),
                                         &mirror_error))
                {
                  g_print ("Error downloading mirror: %s\n", mirror_error->message);
                }
              else
                {
                  mirror_ok = TRUE;
                  break;
                }
            }
        }

      if (!mirror_ok)
        {
          g_propagate_error (error, g_steal_pointer (&first_error));
          return FALSE;
        }
    }

  return TRUE;
}

GFile *
builder_context_get_cache_dir (BuilderContext *self)
{
  return self->cache_dir;
}

GFile *
builder_context_get_build_dir (BuilderContext *self)
{
  return self->build_dir;
}

char *
builder_context_get_checksum_for (BuilderContext *self,
                                  const char *name)
{
  g_autofree char *checksum_name = g_strdup_printf ("%s-%s", builder_context_get_arch (self), name);
  g_autoptr(GFile) checksum_file = g_file_get_child (self->checksums_dir, checksum_name);
  g_autofree gchar *checksum = NULL;

  if (!g_file_get_contents (flatpak_file_get_path_cached (checksum_file), &checksum, NULL, NULL))
    return NULL;

  return g_steal_pointer (&checksum);
}

gboolean
builder_context_set_checksum_for (BuilderContext  *self,
                                  const char      *name,
                                  const char      *checksum,
                                  GError         **error)
{
  g_autofree char *checksum_name = g_strdup_printf ("%s-%s", builder_context_get_arch (self), name);
  g_autoptr(GFile) checksum_file = g_file_get_child (self->checksums_dir, checksum_name);

  if (!flatpak_mkdir_p (self->checksums_dir,
                        NULL, error))
    return FALSE;

  return g_file_set_contents (flatpak_file_get_path_cached (checksum_file), checksum, -1, error);
}

GFile *
builder_context_allocate_build_subdir (BuilderContext *self,
                                       const char *name,
                                       GError **error)
{
  g_autoptr(GError) my_error = NULL;
  int count;

  if (!flatpak_mkdir_p (self->build_dir,
                        NULL, error))
    return NULL;

  for (count = 1; count < 1000; count++)
    {
      g_autofree char *buildname = NULL;
      g_autoptr(GFile) subdir = NULL;

      buildname = g_strdup_printf ("%s-%d", name, count);
      subdir = g_file_get_child (self->build_dir, buildname);

      if (g_file_make_directory (subdir, NULL, &my_error))
        {
          g_autoptr(GFile) build_link = NULL;
          g_autoptr(GError) my_error = NULL;

          /* Make an unversioned symlink */
          build_link = g_file_get_child (self->build_dir, name);
          if (!g_file_delete (build_link, NULL, &my_error) &&
              !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_propagate_error (error, g_steal_pointer (&my_error));
              g_prefix_error (error, "module %s: ", name);
              return NULL;
            }
          g_clear_error (&my_error);

          if (!g_file_make_symbolic_link (build_link, buildname, NULL, error))
            {
              g_prefix_error (error, "module %s: ", name);
              return NULL;
            }

          return g_steal_pointer (&subdir);
        }
      else
        {
          if (!g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
              g_propagate_error (error, g_steal_pointer (&my_error));
              return NULL;
            }
          g_clear_error (&my_error);
          /* Already exists, try again */
        }
    }

  flatpak_fail (error, "Unable to allocate build dir for %s\n", name);
  return NULL;
}

gboolean
builder_context_delete_build_dir (BuilderContext  *self,
                                  GFile *build_dir,
                                  const char *name,
                                  GError **error)
{
  g_autoptr(GFile) build_parent_dir = NULL;
  g_autoptr(GFile) build_link = NULL;
  gboolean res = TRUE;

  build_parent_dir = g_file_get_parent (build_dir);
  build_link = g_file_get_child (build_parent_dir, name);

  builder_set_term_title (_("Cleanup %s"), name);

  if (!g_file_delete (build_link, NULL, error))
    {
      g_prefix_error (error, "module %s: ", name);
      error = NULL; /* Don't report more errors */
      res = FALSE;
    }

  if (!flatpak_rm_rf (build_dir, NULL, error))
    {
      g_prefix_error (error, "module %s: ", name);
      error = NULL; /* Don't report more errors */
      res = FALSE;
    }

  return res;
}


GFile *
builder_context_get_ccache_dir (BuilderContext *self)
{
  return self->ccache_dir;
}

SoupSession *
builder_context_get_soup_session (BuilderContext *self)
{
  if (self->soup_session == NULL)
    self->soup_session = flatpak_create_soup_session ("flatpak-builder " PACKAGE_VERSION);

  return self->soup_session;
}

CURL *
builder_context_get_curl_session (BuilderContext *self)
{
  if (self->curl_session == NULL)
    self->curl_session = flatpak_create_curl_session ("flatpak-builder " PACKAGE_VERSION);

  return self->curl_session;
}

const char *
builder_context_get_arch (BuilderContext *self)
{
  if (self->arch == NULL)
    self->arch = g_strdup (flatpak_get_arch ());

  return (const char *) self->arch;
}

void
builder_context_set_arch (BuilderContext *self,
                          const char     *arch)
{
  g_free (self->arch);
  self->arch = g_strdup (arch);
}

const char *
builder_context_get_stop_at (BuilderContext *self)
{
  return self->stop_at;
}

void
builder_context_set_stop_at (BuilderContext *self,
                             const char     *module)
{
  g_free (self->stop_at);
  self->stop_at = g_strdup (module);
}

int
builder_context_get_jobs (BuilderContext *self)
{
  if (self->jobs == 0)
    return (int) sysconf (_SC_NPROCESSORS_ONLN);
  return self->jobs;
}

void
builder_context_set_jobs (BuilderContext *self,
                          int jobs)
{
  self->jobs = jobs;
}

void
builder_context_set_keep_build_dirs (BuilderContext *self,
                                     gboolean        keep_build_dirs)
{
  self->keep_build_dirs = keep_build_dirs;
}

void
builder_context_set_delete_build_dirs (BuilderContext *self,
                                       gboolean        delete_build_dirs)
{
  self->delete_build_dirs = delete_build_dirs;
}

gboolean
builder_context_get_keep_build_dirs (BuilderContext *self)
{
  return self->keep_build_dirs;
}

gboolean
builder_context_get_delete_build_dirs (BuilderContext *self)
{
  return self->delete_build_dirs;
}

void
builder_context_set_sandboxed (BuilderContext *self,
                               gboolean        sandboxed)
{
  self->sandboxed = sandboxed;
}

gboolean
builder_context_get_sandboxed (BuilderContext *self)
{
  return self->sandboxed;
}

gboolean
builder_context_get_bundle_sources (BuilderContext *self)
{
  return self->bundle_sources;
}

void
builder_context_set_bundle_sources (BuilderContext *self,
                                    gboolean        bundle_sources)
{
  self->bundle_sources = !!bundle_sources;
}

static char *rofiles_unmount_path = NULL;

static void
rofiles_umount_handler (int signum)
{
  char *argv[] = { "fusermount", "-uz", NULL,
                     NULL };

  argv[2] = rofiles_unmount_path;
  g_debug ("unmounting rofiles-fuse %s", rofiles_unmount_path);
  g_spawn_sync (NULL, (char **)argv, NULL,
                G_SPAWN_SEARCH_PATH | G_SPAWN_CLOEXEC_PIPES | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                NULL, NULL, NULL, NULL, NULL, NULL);
  exit (0);
}

static void
rofiles_child_setup (gpointer user_data)
{
  struct rlimit limit;

  /* I had issues with rofiles-fuse running into EMFILE, so
     lets push it as far up as we can */

  if (getrlimit (RLIMIT_NOFILE, &limit) == 0 &&
      limit.rlim_max != limit.rlim_cur)
    {
      limit.rlim_cur  = limit.rlim_max;
      setrlimit (RLIMIT_NOFILE, &limit);
    }
}

gboolean
builder_context_enable_rofiles (BuilderContext *self,
                                GError        **error)
{
  g_autoptr(GFile) rofiles_base = NULL;
  g_autoptr(GFile) rofiles_dir = NULL;
  g_autofree char *tmpdir_name = NULL;
  char *argv[] = { "rofiles-fuse",
                   "-o",
                   "kernel_cache,entry_timeout=60,attr_timeout=60,splice_write,splice_move",
                   (char *)flatpak_file_get_path_cached (self->app_dir),
                   NULL,
                   NULL };
  gint exit_status;
  pid_t child;

  if (!self->use_rofiles)
    return TRUE;

  if (!self->have_rofiles)
    {
      g_warning ("rofiles-fuse not available, doing without");
      return TRUE;
    }

  g_assert (self->rofiles_dir == NULL);

  if (self->rofiles_allocated_dir == NULL)
    {
      rofiles_base = g_file_get_child (self->state_dir, "rofiles");
      if (g_mkdir_with_parents (flatpak_file_get_path_cached (rofiles_base), 0755) != 0)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      if (!flatpak_allocate_tmpdir (AT_FDCWD,
                                    flatpak_file_get_path_cached (rofiles_base),
                                    "rofiles-",
                                    &tmpdir_name, NULL,
                                    &self->rofiles_file_lock,
                                    NULL, NULL, error))
        return FALSE;

      self->rofiles_allocated_dir = g_file_get_child (rofiles_base, tmpdir_name);

      /* Make sure we unmount the fuse fs if flatpak-builder dies unexpectedly */
      rofiles_unmount_path = (char *)flatpak_file_get_path_cached (self->rofiles_allocated_dir);
      child = fork ();
      if (child == -1)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      if (child == 0)
        {
          /* In child */
          struct sigaction new_action;

          prctl (PR_SET_PDEATHSIG, SIGHUP);

          new_action.sa_handler = rofiles_umount_handler;
          sigemptyset (&new_action.sa_mask);
          new_action.sa_flags = 0;
          sigaction (SIGHUP, &new_action, NULL);

          sigset (SIGINT, SIG_IGN);
          sigset (SIGPIPE, SIG_IGN);
          sigset (SIGSTOP, SIG_IGN);

          while (TRUE)
            pause ();

          exit (0);
        }
    }

  rofiles_dir = g_object_ref (self->rofiles_allocated_dir);
  argv[4] = (char *)flatpak_file_get_path_cached (rofiles_dir);

  g_debug ("starting: rofiles-fuse %s %s", argv[3], argv[4]);
  if (!g_spawn_sync (NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_CLOEXEC_PIPES, rofiles_child_setup, NULL, NULL, NULL, &exit_status, error))
    {
      g_prefix_error (error, "Can't spawn rofiles-fuse");
      return FALSE;
    }
  else if (exit_status != 0)
    {
      return flatpak_fail (error, "Failure spawning rofiles-fuse, exit_status: %d", exit_status);
    }


  self->rofiles_dir = g_steal_pointer (&rofiles_dir);

  return TRUE;
}

gboolean
builder_context_disable_rofiles (BuilderContext *self,
                                 GError        **error)
{
  char *argv[] = { "fusermount", "-u", NULL,
                     NULL };

  if (!self->use_rofiles)
    return TRUE;

  if (!self->have_rofiles)
    return TRUE;

  g_assert (self->rofiles_dir != NULL);

  argv[2] = (char *)flatpak_file_get_path_cached (self->rofiles_dir);

  g_debug ("unmounting rofiles-fuse %s", rofiles_unmount_path);
  g_spawn_sync (NULL, (char **)argv, NULL,
                G_SPAWN_SEARCH_PATH | G_SPAWN_CLOEXEC_PIPES,
                NULL, NULL, NULL, NULL, NULL, NULL);

  g_clear_object (&self->rofiles_dir);

  return TRUE;
}

gboolean
builder_context_get_rofiles_active (BuilderContext *self)
{
  return self->rofiles_dir != NULL;
}

gboolean
builder_context_get_use_rofiles (BuilderContext *self)
{
  return self->use_rofiles;
}

void
builder_context_set_use_rofiles (BuilderContext *self,
                                 gboolean use_rofiles)
{
  self->use_rofiles = use_rofiles;
}

gboolean
builder_context_get_run_tests (BuilderContext *self)
{
  return self->run_tests;
}

void
builder_context_set_run_tests (BuilderContext *self,
                               gboolean run_tests)
{
  self->run_tests = run_tests;
}

void
builder_context_set_no_shallow_clone (BuilderContext *self,
                                      gboolean        no_shallow_clone)
{
  self->no_shallow_clone = no_shallow_clone;
}

gboolean
builder_context_get_no_shallow_clone (BuilderContext *self)
{
  return self->no_shallow_clone;
}

gboolean
builder_context_get_rebuild_on_sdk_change (BuilderContext *self)
{
  return self->rebuild_on_sdk_change;
}

void
builder_context_set_rebuild_on_sdk_change (BuilderContext *self,
                                           gboolean        rebuild_on_sdk_change)
{
  self->rebuild_on_sdk_change = !!rebuild_on_sdk_change;
}

gboolean
builder_context_set_enable_ccache (BuilderContext *self,
                                   gboolean        enable,
                                   GError        **error)
{
  int i;

  self->use_ccache = !!enable;

  if (enable)
    {
      g_autofree char *ccache_path = g_file_get_path (self->ccache_dir);
      g_autofree char *ccache_bin_path = g_build_filename (ccache_path, "bin", NULL);
      static const char *compilers[] = {
        "cc",
        "c++",
        "gcc",
        "g++"
      };

      if (g_mkdir_with_parents (ccache_bin_path, 0755) != 0)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      for (i = 0; i < G_N_ELEMENTS (compilers); i++)
        {
          const char *symlink_path = g_build_filename (ccache_bin_path, compilers[i], NULL);
          if (symlink ("/usr/bin/ccache", symlink_path) && errno != EEXIST)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
        }
    }
  else
    {
      g_autoptr(GFile) ccache_disabled_dir = g_file_get_child (self->ccache_dir, "disabled");
      g_autofree char *ccache_disabled_path = g_file_get_path (ccache_disabled_dir);
      g_autofree char *ccache_config_path = g_build_filename (ccache_disabled_path, "ccache.conf", NULL);
      g_autoptr(GError) my_error = NULL;

      if (g_mkdir_with_parents (ccache_disabled_path, 0755) != 0)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      if (!g_file_set_contents (ccache_config_path, "disable = true\n", -1, &my_error))
        {
          if (!g_error_matches (my_error, G_FILE_ERROR, G_FILE_ERROR_EXIST))
            {
              g_propagate_error (error, g_steal_pointer (&my_error));
              return FALSE;
            }
          g_clear_error (&my_error);
        }
    }

  return TRUE;
}

char **
builder_context_extend_env (BuilderContext *self,
                            char          **envp)
{
  g_autofree char *path = NULL;
  const char *ccache_dir = NULL;

  path = g_strdup (g_environ_getenv (envp, "PATH"));
  if (path == NULL)
    path = g_strdup ("/app/bin:/usr/bin"); /* This is the flatpak default PATH, we alway set it so we can easily append to it */

  if (self->use_ccache)
    {
      char *new_path = g_strdup_printf ("/run/ccache/bin:%s", path);
      g_free (path);
      path = new_path;
      ccache_dir = "/run/ccache";
    }
  else
    {
      ccache_dir = "/run/ccache/disabled";
    }

  envp = g_environ_setenv (envp, "CCACHE_DIR", ccache_dir, TRUE);
  envp = g_environ_setenv (envp, "PATH", path, TRUE);

  return envp;
}

gboolean
builder_context_load_sdk_config (BuilderContext   *self,
                                 const char       *sdk_path,
                                 GError          **error)
{
  g_autoptr(GFile) root = g_file_new_for_path (sdk_path);
  g_autoptr(GFile) config_file = g_file_resolve_relative_path (root, "files/etc/flatpak-builder/defaults.json");
  g_autoptr(GError) local_error = NULL;
  g_autoptr(BuilderSdkConfig) sdk_config = NULL;

  sdk_config = builder_sdk_config_from_file (config_file, &local_error);
  if (sdk_config == NULL &&
      !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  g_set_object (&self->sdk_config, sdk_config);
  return TRUE;
}

BuilderSdkConfig *
builder_context_get_sdk_config (BuilderContext *self)
{
  return self->sdk_config;
}

BuilderContext *
builder_context_new (GFile *app_dir,
                     const char *state_subdir)
{
  g_autofree char *cwd = NULL;
  g_autoptr(GFile) cwd_dir = NULL;

  cwd = g_get_current_dir ();
  cwd_dir = g_file_new_for_path (cwd);

  return g_object_new (BUILDER_TYPE_CONTEXT,
                       "run-dir", cwd_dir,
                       "app-dir", app_dir,
                       "state-subdir", state_subdir,
                       NULL);
}

static GPtrArray *
setup_build_args (BuilderContext  *context,
                  GFile           *source_dir,
                  const char      *cwd_subdir,
                  const char      *source_dir_alias,
                  char           **flatpak_opts,
                  char           **env_vars,
                  GFile          **cwd_file)
{
  GFile *app_dir = builder_context_get_app_dir (context);
  g_autoptr(GPtrArray) args = NULL;
  g_autofree char *source_dir_path = g_file_get_path (source_dir);
  g_autofree char *source_dir_path_canonical = NULL;
  g_autofree char *ccache_dir_path = NULL;
  const char *build_dir;
  int i;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build"));
  g_ptr_array_add (args, g_strdup ("--die-with-parent"));
  source_dir_path_canonical = realpath (source_dir_path, NULL);
  if (source_dir_path_canonical == NULL)
    source_dir_path_canonical = g_strdup (source_dir_path);


  g_ptr_array_add (args, g_strdup ("--nofilesystem=host"));

  /* We mount the canonical location, because bind-mounts of symlinks don't really work */
  g_ptr_array_add (args, g_strdup_printf ("--filesystem=%s", source_dir_path_canonical));

  /* Also make sure the original path is available (if it was not canonical, in case something references that. */
  if (strcmp (source_dir_path_canonical, source_dir_path) != 0)
    g_ptr_array_add (args, g_strdup_printf ("--bind-mount=%s=%s", source_dir_path, source_dir_path_canonical));

  if (g_file_query_exists (builder_context_get_ccache_dir (context), NULL))
    {
      ccache_dir_path = g_file_get_path (builder_context_get_ccache_dir (context));
      g_ptr_array_add (args, g_strdup_printf ("--bind-mount=/run/ccache=%s", ccache_dir_path));
    }

  if (source_dir_alias)
    {
      g_ptr_array_add (args, g_strdup_printf ("--bind-mount=%s=%s", source_dir_alias, source_dir_path_canonical));
      build_dir = source_dir_alias;
    }
  else
    build_dir = source_dir_path_canonical;

  if (cwd_subdir)
    g_ptr_array_add (args, g_strdup_printf ("--build-dir=%s/%s", build_dir, cwd_subdir));
  else
    g_ptr_array_add (args, g_strdup_printf ("--build-dir=%s", build_dir));

  g_ptr_array_add (args, g_strdup_printf ("--env=FLATPAK_BUILDER_BUILDDIR=%s", build_dir));

  if (flatpak_opts)
    {
      for (i = 0; flatpak_opts[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup (flatpak_opts[i]));
    }

  if (env_vars)
    {
      for (i = 0; env_vars[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup_printf ("--env=%s", env_vars[i]));
    }

  g_ptr_array_add (args, g_file_get_path (app_dir));

  *cwd_file = g_file_new_for_path (source_dir_path_canonical);

  return g_steal_pointer (&args);
}

gboolean
builder_context_spawnv (BuilderContext  *context,
                        GFile           *source_dir,
                        const char      *source_subdir,
                        const char      *source_dir_alias,
                        char           **flatpak_opts,
                        char           **env_vars,
                        const gchar * const  *argv,
                        GError         **error)
{
  g_autoptr(GFile) cwd_file = NULL;
  g_autoptr(GPtrArray) args = NULL;
  int i;

  if (context->app_dir == NULL)
    {
      g_auto(GStrv) env = NULL;
      g_autofree char *builddir = NULL;
      gint exit_status;

      if (source_subdir)
        cwd_file = g_file_resolve_relative_path (source_dir, source_subdir);
      else
        cwd_file = g_object_ref (source_dir);

      env = g_get_environ ();

      env = g_environ_setenv (env, "FLATPAK_BUILDER_BUILDDIR", flatpak_file_get_path_cached (cwd_file), TRUE);
      if (env_vars)
        {
          for (i = 0; env_vars[i] != NULL; i++)
            {
              g_auto(GStrv) s = g_strsplit (env_vars[i], "=", 2);
              env = g_environ_setenv (env, s[0], s[1], TRUE);
            }
        }

      if (!g_spawn_sync (flatpak_file_get_path_cached (cwd_file),
                         (char **)argv, env, G_SPAWN_SEARCH_PATH,
                         NULL, NULL, NULL, NULL, &exit_status, error))
        return FALSE;

      if (!g_spawn_check_exit_status (exit_status, error))
        return FALSE;

      return TRUE;
    }

  args = setup_build_args (context, source_dir, source_subdir, source_dir_alias, flatpak_opts, env_vars, &cwd_file);
  for (i = 0; argv[i] != NULL; i++)
    g_ptr_array_add (args, g_strdup (argv[i]));
  g_ptr_array_add (args, NULL);

  return builder_maybe_host_spawnv (cwd_file, NULL, 0, error, (const char * const *)args->pdata);
}

gboolean
builder_context_execv (BuilderContext  *context,
                       GFile           *source_dir,
                       const char      *source_subdir,
                       const char      *source_dir_alias,
                       char           **flatpak_opts,
                       char           **env_vars,
                       const gchar * const  *argv,
                       GError         **error)
{
  g_autoptr(GFile) cwd_file = NULL;
  g_autoptr(GPtrArray) args = NULL;
  int i;

  args = setup_build_args (context, source_dir, source_subdir, source_dir_alias, flatpak_opts, env_vars, &cwd_file);
  for (i = 0; argv[i] != NULL; i++)
    g_ptr_array_add (args, g_strdup (argv[i]));
  g_ptr_array_add (args, NULL);

  if (chdir (flatpak_file_get_path_cached (cwd_file)))
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (execvp ((char *) args->pdata[0], (char **) args->pdata) == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to start flatpak build");
      return FALSE;
    }

  g_assert_not_reached ();
}
