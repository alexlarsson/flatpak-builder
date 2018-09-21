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

#ifndef __BUILDER_MODULE_PRIVATE_H__
#define __BUILDER_MODULE_PRIVATE_H__

#include "builder-module.h"

G_BEGIN_DECLS

struct BuilderModule
{
  GObject         parent;

  BuilderManifest *manifest; /* non-owning ref */
  char           *json_path;
  char           *name;
  char           *subdir;
  char          **post_install;
  char          **config_opts;
  char          **make_args;
  char          **make_install_args;
  char           *install_rule;
  char           *test_rule;
  char           *buildsystem;
  char          **ensure_writable;
  char          **only_arches;
  char          **skip_arches;
  gboolean        disabled;
  gboolean        rm_configure;
  gboolean        no_autogen;
  gboolean        no_parallel_make;
  gboolean        no_make_install;
  gboolean        no_python_timestamp_fix;
  gboolean        cmake;
  gboolean        builddir;
  gboolean        run_tests;
  BuilderOptions *build_options;
  GPtrArray      *changes;
  char          **cleanup;
  char          **cleanup_platform;
  GList          *sources;
  GList          *modules;
  char          **build_commands;
  char          **test_commands;
};

typedef struct
{
  GObjectClass parent_class;
} BuilderModuleClass;

G_END_DECLS

#endif /* __BUILDER_MODULE_PRIVATE_H__ */
