/* builder-source-shell.c
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

#include "builder-flatpak-utils.h"
#include "builder-utils.h"
#include "builder-source-shell.h"
#include "builder-manifest.h"
#include "builder-checksum.h"

struct BuilderSourceShell
{
  BuilderSource parent;

  char        **commands;
};

typedef struct
{
  BuilderSourceClass parent_class;
} BuilderSourceShellClass;

G_DEFINE_TYPE (BuilderSourceShell, builder_source_shell, BUILDER_TYPE_SOURCE);

enum {
  PROP_0,
  PROP_COMMANDS,
  LAST_PROP
};

static void
builder_source_shell_finalize (GObject *object)
{
  BuilderSourceShell *self = (BuilderSourceShell *) object;

  g_strfreev (self->commands);

  G_OBJECT_CLASS (builder_source_shell_parent_class)->finalize (object);
}

static void
builder_source_shell_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  BuilderSourceShell *self = BUILDER_SOURCE_SHELL (object);

  switch (prop_id)
    {
    case PROP_COMMANDS:
      g_value_set_boxed (value, self->commands);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_source_shell_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  BuilderSourceShell *self = BUILDER_SOURCE_SHELL (object);
  gchar **tmp;

  switch (prop_id)
    {
    case PROP_COMMANDS:
      tmp = self->commands;
      self->commands = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static gboolean
builder_source_shell_download (BuilderSource  *source,
                               gboolean        update_vcs,
                               BuilderContext *context,
                               GError        **error)
{
  return TRUE;
}

static gboolean
run_script (BuilderManifest *manifest,
            BuilderContext *context,
            BuilderOptions *build_options,
            GFile          *source_dir,
            const gchar    *script,
            GError        **error)
{
  g_autofree char *source_dir_path = g_file_get_path (source_dir);
  g_auto(GStrv) build_args = NULL;
  g_auto(GStrv) env_vars = NULL;
  const char *args[] = { "/bin/sh", "-c", "ARG", NULL};

  env_vars = builder_options_get_env (build_options, manifest, context);

  build_args = builder_options_get_build_args (build_options, manifest, context, error);
  if (build_args == NULL)
    return FALSE;

  args[2] = script;

  return builder_context_spawnv (context, source_dir, NULL, NULL,
                                 build_args, env_vars, (const char * const *)args,
                                 error);
}


static gboolean
builder_source_shell_extract (BuilderSource  *source,
                              GFile          *dest,
                              BuilderOptions *build_options,
                              BuilderManifest *manifest,
                              BuilderContext *context,
                              GError        **error)
{
  BuilderSourceShell *self = BUILDER_SOURCE_SHELL (source);
  int i;

  if (self->commands)
    {
      for (i = 0; self->commands[i] != NULL; i++)
        {
          if (!run_script (manifest, context, build_options,
                           dest, self->commands[i], error))
            return FALSE;
        }
    }


  return TRUE;
}

static gboolean
builder_source_shell_bundle (BuilderSource  *source,
                             BuilderContext *context,
                             GError        **error)
{
  /* no need to bundle anything here as this part
     can be reconstructed from the manifest */
  return TRUE;
}

static void
builder_source_shell_checksum (BuilderSource  *source,
                               GChecksum      *checksum,
                               BuilderContext *context)
{
  BuilderSourceShell *self = BUILDER_SOURCE_SHELL (source);

  builder_checksum_strv (checksum, self->commands);
}

static void
builder_source_shell_class_init (BuilderSourceShellClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  BuilderSourceClass *source_class = BUILDER_SOURCE_CLASS (klass);

  object_class->finalize = builder_source_shell_finalize;
  object_class->get_property = builder_source_shell_get_property;
  object_class->set_property = builder_source_shell_set_property;

  source_class->download = builder_source_shell_download;
  source_class->extract = builder_source_shell_extract;
  source_class->bundle = builder_source_shell_bundle;
  source_class->checksum = builder_source_shell_checksum;

  g_object_class_install_property (object_class,
                                   PROP_COMMANDS,
                                   g_param_spec_boxed ("commands",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
}

static void
builder_source_shell_init (BuilderSourceShell *self)
{
}
