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

#ifndef __BUILDER_MANIFEST_PRIVATE_H__
#define __BUILDER_MANIFEST_PRIVATE_H__

#include "builder-manifest.h"

G_BEGIN_DECLS

struct BuilderManifest
{
  GObject         parent;

  char           *manifest_contents;

  char           *id;
  char           *id_platform;
  char           *branch;
  char           *collection_id;
  char           *extension_tag;
  char           *type;
  char           *runtime;
  char           *runtime_commit;
  char           *runtime_version;
  char           *sdk;
  char           *sdk_commit;
  char           *var;
  char           *base;
  char           *base_commit;
  char           *base_version;
  char          **base_extensions;
  char           *metadata;
  char           *metadata_platform;
  gboolean        separate_locales;
  char          **cleanup;
  char          **cleanup_commands;
  char          **cleanup_platform;
  char          **cleanup_platform_commands;
  char          **prepare_platform_commands;
  char          **finish_args;
  char          **inherit_extensions;
  char          **inherit_sdk_extensions;
  char          **tags;
  char           *rename_desktop_file;
  char           *rename_appdata_file;
  char           *appdata_license;
  char           *rename_icon;
  gboolean        copy_icon;
  char           *desktop_file_name_prefix;
  char           *desktop_file_name_suffix;
  gboolean        build_runtime;
  gboolean        build_extension;
  gboolean        writable_sdk;
  gboolean        appstream_compose;
  char          **sdk_extensions;
  char          **platform_extensions;
  char           *command;
  BuilderOptions *build_options;
  GList          *modules;
  GList          *add_extensions;
  GList          *add_build_extensions;
};

typedef struct
{
  GObjectClass parent_class;
} BuilderManifestClass;

void _builder_manifest_set_demarshal_base_dir (GFile *dir);
GFile *_builder_manifest_get_demarshal_base_dir (void);

G_END_DECLS

#endif /* __BUILDER_MANIFEST_PRIVATE_H__ */
