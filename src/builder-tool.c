/*
 * Copyright © 2015 Red Hat, Inc
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

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib/gi18n.h>
#include <gio/gio.h>
#include "libglnx/libglnx.h"

#include "builder-flatpak-utils.h"
#include "builder-manifest.h"
#include "builder-utils.h"
#include "builder-git.h"

static gboolean opt_verbose;
static gboolean opt_version;
static gboolean opt_all;
static char *opt_arch;
static char *opt_start_at;
static char *opt_start_after;
static char *opt_stop_at;
static char *opt_stop_after;
static char *opt_appdir;
static gboolean opt_disable_updates;
static gboolean opt_disable_download;

static GOptionEntry main_entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
  { NULL }
};

static GOptionEntry context_entries[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Architecture to apply for", "ARCH" },
  { NULL }
};

static GOptionEntry selection_entries[] = {
  { "start-at", 0, 0, G_OPTION_ARG_STRING, &opt_start_at, "Start at this module", "MODULENAME"},
  { "start-after", 0, 0, G_OPTION_ARG_STRING, &opt_start_after, "Start after this module", "MODULENAME"},
  { "stop-at", 0, 0, G_OPTION_ARG_STRING, &opt_stop_at, "Stop at this module", "MODULENAME"},
  { "stop-after", 0, 0, G_OPTION_ARG_STRING, &opt_stop_after, "Stop after this module", "MODULENAME"},
  { NULL }
};

static GOptionEntry modules_entries[] = {
  { "all", 0, 0, G_OPTION_ARG_NONE, &opt_all, "List all (not just enabled) modules"},
  { NULL }
};

static GOptionEntry build_entries[] = {
  { "appdir", 0, 0, G_OPTION_ARG_FILENAME, &opt_appdir, "Build in this appdir"},
  { "disable-download", 0, 0, G_OPTION_ARG_NONE, &opt_disable_download, "Don't download any new sources", NULL },
  { "disable-updates", 0, 0, G_OPTION_ARG_NONE, &opt_disable_updates, "Only download missing sources, never update to latest vcs version", NULL },
  { NULL }
};

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("FB: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

static int
usage (GOptionContext *context,
       const char *format,
       ...)
{
  g_autofree gchar *help = g_option_context_get_help (context, TRUE, NULL);
  g_autofree gchar *message = NULL;
  va_list args;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  g_printerr ("%s\n", message);
  g_printerr ("%s", help);
  return 1;
}

static int
json (GOptionContext *context,
      int argc,
      char **argv)
{
  g_autoptr(GFile) manifest_file = NULL;
  g_autoptr(BuilderManifest) manifest = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *json = NULL;

  if (argc < 2)
    return usage (context, "Must specify a manifest file");

  manifest_file = g_file_new_for_path (argv[1]);

  manifest = builder_manifest_load (manifest_file, &error);
  if (manifest == NULL)
    {
      g_printerr ("Error loading '%s': %s\n", argv[1], error->message);
      return 1;
    }

  json = builder_manifest_serialize (manifest);
  g_print ("%s\n", json);

  return 0;
}

static BuilderContext *
get_build_context (void)
{
  g_autoptr(BuilderContext) build_context = NULL;
  g_autoptr(GFile) app_dir = NULL;

  if (opt_appdir)
    app_dir = g_file_new_for_path (opt_appdir);

  build_context = builder_context_new (app_dir, NULL);
  builder_context_set_use_rofiles (build_context, FALSE);
  if (opt_arch)
    builder_context_set_arch (build_context, opt_arch);

  return g_steal_pointer (&build_context);
}

static int
modules (GOptionContext *context,
         int argc,
         char **argv)
{
  g_autoptr(GFile) manifest_file = NULL;
  g_autoptr(BuilderManifest) manifest = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(BuilderContext) build_context = NULL;
  g_autoptr(GList) modules = NULL;
  gboolean found_start = FALSE;
  GList *l;

  if (argc < 2)
    return usage (context, "Must specify a manifest file");

  manifest_file = g_file_new_for_path (argv[1]);

  manifest = builder_manifest_load (manifest_file, &error);
  if (manifest == NULL)
    {
      g_printerr ("Error loading '%s': %s\n", argv[1], error->message);
      return 1;
    }

  build_context = get_build_context ();
  if (opt_all)
    modules = builder_manifest_get_all_modules (manifest);
  else
    modules = builder_manifest_get_enabled_modules (manifest, build_context);

  for (l = modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;
      const char *name = builder_module_get_name (m);

      if (opt_start_at != NULL && !found_start)
        {
          if (strcmp (name, opt_start_at) == 0)
            found_start = TRUE;
          else
            continue;
        }

      if (opt_start_after != NULL && !found_start)
        {
          if (strcmp (name, opt_start_after) == 0)
            found_start = TRUE;
          continue;
        }

      if (opt_stop_at != NULL && strcmp (name, opt_stop_at) == 0)
        break;

      g_print ("%s\n", name);

      if (opt_stop_after != NULL && strcmp (name, opt_stop_after) == 0)
        break;
    }

  return 0;
}

static int
module (GOptionContext *context,
        int argc,
        char **argv)
{
  g_autoptr(GFile) manifest_file = NULL;
  g_autoptr(BuilderManifest) manifest = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *json = NULL;
  BuilderModule *module;

  if (argc < 2)
    return usage (context, "Must specify a manifest file");

  if (argc < 3)
    return usage (context, "Must specify a module file");

  manifest_file = g_file_new_for_path (argv[1]);

  manifest = builder_manifest_load (manifest_file, &error);
  if (manifest == NULL)
    {
      g_printerr ("Error loading '%s': %s\n", argv[1], error->message);
      return 1;
    }

  module = builder_manifest_get_module (manifest, argv[2]);
  if (module == NULL)
    {
      g_printerr ("Error: No module named '%s'\n", argv[2]);
      return 1;
    }

  json = builder_module_serialize (module);
  g_print ("%s\n", json);

  return 0;
}

static gboolean
do_build (BuilderModule *module,
          BuilderContext *context,
          GError **error)
{
  g_autoptr(GFile) build_dir = NULL;
  const char *name = builder_module_get_name (module);

  build_dir = builder_context_allocate_build_subdir (context, name, error);
  if (build_dir == NULL)
    return FALSE;

  if (!builder_module_extract_sources (module, build_dir, context, error))
    return FALSE;

  if (!builder_module_configure (module, context, build_dir, error))
    return FALSE;

  if (!builder_module_build (module, context, build_dir, error))
    return FALSE;

  if (!builder_context_delete_build_dir (context, build_dir, name, error))
    return FALSE;

  return TRUE;
}

static int
build_module (GOptionContext *option_context,
              int argc,
              char **argv)
{
  g_autoptr(GFile) manifest_file = NULL;
  g_autoptr(BuilderManifest) manifest = NULL;
  g_autoptr(BuilderContext) context = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) build_dir = NULL;
  BuilderModule *module;

  if (argc < 2)
    return usage (option_context, "Must specify a manifest file");

  if (argc < 3)
    return usage (option_context, "Must specify a module file");

  manifest_file = g_file_new_for_path (argv[1]);

  manifest = builder_manifest_load (manifest_file, &error);
  if (manifest == NULL)
    {
      g_printerr ("Error loading '%s': %s\n", argv[1], error->message);
      return 1;
    }

  module = builder_manifest_get_module (manifest, argv[2]);
  if (module == NULL)
    {
      g_printerr ("Error: No module named '%s'\n", argv[2]);
      return 1;
    }

  context = get_build_context ();

  if (!opt_disable_download &&
      !builder_module_download_sources (module, !opt_disable_updates, context, &error))
    {
      g_printerr ("Error: '%s'\n", error->message);
      return 1;
    }

  if (!do_build (module, context, &error))
    {
      g_printerr ("Error: '%s'\n", error->message);
      return 1;
    }

  return 0;
}


static int
build (GOptionContext *option_context,
              int argc,
              char **argv)
{
  g_autoptr(GFile) manifest_file = NULL;
  g_autoptr(BuilderManifest) manifest = NULL;
  g_autoptr(BuilderContext) context = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GList) modules = NULL;
  gboolean found_start = FALSE;
  GList *l;

  if (argc < 2)
    return usage (option_context, "Must specify a manifest file");

  manifest_file = g_file_new_for_path (argv[1]);

  manifest = builder_manifest_load (manifest_file, &error);
  if (manifest == NULL)
    {
      g_printerr ("Error loading '%s': %s\n", argv[1], error->message);
      return 1;
    }

  context = get_build_context ();

  modules = builder_manifest_get_enabled_modules (manifest, context);

  for (l = modules; l != NULL; l = l->next)
    {
      BuilderModule *module = l->data;
      const char *name = builder_module_get_name (module);

      if (opt_start_at != NULL && !found_start)
        {
          if (strcmp (name, opt_start_at) == 0)
            found_start = TRUE;
          else
            continue;
        }

      if (opt_start_after != NULL && !found_start)
        {
          if (strcmp (name, opt_start_after) == 0)
            found_start = TRUE;
          continue;
        }

      if (opt_stop_at != NULL && strcmp (name, opt_stop_at) == 0)
        break;

      if (!opt_disable_download &&
          !builder_module_download_sources (module, !opt_disable_updates, context, &error))
        {
          g_printerr ("Error: '%s'\n", error->message);
          return 1;
        }

      if (!do_build (module, context, &error))
        {
          g_printerr ("Error: '%s'\n", error->message);
          return 1;
        }

      if (opt_stop_after != NULL && strcmp (name, opt_stop_after) == 0)
        break;
    }

  return 0;
}

typedef struct {
  const char *name;
  int (*main)(GOptionContext *context,
              int argc,
              char **argv);
  const char *usage;
  GOptionEntry *extra_entries[4];
} CommandData;

static CommandData commands[] = {
  { "json", json, "json FILE", {} },
  { "modules", modules, "modules FILE", { context_entries, selection_entries, modules_entries } },
  { "module", module, "modules FILE MODULE", { } },
  { "build-module", build_module, "build-module FILE MODULE", { context_entries, build_entries  } },
  { "build", build, "build FILE", { context_entries, selection_entries, build_entries  } },
  { NULL, NULL, NULL, {}}
};

int
main (int    argc,
      char **argv)
{
  g_autofree const char *old_env = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(BuilderManifest) manifest = NULL;
  g_autoptr(GOptionContext) context = NULL;
  int i, first_non_arg;
  CommandData *command = NULL;
  const char *command_name = NULL;

  setlocale (LC_ALL, "");

  g_log_set_handler (NULL, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  g_set_prgname (argv[0]);

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  old_env = g_strdup (g_getenv ("GIO_USE_VFS"));
  g_setenv ("GIO_USE_VFS", "local", TRUE);
  g_vfs_get_default ();
  if (old_env)
    g_setenv ("GIO_USE_VFS", old_env, TRUE);
  else
    g_unsetenv ("GIO_USE_VFS");


  /* Work around libsoup/glib race condition, as per:
     https://bugzilla.gnome.org/show_bug.cgi?id=796031 and
     https://bugzilla.gnome.org/show_bug.cgi?id=674885#c87 */
  g_type_ensure (G_TYPE_SOCKET_FAMILY);
  g_type_ensure (G_TYPE_SOCKET_TYPE);
  g_type_ensure (G_TYPE_SOCKET_PROTOCOL);
  g_type_ensure (G_TYPE_SOCKET_ADDRESS);

  first_non_arg = 1;
  for (i = 1; i < argc; i++)
    {
      if (argv[i][0] != '-')
        break;
      first_non_arg = i + 1;
    }

  command_name = argv[first_non_arg];
  if (command_name != NULL)
    {
     for (i = first_non_arg; i < argc - 1; i++)
       argv[i] = argv[i+1];
     argc--;

     for (i = 0; commands[i].name != NULL; i++)
        {
          if (strcmp (commands[i].name, command_name) == 0)
            {
              command = &commands[i];
              break;
            }
        }
    }

  context = g_option_context_new (command ? command->usage :"COMMAND ...");
  g_option_context_add_main_entries (context, main_entries, NULL);

  if (command_name != NULL && command == NULL)
       return usage (context, "Unknown command %s", command_name);

  if (command && command->extra_entries)
    {
      for (i = 0; i < G_N_ELEMENTS (command->extra_entries) && command->extra_entries[i] != NULL; i++)
        g_option_context_add_main_entries (context, command->extra_entries[i], NULL);
    }

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s\n", error->message);
      return 1;
    }

  if (opt_version)
    {
      g_print ("%s\n", PACKAGE_STRING);
      exit (EXIT_SUCCESS);
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  if (command == NULL)
    return usage (context, "Must specify a command");

  return command->main (context, argc, argv);
}
