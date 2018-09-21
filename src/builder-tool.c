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

static GOptionEntry main_entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
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

typedef struct {
  const char *name;
  int (*main)(GOptionContext *context,
              int argc,
              char **argv);
  const char *usage;
  GOptionEntry *extra_entries;
} CommandData;

static int
json (GOptionContext *context,
      int argc,
      char **argv)
{
  g_autoptr(GFile) manifest_file = NULL;
  g_autoptr(BuilderManifest) manifest = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *json = NULL;

  if (argc != 2)
    return usage (context, "Must specify a manifest file");

  manifest_file = g_file_new_for_path (argv[2]);

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

int
main (int    argc,
      char **argv)
{
  g_autofree const char *old_env = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(BuilderManifest) manifest = NULL;
  g_autoptr(GOptionContext) context = NULL;
  int i, first_non_arg;
  CommandData commands[] = {
    { "json", json, "json FILE", NULL },
    { NULL }
  };
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
     for (i = first_non_arg; i < argc-1; i++)
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
    g_option_context_add_main_entries (context, command->extra_entries, NULL);

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
