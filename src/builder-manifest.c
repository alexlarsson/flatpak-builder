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

#include <libxml/parser.h>

#include "libglnx/libglnx.h"

/* Shared with builder-module.c */
static GFile *demarshal_base_dir = NULL;

void
_builder_manifest_set_demarshal_base_dir (GFile *dir)
{
  g_set_object (&demarshal_base_dir, dir);
}

GFile *
_builder_manifest_get_demarshal_base_dir (void)
{
  return g_object_ref (demarshal_base_dir);
}

static void serializable_iface_init (JsonSerializableIface *serializable_iface);

G_DEFINE_TYPE_WITH_CODE (BuilderManifest, builder_manifest, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (JSON_TYPE_SERIALIZABLE, serializable_iface_init));

enum {
  PROP_0,
  PROP_APP_ID, /* Backwards compat with early version, use id */
  PROP_ID,
  PROP_ID_PLATFORM,
  PROP_BRANCH,
  PROP_RUNTIME,
  PROP_RUNTIME_VERSION,
  PROP_RUNTIME_COMMIT,
  PROP_SDK,
  PROP_SDK_COMMIT,
  PROP_BASE,
  PROP_BASE_VERSION,
  PROP_BASE_COMMIT,
  PROP_BASE_EXTENSIONS,
  PROP_VAR,
  PROP_METADATA,
  PROP_METADATA_PLATFORM,
  PROP_BUILD_OPTIONS,
  PROP_COMMAND,
  PROP_MODULES,
  PROP_CLEANUP,
  PROP_CLEANUP_COMMANDS,
  PROP_CLEANUP_PLATFORM_COMMANDS,
  PROP_CLEANUP_PLATFORM,
  PROP_PREPARE_PLATFORM_COMMANDS,
  PROP_BUILD_RUNTIME,
  PROP_BUILD_EXTENSION,
  PROP_SEPARATE_LOCALES,
  PROP_WRITABLE_SDK,
  PROP_APPSTREAM_COMPOSE,
  PROP_SDK_EXTENSIONS,
  PROP_PLATFORM_EXTENSIONS,
  PROP_FINISH_ARGS,
  PROP_INHERIT_EXTENSIONS,
  PROP_INHERIT_SDK_EXTENSIONS,
  PROP_TAGS,
  PROP_RENAME_DESKTOP_FILE,
  PROP_RENAME_APPDATA_FILE,
  PROP_APPDATA_LICENSE,
  PROP_RENAME_ICON,
  PROP_COPY_ICON,
  PROP_DESKTOP_FILE_NAME_PREFIX,
  PROP_DESKTOP_FILE_NAME_SUFFIX,
  PROP_COLLECTION_ID,
  PROP_ADD_EXTENSIONS,
  PROP_ADD_BUILD_EXTENSIONS,
  PROP_EXTENSION_TAG,
  LAST_PROP
};

static void
builder_manifest_finalize (GObject *object)
{
  BuilderManifest *self = (BuilderManifest *) object;

  g_free (self->manifest_contents);

  g_free (self->id);
  g_free (self->branch);
  g_free (self->collection_id);
  g_free (self->extension_tag);
  g_free (self->runtime);
  g_free (self->runtime_commit);
  g_free (self->runtime_version);
  g_free (self->sdk);
  g_free (self->sdk_commit);
  g_free (self->base);
  g_free (self->base_commit);
  g_free (self->base_version);
  g_free (self->var);
  g_free (self->metadata);
  g_free (self->metadata_platform);
  g_free (self->command);
  g_clear_object (&self->build_options);
  g_list_free_full (self->modules, g_object_unref);
  g_list_free_full (self->add_extensions, g_object_unref);
  g_list_free_full (self->add_build_extensions, g_object_unref);
  g_strfreev (self->cleanup);
  g_strfreev (self->cleanup_commands);
  g_strfreev (self->cleanup_platform);
  g_strfreev (self->cleanup_platform_commands);
  g_strfreev (self->prepare_platform_commands);
  g_strfreev (self->finish_args);
  g_strfreev (self->inherit_extensions);
  g_strfreev (self->inherit_sdk_extensions);
  g_strfreev (self->tags);
  g_free (self->rename_desktop_file);
  g_free (self->rename_appdata_file);
  g_free (self->appdata_license);
  g_free (self->rename_icon);
  g_free (self->desktop_file_name_prefix);
  g_free (self->desktop_file_name_suffix);

  G_OBJECT_CLASS (builder_manifest_parent_class)->finalize (object);
}

static gboolean
init_modules (BuilderManifest *manifest,
              GList *modules,
              GHashTable *names,
              GError **error)
{
  GList *l;

  for (l = modules; l; l = l->next)
    {
      BuilderModule *m = l->data;
      const char *name;

      builder_module_set_manifest (m, manifest);

      if (!init_modules (manifest, builder_module_get_modules (m), names, error))
        return FALSE;

      name = builder_module_get_name (m);
      if (name == NULL)
        {
          /* FIXME: We'd like to report *something* for the user
                    to locate the errornous module definition.
           */
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Module has no 'name' attribute set");
          return FALSE;
        }

      if (g_hash_table_lookup (names, name) != NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Duplicate modules named '%s'", name);
          return FALSE;
        }

      g_hash_table_insert (names, (char *)name, (char *)name);
    }

  return TRUE;
}

BuilderManifest *
builder_manifest_load (GFile *file,
                       GError **error)
{
  g_autoptr(BuilderManifest) manifest = NULL;
  g_autoptr(GFile) base_dir = g_file_get_parent (file);
  g_autofree char *basename = g_file_get_basename (file);
  g_autofree gchar *contents = NULL;
  g_autoptr(GHashTable) names = g_hash_table_new (g_str_hash, g_str_equal);

  if (!g_file_get_contents (flatpak_file_get_path_cached (file), &contents, NULL, error))
    return NULL;

  /* Can't push this as user data to the demarshalling :/ */
  _builder_manifest_set_demarshal_base_dir (base_dir);

  manifest = (BuilderManifest *) builder_gobject_from_data (BUILDER_TYPE_MANIFEST, basename,
                                                            contents, error);

  _builder_manifest_set_demarshal_base_dir (NULL);

  if (manifest == NULL)
    return NULL;

  if (manifest->build_runtime && manifest->build_extension)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't build both a runtime and an extension");
      return FALSE;
    }

  if (!init_modules (manifest, manifest->modules, names, error))
    return NULL;

  manifest->manifest_contents = g_steal_pointer (&contents);
  return g_steal_pointer (&manifest);
}

char *
builder_manifest_get_content_checksum (BuilderManifest *self)
{
  return g_compute_checksum_for_string (G_CHECKSUM_SHA256, self->manifest_contents, -1);
}

static void
builder_manifest_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  BuilderManifest *self = BUILDER_MANIFEST (object);

  switch (prop_id)
    {
    case PROP_APP_ID:
      g_value_set_string (value, NULL);
      break;

    case PROP_ID:
      g_value_set_string (value, self->id);
      break;

    case PROP_ID_PLATFORM:
      g_value_set_string (value, self->id_platform);
      break;

    case PROP_BRANCH:
      g_value_set_string (value, self->branch);
      break;

    case PROP_RUNTIME:
      g_value_set_string (value, self->runtime);
      break;

    case PROP_RUNTIME_COMMIT:
      g_value_set_string (value, self->runtime_commit);
      break;

    case PROP_RUNTIME_VERSION:
      g_value_set_string (value, self->runtime_version);
      break;

    case PROP_SDK:
      g_value_set_string (value, self->sdk);
      break;

    case PROP_SDK_COMMIT:
      g_value_set_string (value, self->sdk_commit);
      break;

    case PROP_BASE:
      g_value_set_string (value, self->base);
      break;

    case PROP_BASE_COMMIT:
      g_value_set_string (value, self->base_commit);
      break;

    case PROP_BASE_VERSION:
      g_value_set_string (value, self->base_version);
      break;

    case PROP_BASE_EXTENSIONS:
      g_value_set_boxed (value, self->base_extensions);
      break;

    case PROP_VAR:
      g_value_set_string (value, self->var);
      break;

    case PROP_METADATA:
      g_value_set_string (value, self->metadata);
      break;

    case PROP_METADATA_PLATFORM:
      g_value_set_string (value, self->metadata_platform);
      break;

    case PROP_COMMAND:
      g_value_set_string (value, self->command);
      break;

    case PROP_BUILD_OPTIONS:
      g_value_set_object (value, self->build_options);
      break;

    case PROP_MODULES:
      g_value_set_pointer (value, self->modules);
      break;

    case PROP_ADD_EXTENSIONS:
      g_value_set_pointer (value, self->add_extensions);
      break;

    case PROP_ADD_BUILD_EXTENSIONS:
      g_value_set_pointer (value, self->add_build_extensions);
      break;

    case PROP_CLEANUP:
      g_value_set_boxed (value, self->cleanup);
      break;

    case PROP_CLEANUP_COMMANDS:
      g_value_set_boxed (value, self->cleanup_commands);
      break;

    case PROP_CLEANUP_PLATFORM:
      g_value_set_boxed (value, self->cleanup_platform);
      break;

    case PROP_CLEANUP_PLATFORM_COMMANDS:
      g_value_set_boxed (value, self->cleanup_platform_commands);
      break;

    case PROP_PREPARE_PLATFORM_COMMANDS:
      g_value_set_boxed (value, self->prepare_platform_commands);
      break;

    case PROP_FINISH_ARGS:
      g_value_set_boxed (value, self->finish_args);
      break;

    case PROP_INHERIT_EXTENSIONS:
      g_value_set_boxed (value, self->inherit_extensions);
      break;

    case PROP_INHERIT_SDK_EXTENSIONS:
      g_value_set_boxed (value, self->inherit_sdk_extensions);
      break;

    case PROP_TAGS:
      g_value_set_boxed (value, self->tags);
      break;

    case PROP_BUILD_RUNTIME:
      g_value_set_boolean (value, self->build_runtime);
      break;

    case PROP_BUILD_EXTENSION:
      g_value_set_boolean (value, self->build_extension);
      break;

    case PROP_SEPARATE_LOCALES:
      g_value_set_boolean (value, self->separate_locales);
      break;

    case PROP_WRITABLE_SDK:
      g_value_set_boolean (value, self->writable_sdk);
      break;

    case PROP_APPSTREAM_COMPOSE:
      g_value_set_boolean (value, self->appstream_compose);
      break;

    case PROP_SDK_EXTENSIONS:
      g_value_set_boxed (value, self->sdk_extensions);
      break;

    case PROP_PLATFORM_EXTENSIONS:
      g_value_set_boxed (value, self->platform_extensions);
      break;

    case PROP_COPY_ICON:
      g_value_set_boolean (value, self->copy_icon);
      break;

    case PROP_RENAME_DESKTOP_FILE:
      g_value_set_string (value, self->rename_desktop_file);
      break;

    case PROP_RENAME_APPDATA_FILE:
      g_value_set_string (value, self->rename_appdata_file);
      break;

    case PROP_APPDATA_LICENSE:
      g_value_set_string (value, self->appdata_license);
      break;

    case PROP_RENAME_ICON:
      g_value_set_string (value, self->rename_icon);
      break;

    case PROP_DESKTOP_FILE_NAME_PREFIX:
      g_value_set_string (value, self->desktop_file_name_prefix);
      break;

    case PROP_DESKTOP_FILE_NAME_SUFFIX:
      g_value_set_string (value, self->desktop_file_name_suffix);
      break;

    case PROP_COLLECTION_ID:
      g_value_set_string (value, self->collection_id);
      break;

    case PROP_EXTENSION_TAG:
      g_value_set_string (value, self->extension_tag);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_manifest_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  BuilderManifest *self = BUILDER_MANIFEST (object);
  gchar **tmp;

  switch (prop_id)
    {
    case PROP_APP_ID:
      g_free (self->id);
      self->id = g_value_dup_string (value);
      break;

    case PROP_ID:
      g_free (self->id);
      self->id = g_value_dup_string (value);
      break;

    case PROP_ID_PLATFORM:
      g_free (self->id_platform);
      self->id_platform = g_value_dup_string (value);
      break;

    case PROP_BRANCH:
      g_free (self->branch);
      self->branch = g_value_dup_string (value);
      break;

    case PROP_RUNTIME:
      g_free (self->runtime);
      self->runtime = g_value_dup_string (value);
      break;

    case PROP_RUNTIME_COMMIT:
      g_free (self->runtime_commit);
      self->runtime_commit = g_value_dup_string (value);
      break;

    case PROP_RUNTIME_VERSION:
      g_free (self->runtime_version);
      self->runtime_version = g_value_dup_string (value);
      break;

    case PROP_SDK:
      g_free (self->sdk);
      self->sdk = g_value_dup_string (value);
      break;

    case PROP_SDK_COMMIT:
      g_free (self->sdk_commit);
      self->sdk_commit = g_value_dup_string (value);
      break;

    case PROP_BASE:
      g_free (self->base);
      self->base = g_value_dup_string (value);
      break;

    case PROP_BASE_COMMIT:
      g_free (self->base_commit);
      self->base_commit = g_value_dup_string (value);
      break;

    case PROP_BASE_VERSION:
      g_free (self->base_version);
      self->base_version = g_value_dup_string (value);
      break;

    case PROP_BASE_EXTENSIONS:
      tmp = self->base_extensions;
      self->base_extensions = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_VAR:
      g_free (self->var);
      self->var = g_value_dup_string (value);
      break;

    case PROP_METADATA:
      g_free (self->metadata);
      self->metadata = g_value_dup_string (value);
      break;

    case PROP_METADATA_PLATFORM:
      g_free (self->metadata_platform);
      self->metadata_platform = g_value_dup_string (value);
      break;

    case PROP_COMMAND:
      g_free (self->command);
      self->command = g_value_dup_string (value);
      break;

    case PROP_BUILD_OPTIONS:
      g_set_object (&self->build_options,  g_value_get_object (value));
      break;

    case PROP_MODULES:
      g_list_free_full (self->modules, g_object_unref);
      /* NOTE: This takes ownership of the list! */
      self->modules = g_value_get_pointer (value);
      break;

    case PROP_ADD_EXTENSIONS:
      g_list_free_full (self->add_extensions, g_object_unref);
      /* NOTE: This takes ownership of the list! */
      self->add_extensions = g_value_get_pointer (value);
      break;

    case PROP_ADD_BUILD_EXTENSIONS:
      g_list_free_full (self->add_build_extensions, g_object_unref);
      /* NOTE: This takes ownership of the list! */
      self->add_build_extensions = g_value_get_pointer (value);
      break;

    case PROP_CLEANUP:
      tmp = self->cleanup;
      self->cleanup = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_CLEANUP_COMMANDS:
      tmp = self->cleanup_commands;
      self->cleanup_commands = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_CLEANUP_PLATFORM:
      tmp = self->cleanup_platform;
      self->cleanup_platform = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_CLEANUP_PLATFORM_COMMANDS:
      tmp = self->cleanup_platform_commands;
      self->cleanup_platform_commands = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_PREPARE_PLATFORM_COMMANDS:
      tmp = self->prepare_platform_commands;
      self->prepare_platform_commands = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_FINISH_ARGS:
      tmp = self->finish_args;
      self->finish_args = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_INHERIT_EXTENSIONS:
      tmp = self->inherit_extensions;
      self->inherit_extensions = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_INHERIT_SDK_EXTENSIONS:
      tmp = self->inherit_sdk_extensions;
      self->inherit_sdk_extensions = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_TAGS:
      tmp = self->tags;
      self->tags = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_BUILD_RUNTIME:
      self->build_runtime = g_value_get_boolean (value);
      break;

    case PROP_BUILD_EXTENSION:
      self->build_extension = g_value_get_boolean (value);
      break;

    case PROP_SEPARATE_LOCALES:
      self->separate_locales = g_value_get_boolean (value);
      break;

    case PROP_WRITABLE_SDK:
      self->writable_sdk = g_value_get_boolean (value);
      break;

    case PROP_APPSTREAM_COMPOSE:
      self->appstream_compose = g_value_get_boolean (value);
      break;

    case PROP_SDK_EXTENSIONS:
      tmp = self->sdk_extensions;
      self->sdk_extensions = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_PLATFORM_EXTENSIONS:
      tmp = self->platform_extensions;
      self->platform_extensions = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_COPY_ICON:
      self->copy_icon = g_value_get_boolean (value);
      break;

    case PROP_RENAME_DESKTOP_FILE:
      g_free (self->rename_desktop_file);
      self->rename_desktop_file = g_value_dup_string (value);
      break;

    case PROP_RENAME_APPDATA_FILE:
      g_free (self->rename_appdata_file);
      self->rename_appdata_file = g_value_dup_string (value);
      break;

    case PROP_APPDATA_LICENSE:
      g_free (self->appdata_license);
      self->appdata_license = g_value_dup_string (value);
      break;

    case PROP_RENAME_ICON:
      g_free (self->rename_icon);
      self->rename_icon = g_value_dup_string (value);
      break;

    case PROP_DESKTOP_FILE_NAME_PREFIX:
      g_free (self->desktop_file_name_prefix);
      self->desktop_file_name_prefix = g_value_dup_string (value);
      break;

    case PROP_DESKTOP_FILE_NAME_SUFFIX:
      g_free (self->desktop_file_name_suffix);
      self->desktop_file_name_suffix = g_value_dup_string (value);
      break;

    case PROP_COLLECTION_ID:
      g_free (self->collection_id);
      self->collection_id = g_value_dup_string (value);
      break;

    case PROP_EXTENSION_TAG:
      g_free (self->extension_tag);
      self->extension_tag = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_manifest_class_init (BuilderManifestClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = builder_manifest_finalize;
  object_class->get_property = builder_manifest_get_property;
  object_class->set_property = builder_manifest_set_property;

  g_object_class_install_property (object_class,
                                   PROP_APP_ID,
                                   g_param_spec_string ("app-id",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ID,
                                   g_param_spec_string ("id",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ID_PLATFORM,
                                   g_param_spec_string ("id-platform",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BRANCH,
                                   g_param_spec_string ("branch",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RUNTIME,
                                   g_param_spec_string ("runtime",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RUNTIME_COMMIT,
                                   g_param_spec_string ("runtime-commit",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RUNTIME_VERSION,
                                   g_param_spec_string ("runtime-version",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SDK,
                                   g_param_spec_string ("sdk",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SDK_COMMIT,
                                   g_param_spec_string ("sdk-commit",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BASE,
                                   g_param_spec_string ("base",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BASE_COMMIT,
                                   g_param_spec_string ("base-commit",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BASE_VERSION,
                                   g_param_spec_string ("base-version",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BASE_EXTENSIONS,
                                   g_param_spec_boxed ("base-extensions",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_VAR,
                                   g_param_spec_string ("var",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_METADATA,
                                   g_param_spec_string ("metadata",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_METADATA_PLATFORM,
                                   g_param_spec_string ("metadata-platform",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_COMMAND,
                                   g_param_spec_string ("command",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BUILD_OPTIONS,
                                   g_param_spec_object ("build-options",
                                                        "",
                                                        "",
                                                        BUILDER_TYPE_OPTIONS,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_MODULES,
                                   g_param_spec_pointer ("modules",
                                                         "",
                                                         "",
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ADD_EXTENSIONS,
                                   g_param_spec_pointer ("add-extensions",
                                                         "",
                                                         "",
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ADD_BUILD_EXTENSIONS,
                                   g_param_spec_pointer ("add-build-extensions",
                                                         "",
                                                         "",
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CLEANUP,
                                   g_param_spec_boxed ("cleanup",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CLEANUP_COMMANDS,
                                   g_param_spec_boxed ("cleanup-commands",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CLEANUP_PLATFORM,
                                   g_param_spec_boxed ("cleanup-platform",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CLEANUP_PLATFORM_COMMANDS,
                                   g_param_spec_boxed ("cleanup-platform-commands",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_PREPARE_PLATFORM_COMMANDS,
                                   g_param_spec_boxed ("prepare-platform-commands",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_FINISH_ARGS,
                                   g_param_spec_boxed ("finish-args",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_INHERIT_EXTENSIONS,
                                   g_param_spec_boxed ("inherit-extensions",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_INHERIT_SDK_EXTENSIONS,
                                   g_param_spec_boxed ("inherit-sdk-extensions",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BUILD_RUNTIME,
                                   g_param_spec_boolean ("build-runtime",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BUILD_EXTENSION,
                                   g_param_spec_boolean ("build-extension",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SEPARATE_LOCALES,
                                   g_param_spec_boolean ("separate-locales",
                                                         "",
                                                         "",
                                                         TRUE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_WRITABLE_SDK,
                                   g_param_spec_boolean ("writable-sdk",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_APPSTREAM_COMPOSE,
                                   g_param_spec_boolean ("appstream-compose",
                                                         "",
                                                         "",
                                                         TRUE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SDK_EXTENSIONS,
                                   g_param_spec_boxed ("sdk-extensions",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_PLATFORM_EXTENSIONS,
                                   g_param_spec_boxed ("platform-extensions",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_TAGS,
                                   g_param_spec_boxed ("tags",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RENAME_DESKTOP_FILE,
                                   g_param_spec_string ("rename-desktop-file",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RENAME_APPDATA_FILE,
                                   g_param_spec_string ("rename-appdata-file",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_APPDATA_LICENSE,
                                   g_param_spec_string ("appdata-license",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RENAME_ICON,
                                   g_param_spec_string ("rename-icon",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_COPY_ICON,
                                   g_param_spec_boolean ("copy-icon",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_DESKTOP_FILE_NAME_PREFIX,
                                   g_param_spec_string ("desktop-file-name-prefix",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_DESKTOP_FILE_NAME_SUFFIX,
                                   g_param_spec_string ("desktop-file-name-suffix",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_COLLECTION_ID,
                                   g_param_spec_string ("collection-id",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));

  g_object_class_install_property (object_class,
                                   PROP_EXTENSION_TAG,
                                   g_param_spec_string ("extension-tag",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
builder_manifest_init (BuilderManifest *self)
{
  self->appstream_compose = TRUE;
  self->separate_locales = TRUE;
}

static JsonNode *
builder_manifest_serialize_property (JsonSerializable *serializable,
                                     const gchar      *property_name,
                                     const GValue     *value,
                                     GParamSpec       *pspec)
{
  if (strcmp (property_name, "modules") == 0)
    {
      BuilderManifest *self = BUILDER_MANIFEST (serializable);
      JsonNode *retval = NULL;
      GList *l;

      if (self->modules)
        {
          JsonArray *array;

          array = json_array_sized_new (g_list_length (self->modules));

          for (l = self->modules; l != NULL; l = l->next)
            {
              JsonNode *child = json_gobject_serialize (l->data);
              json_array_add_element (array, child);
            }

          retval = json_node_init_array (json_node_alloc (), array);
          json_array_unref (array);
        }

      return retval;
    }
  else if (strcmp (property_name, "add-extensions") == 0 ||
           strcmp (property_name, "add-build-extensions") == 0)
    {
      BuilderManifest *self = BUILDER_MANIFEST (serializable);
      JsonNode *retval = NULL;
      GList *extensions;

      if (strcmp (property_name, "add-extensions") == 0)
        extensions = self->add_extensions;
      else
        extensions = self->add_build_extensions;

      if (extensions)
        {
          JsonObject *object;
          GList *l;

          object = json_object_new ();

          for (l = extensions; l != NULL; l = l->next)
            {
              BuilderExtension *e = l->data;
              JsonNode *child = json_gobject_serialize (G_OBJECT (e));
              json_object_set_member (object, (char *) builder_extension_get_name (e), child);
            }

          retval = json_node_init_object (json_node_alloc (), object);
          json_object_unref (object);
        }

      return retval;
    }
  else
    {
      return builder_serializable_serialize_property (serializable,
                                                      property_name,
                                                      value,
                                                      pspec);
    }
}

static gint
sort_extension (gconstpointer  a,
                gconstpointer  b)
{
  return strcmp (builder_extension_get_name (BUILDER_EXTENSION (a)),
                 builder_extension_get_name (BUILDER_EXTENSION (b)));
}

static gboolean
builder_manifest_deserialize_property (JsonSerializable *serializable,
                                       const gchar      *property_name,
                                       GValue           *value,
                                       GParamSpec       *pspec,
                                       JsonNode         *property_node)
{
  if (strcmp (property_name, "modules") == 0)
    {
      if (JSON_NODE_TYPE (property_node) == JSON_NODE_NULL)
        {
          g_value_set_pointer (value, NULL);
          return TRUE;
        }
      else if (JSON_NODE_TYPE (property_node) == JSON_NODE_ARRAY)
        {
          JsonArray *array = json_node_get_array (property_node);
          guint i, array_len = json_array_get_length (array);
          g_autoptr(GFile) saved_demarshal_base_dir = _builder_manifest_get_demarshal_base_dir ();
          GList *modules = NULL;
          GObject *module;

          for (i = 0; i < array_len; i++)
            {
              JsonNode *element_node = json_array_get_element (array, i);

              module = NULL;

              if (JSON_NODE_HOLDS_VALUE (element_node) &&
                  json_node_get_value_type (element_node) == G_TYPE_STRING)
                {
                  const char *module_relpath = json_node_get_string (element_node);
                  g_autoptr(GFile) module_file =
                    g_file_resolve_relative_path (demarshal_base_dir, module_relpath);
                  const char *module_path = flatpak_file_get_path_cached (module_file);
                  g_autofree char *module_contents = NULL;
                  g_autoptr(GError) error = NULL;

                  if (g_file_get_contents (module_path, &module_contents, NULL, &error))
                    {
                      g_autoptr(GFile) module_file_dir = g_file_get_parent (module_file);
                      _builder_manifest_set_demarshal_base_dir (module_file_dir);
                      module = builder_gobject_from_data (BUILDER_TYPE_MODULE,
                                                          module_relpath, module_contents, &error);
                      _builder_manifest_set_demarshal_base_dir (saved_demarshal_base_dir);
                      if (module)
                        {
                          builder_module_set_json_path (BUILDER_MODULE (module), module_path);
                          builder_module_set_base_dir (BUILDER_MODULE (module), module_file_dir);
                        }
                    }
                  if (error != NULL)
                    {
                      g_error ("Failed to load included manifest (%s): %s", module_path, error->message);
                    }
                }
              else if (JSON_NODE_HOLDS_OBJECT (element_node))
                {
                  module = json_gobject_deserialize (BUILDER_TYPE_MODULE, element_node);
                  if (module != NULL)
                    builder_module_set_base_dir (BUILDER_MODULE (module), saved_demarshal_base_dir);
                }

              if (module == NULL)
                {
                  g_list_free_full (modules, g_object_unref);
                  return FALSE;
                }

              modules = g_list_prepend (modules, module);
            }

          g_value_set_pointer (value, g_list_reverse (modules));

          return TRUE;
        }

      return FALSE;
    }
  else if (strcmp (property_name, "add-extensions") == 0 ||
           strcmp (property_name, "add-build-extensions") == 0)
    {
      if (JSON_NODE_TYPE (property_node) == JSON_NODE_NULL)
        {
          g_value_set_pointer (value, NULL);
          return TRUE;
        }
      else if (JSON_NODE_TYPE (property_node) == JSON_NODE_OBJECT)
        {
          JsonObject *object = json_node_get_object (property_node);
          g_autoptr(GHashTable) hash = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
          g_autoptr(GList) members = NULL;
          GList *extensions;
          GList *l;

          members = json_object_get_members (object);
          for (l = members; l != NULL; l = l->next)
            {
              const char *member_name = l->data;
              JsonNode *val;
              GObject *extension;

              val = json_object_get_member (object, member_name);
              extension = json_gobject_deserialize (BUILDER_TYPE_EXTENSION, val);
              if (extension == NULL)
                return FALSE;

              builder_extension_set_name (BUILDER_EXTENSION (extension), member_name);
              g_hash_table_insert (hash, (char *)builder_extension_get_name (BUILDER_EXTENSION (extension)), extension);
            }

          extensions = g_hash_table_get_values (hash);
          g_hash_table_steal_all (hash);

          extensions = g_list_sort (extensions, sort_extension);
          g_value_set_pointer (value, extensions);

          return TRUE;
        }

      return FALSE;
    }
  else
    {
      return builder_serializable_deserialize_property (serializable,
                                                        property_name,
                                                        value,
                                                        pspec, property_node);
    }
}

static void
serializable_iface_init (JsonSerializableIface *serializable_iface)
{
  serializable_iface->serialize_property = builder_manifest_serialize_property;
  serializable_iface->deserialize_property = builder_manifest_deserialize_property;
  serializable_iface->find_property = builder_serializable_find_property;
  serializable_iface->list_properties = builder_serializable_list_properties;
  serializable_iface->set_property = builder_serializable_set_property;
  serializable_iface->get_property = builder_serializable_get_property;
}

char *
builder_manifest_serialize (BuilderManifest *self)
{
  JsonNode *node;
  JsonGenerator *generator;
  char *json;

  node = json_gobject_serialize (G_OBJECT (self));
  generator = json_generator_new ();
  json_generator_set_pretty (generator, TRUE);
  json_generator_set_root (generator, node);
  json = json_generator_to_data (generator, NULL);
  g_object_unref (generator);
  json_node_free (node);

  return json;
}

const char *
builder_manifest_get_id (BuilderManifest *self)
{
  return self->id;
}

char *
builder_manifest_get_locale_id (BuilderManifest *self)
{
  g_autofree char *id = flatpak_make_valid_id_prefix (self->id);
  return g_strdup_printf ("%s.Locale", id);
}

char *
builder_manifest_get_debug_id (BuilderManifest *self)
{
  g_autofree char *id = flatpak_make_valid_id_prefix (self->id);
  return g_strdup_printf ("%s.Debug", id);
}

char *
builder_manifest_get_sources_id (BuilderManifest *self)
{
  g_autofree char *id = flatpak_make_valid_id_prefix (self->id);
  return g_strdup_printf ("%s.Sources", id);
}

const char *
builder_manifest_get_id_platform (BuilderManifest *self)
{
  return self->id_platform;
}

char *
builder_manifest_get_locale_id_platform (BuilderManifest *self)
{
  char *res = NULL;

  if (self->id_platform != NULL)
    {
      g_autofree char *id = flatpak_make_valid_id_prefix (self->id_platform);
      res = g_strdup_printf ("%s.Locale", id);
    }

  return res;
}

BuilderOptions *
builder_manifest_get_build_options (BuilderManifest *self)
{
  return self->build_options;
}

GList *
builder_manifest_get_modules (BuilderManifest *self)
{
  return self->modules;
}

GList *
builder_manifest_get_add_extensions (BuilderManifest *self)
{
  return self->add_extensions;
}

GList *
builder_manifest_get_add_build_extensions (BuilderManifest *self)
{
  return self->add_build_extensions;
}

const char *
builder_manifest_get_runtime_version (BuilderManifest *self)
{
  return self->runtime_version ? self->runtime_version : "master";
}

const char *
builder_manifest_get_branch (BuilderManifest *self)
{
  if (self->branch)
    return self->branch;

  return "master";
}

void
builder_manifest_set_default_branch (BuilderManifest *self,
                                     const char *default_branch)
{
  if (self->branch == NULL)
    self->branch = g_strdup (default_branch);
}

const char *
builder_manifest_get_collection_id (BuilderManifest *self)
{
  return self->collection_id;
}

void
builder_manifest_set_default_collection_id (BuilderManifest *self,
                                            const char      *default_collection_id)
{
  if (self->collection_id == NULL)
    self->collection_id = g_strdup (default_collection_id);
}

const char *
builder_manifest_get_extension_tag (BuilderManifest *self)
{
  return self->extension_tag;
}

gboolean
builder_manifest_get_separate_locales (BuilderManifest *self)
{
  return self->separate_locales;
}

BuilderOptions *
builder_manifest_get_options (BuilderManifest *self)
{
  return self->build_options;
}

gboolean
builder_manifest_get_build_runtime (BuilderManifest *self)
{
  return self->build_runtime;
}

gboolean
builder_manifest_get_build_extension (BuilderManifest *self)
{
  return self->build_extension;
}

const char **
builder_manifest_get_cleanup (BuilderManifest *self)
{
  return (const char **) self->cleanup;
}

const char **
builder_manifest_get_cleanup_platform (BuilderManifest *self)
{
  return (const char **) self->cleanup_platform;
}

const char *
builder_manifest_get_base_version (BuilderManifest *self)
{
  return self->base_version ? self->base_version : builder_manifest_get_branch (self);
}

static void
add_installation_args (GPtrArray *args,
                       gboolean opt_user,
                       const char *opt_installation)
{
  if (opt_user)
    g_ptr_array_add (args, g_strdup ("--user"));
  else if (opt_installation)
    g_ptr_array_add (args, g_strdup_printf ("--installation=%s", opt_installation));
  else
    g_ptr_array_add (args, g_strdup ("--system"));
}

static char *
flatpak_info (gboolean opt_user,
              const char *opt_installation,
              const char *ref,
              const char *extra_arg,
              GError **error)
{
  gboolean res;
  g_autofree char *output = NULL;
  g_autoptr(GPtrArray) args = NULL;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  add_installation_args (args, opt_user, opt_installation);
  g_ptr_array_add (args, g_strdup ("info"));
  if (extra_arg)
    g_ptr_array_add (args, g_strdup (extra_arg));
  g_ptr_array_add (args, g_strdup (ref));
  g_ptr_array_add (args, NULL);

  res = builder_maybe_host_spawnv (NULL, &output, G_SUBPROCESS_FLAGS_STDERR_SILENCE, error, (const char * const *)args->pdata);

  if (res)
    {
      g_strchomp (output);
      return g_steal_pointer (&output);
    }
  return NULL;
}

static char *
flatpak_info_show_path (const char *id,
                        const char *branch,
                        BuilderContext  *context)
{
  g_autofree char *sdk_info = NULL;
  g_autofree char *arch_option = NULL;
  g_auto(GStrv) sdk_info_lines = NULL;
  int i;

  /* Unfortunately there is not flatpak info --show-path, so we have to look at the full flatpak info output */

  arch_option = g_strdup_printf ("--arch=%s", builder_context_get_arch (context));

  sdk_info = flatpak (NULL, "info", arch_option, id, branch, NULL);
  if (sdk_info == NULL)
    return NULL;

  sdk_info_lines = g_strsplit (sdk_info, "\n", -1);
  for (i = 0; sdk_info_lines[i] != NULL; i++)
    {
      if (g_str_has_prefix (sdk_info_lines[i], "Location:"))
        return g_strstrip (g_strdup (sdk_info_lines[i] + strlen ("Location:")));
    }

  return NULL;
}

gboolean
builder_manifest_start (BuilderManifest *self,
                        gboolean download_only,
                        gboolean allow_missing_runtimes,
                        BuilderContext  *context,
                        GError         **error)
{
  g_autofree char *arch_option = NULL;
  g_autofree char *sdk_path = NULL;
  const char *stop_at;

  if (self->sdk == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "sdk not specified");
      return FALSE;
    }

  arch_option = g_strdup_printf ("--arch=%s", builder_context_get_arch (context));

  self->sdk_commit = flatpak (NULL, "info", arch_option, "--show-commit", self->sdk,
                              builder_manifest_get_runtime_version (self), NULL);
  if (!download_only && !allow_missing_runtimes && self->sdk_commit == NULL)
    return flatpak_fail (error, "Unable to find sdk %s version %s",
                         self->sdk,
                         builder_manifest_get_runtime_version (self));

  sdk_path = flatpak_info_show_path (self->sdk, builder_manifest_get_runtime_version (self), context);
  if (sdk_path != NULL &&
      !builder_context_load_sdk_config (context, sdk_path, error))
    return FALSE;

  self->runtime_commit = flatpak (NULL, "info", arch_option, "--show-commit", self->runtime,
                                  builder_manifest_get_runtime_version (self), NULL);
  if (!download_only && !allow_missing_runtimes && self->runtime_commit == NULL)
    return flatpak_fail (error, "Unable to find runtime %s version %s",
                         self->runtime,
                         builder_manifest_get_runtime_version (self));

  if (self->base != NULL && *self->base != 0)
    {
      self->base_commit = flatpak (NULL, "info", arch_option, "--show-commit", self->base,
                                   builder_manifest_get_base_version (self), NULL);
      if (!download_only && self->base_commit == NULL)
        return flatpak_fail (error, "Unable to find app %s version %s",
                             self->base, builder_manifest_get_base_version (self));
    }

  stop_at = builder_context_get_stop_at (context);
  if (stop_at != NULL && builder_manifest_get_module (self, stop_at) == NULL)
    return flatpak_fail (error, "No module named %s (specified with --stop-at)", stop_at);

  return TRUE;
}

/* This gets the checksum of everything that globally affects the build */
void
builder_manifest_checksum (BuilderManifest *self,
                           GChecksum      *checksum,
                           BuilderContext  *context)
{
  GList *l;

  builder_checksum_str (checksum, BUILDER_MANIFEST_CHECKSUM_VERSION);
  builder_checksum_str (checksum, self->id);
  /* No need to include version here, it doesn't affect the build */
  builder_checksum_str (checksum, self->runtime);
  builder_checksum_str (checksum, builder_manifest_get_runtime_version (self));
  builder_checksum_str (checksum, self->sdk);
  /* Always rebuild on sdk change if we're actually including the sdk in the cache */
  if (self->writable_sdk || self->build_runtime ||
      builder_context_get_rebuild_on_sdk_change (context))
    builder_checksum_str (checksum, self->sdk_commit);
  builder_checksum_str (checksum, self->var);
  builder_checksum_str (checksum, self->metadata);
  builder_checksum_strv (checksum, self->tags);
  builder_checksum_boolean (checksum, self->writable_sdk);
  builder_checksum_strv (checksum, self->sdk_extensions);
  builder_checksum_boolean (checksum, self->build_runtime);
  builder_checksum_boolean (checksum, self->build_extension);
  builder_checksum_boolean (checksum, self->separate_locales);
  builder_checksum_str (checksum, self->base);
  builder_checksum_str (checksum, self->base_version);
  builder_checksum_str (checksum, self->base_commit);
  builder_checksum_strv (checksum, self->base_extensions);
  builder_checksum_compat_str (checksum, self->extension_tag);

  if (self->build_options)
    builder_options_checksum (self->build_options, checksum, context);

  for (l = self->add_build_extensions; l != NULL; l = l->next)
    {
      BuilderExtension *e = l->data;
      builder_extension_checksum (e, checksum, context);
    }
}

static BuilderModule *
get_module (GList *modules, const char *name)
{
  GList *l;

  for (l = modules; l; l = l->next)
    {
      BuilderModule *m = l->data;
      const char *m_name;

      m_name = builder_module_get_name (m);
      if (strcmp (name, m_name) == 0)
        return m;

      m = get_module (builder_module_get_modules (m), name);
      if (m != NULL)
        return m;
    }

  return NULL;
}

BuilderModule *
builder_manifest_get_module (BuilderManifest *self,
                             const char *name)
{
  return get_module (self->modules, name);
}

static GList *
get_enabled_modules (BuilderContext *context, GList *modules)
{
  GList *enabled = NULL;
  GList *l;

  for (l = modules; l; l = l->next)
    {
      BuilderModule *m = l->data;
      GList *submodules = NULL;

      if (!builder_module_is_enabled (m, context))
        continue;

      submodules = get_enabled_modules (context, builder_module_get_modules (m));

      enabled = g_list_concat (enabled, submodules);
      enabled = g_list_append (enabled, m);
    }
  return enabled;
}

GList *
builder_manifest_get_enabled_modules (BuilderManifest *self,
                                      BuilderContext  *context)
{
  return get_enabled_modules (context, self->modules);
}

void
builder_manifest_checksum_for_cleanup (BuilderManifest *self,
                                       GChecksum      *checksum,
                                       BuilderContext  *context)
{
  g_autoptr(GList) enabled_modules = NULL;
  GList *l;

  builder_checksum_str (checksum, BUILDER_MANIFEST_CHECKSUM_CLEANUP_VERSION);
  builder_checksum_strv (checksum, self->cleanup);
  builder_checksum_strv (checksum, self->cleanup_commands);
  builder_checksum_str (checksum, self->rename_desktop_file);
  builder_checksum_str (checksum, self->rename_appdata_file);
  builder_checksum_str (checksum, self->appdata_license);
  builder_checksum_str (checksum, self->rename_icon);
  builder_checksum_boolean (checksum, self->copy_icon);
  builder_checksum_str (checksum, self->desktop_file_name_prefix);
  builder_checksum_str (checksum, self->desktop_file_name_suffix);
  builder_checksum_boolean (checksum, self->appstream_compose);

  enabled_modules = builder_manifest_get_enabled_modules (self, context);
  for (l = enabled_modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;
      builder_module_checksum_for_cleanup (m, checksum, context);
    }
}

void
builder_manifest_checksum_for_finish (BuilderManifest *self,
                                      GChecksum      *checksum,
                                      BuilderContext  *context)
{
  GList *l;
  g_autofree char *json = NULL;

  builder_checksum_str (checksum, BUILDER_MANIFEST_CHECKSUM_FINISH_VERSION);
  builder_checksum_strv (checksum, self->finish_args);
  builder_checksum_str (checksum, self->command);
  builder_checksum_strv (checksum, self->inherit_extensions);
  builder_checksum_compat_strv (checksum, self->inherit_sdk_extensions);

  for (l = self->add_extensions; l != NULL; l = l->next)
    {
      BuilderExtension *e = l->data;
      builder_extension_checksum (e, checksum, context);
    }

  if (self->metadata)
    {
      GFile *base_dir = builder_context_get_base_dir (context);
      g_autoptr(GFile) metadata = g_file_resolve_relative_path (base_dir, self->metadata);
      g_autofree char *data = NULL;
      g_autoptr(GError) my_error = NULL;
      gsize len;

      if (g_file_load_contents (metadata, NULL, &data, &len, NULL, &my_error))
        builder_checksum_data (checksum, (guchar *) data, len);
      else
        g_warning ("Can't load metadata file %s: %s", self->metadata, my_error->message);
    }

  json = builder_manifest_serialize (self);
  builder_checksum_str (checksum, json);
}

void
builder_manifest_checksum_for_bundle_sources (BuilderManifest *self,
                                              GChecksum      *checksum,
                                              BuilderContext  *context)
{
  builder_checksum_str (checksum, BUILDER_MANIFEST_CHECKSUM_BUNDLE_SOURCES_VERSION);
  builder_checksum_boolean (checksum, builder_context_get_bundle_sources (context));
}

void
builder_manifest_checksum_for_platform (BuilderManifest *self,
                                        GChecksum      *checksum,
                                        BuilderContext  *context)
{
  g_autoptr(GList) enabled_modules = NULL;
  GList *l;

  builder_checksum_str (checksum, BUILDER_MANIFEST_CHECKSUM_PLATFORM_VERSION);
  builder_checksum_str (checksum, self->id_platform);
  builder_checksum_str (checksum, self->runtime_commit);
  builder_checksum_str (checksum, self->metadata_platform);
  builder_checksum_strv (checksum, self->cleanup_platform);
  builder_checksum_strv (checksum, self->cleanup_platform_commands);
  builder_checksum_strv (checksum, self->prepare_platform_commands);
  builder_checksum_strv (checksum, self->platform_extensions);

  if (self->metadata_platform)
    {
      GFile *base_dir = builder_context_get_base_dir (context);
      g_autoptr(GFile) metadata = g_file_resolve_relative_path (base_dir, self->metadata_platform);
      g_autofree char *data = NULL;
      g_autoptr(GError) my_error = NULL;
      gsize len;

      if (g_file_load_contents (metadata, NULL, &data, &len, NULL, &my_error))
        builder_checksum_data (checksum, (guchar *) data, len);
      else
        g_warning ("Can't load metadata-platform file %s: %s", self->metadata_platform, my_error->message);
    }

  enabled_modules = builder_manifest_get_enabled_modules (self, context);
  for (l = enabled_modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;
      builder_module_checksum_for_platform (m, checksum, context);
    }
}

gboolean
builder_manifest_download (BuilderManifest *self,
                           gboolean         update_vcs,
                           const char      *only_module,
                           BuilderContext  *context,
                           GError         **error)
{
  g_autoptr(GList) enabled_modules = NULL;
  const char *stop_at = builder_context_get_stop_at (context);
  GList *l;

  g_print ("Downloading sources\n");
  enabled_modules = builder_manifest_get_enabled_modules (self, context);
  for (l = enabled_modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;
      const char *name = builder_module_get_name (m);

      if (only_module && strcmp (name, only_module) != 0)
        continue;

      if (stop_at != NULL && strcmp (name, stop_at) == 0)
        {
          g_print ("Stopping at module %s\n", stop_at);
          return TRUE;
        }

      if (!builder_module_download_sources (m, update_vcs, context, error))
        return FALSE;
    }

  return TRUE;
}


gboolean
builder_manifest_show_deps (BuilderManifest *self,
                            BuilderContext  *context,
                            GError         **error)
{
  g_autoptr(GHashTable) names = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr(GList) enabled_modules = NULL;
  GList *l;

  enabled_modules = builder_manifest_get_enabled_modules (self, context);
  for (l = enabled_modules; l != NULL; l = l->next)
    {
      BuilderModule *module = l->data;

      if (!builder_module_show_deps (module, context, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
builder_manifest_install_dep (BuilderManifest *self,
                              BuilderContext  *context,
                              const char *remote,
                              gboolean opt_user,
                              const char *opt_installation,
                              const char *runtime,
                              const char *version,
                              gboolean opt_yes,
                              GError         **error)
{
  g_autofree char *ref = NULL;
  g_autofree char *commit = NULL;
  g_autoptr(GPtrArray) args = NULL;
  gboolean installed;

  if (version == NULL)
    version = builder_manifest_get_runtime_version (self);

  ref = flatpak_build_untyped_ref (runtime,
                                   version,
                                   builder_context_get_arch (context));

  commit = flatpak_info (opt_user, opt_installation, "--show-commit", ref, NULL);
  installed = (commit != NULL);

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  add_installation_args (args, opt_user, opt_installation);
  if (installed)
    {
      g_ptr_array_add (args, g_strdup ("update"));
      g_ptr_array_add (args, g_strdup ("--subpath="));
    }
  else
    {
      g_ptr_array_add (args, g_strdup ("install"));
      g_ptr_array_add (args, g_strdup (remote));
    }

  g_ptr_array_add (args, g_strdup ("-y"));

  g_ptr_array_add (args, g_strdup (ref));
  g_ptr_array_add (args, NULL);

  if (!builder_maybe_host_spawnv (NULL, NULL, 0, error, (const char * const *)args->pdata))
    return FALSE;

  return TRUE;
}

static gboolean
builder_manifest_install_extension_deps (BuilderManifest *self,
                                         BuilderContext  *context,
                                         const char *runtime,
                                         char **runtime_extensions,
                                         const char *remote,
                                         gboolean opt_user,
                                         const char *opt_installation,
                                         gboolean opt_yes,
                                         GError **error)
{
  g_autofree char *runtime_ref = flatpak_build_runtime_ref (runtime, builder_manifest_get_runtime_version (self),
                                                            builder_context_get_arch (context));
  g_autofree char *metadata = NULL;
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  int i;

  if (runtime_extensions == NULL)
    return TRUE;

  metadata = flatpak_info (opt_user, opt_installation, "--show-metadata", runtime_ref, NULL);
  if (metadata == NULL)
    return FALSE;

  if (!g_key_file_load_from_data (keyfile,
                                  metadata, -1,
                                  0,
                                  error))
    return FALSE;

  for (i = 0; runtime_extensions != NULL && runtime_extensions[i] != NULL; i++)
    {
      g_autofree char *extension_group = g_strdup_printf ("Extension %s", runtime_extensions[i]);
      g_autofree char *extension_version = NULL;

      if (!g_key_file_has_group (keyfile, extension_group))
        {
          g_autofree char *base = g_strdup (runtime_extensions[i]);
          char *dot = strrchr (base, '.');
          if (dot != NULL)
            *dot = 0;

          g_free (extension_group);
          extension_group = g_strdup_printf ("Extension %s", base);
          if (!g_key_file_has_group (keyfile, extension_group))
            return flatpak_fail (error, "Unknown extension '%s' in runtime\n", runtime_extensions[i]);
        }

      extension_version = g_key_file_get_string (keyfile, extension_group, "version", NULL);
      if (extension_version == NULL)
        extension_version = g_strdup (builder_manifest_get_runtime_version (self));

      g_print ("Dependency Extension: %s %s\n", runtime_extensions[i], extension_version);
      if (!builder_manifest_install_dep (self, context, remote, opt_user, opt_installation,
                                         runtime_extensions[i], extension_version,
                                         opt_yes,
                                         error))
        return FALSE;
    }

  return TRUE;
}

gboolean
builder_manifest_install_deps (BuilderManifest *self,
                               BuilderContext  *context,
                               const char *remote,
                               gboolean opt_user,
                               const char *opt_installation,
                               gboolean opt_yes,
                               GError **error)
{
  GList *l;

  /* Sdk */
  g_print ("Dependency Sdk: %s %s\n", self->sdk, builder_manifest_get_runtime_version (self));
  if (!builder_manifest_install_dep (self, context, remote, opt_user, opt_installation,
                                     self->sdk, builder_manifest_get_runtime_version (self),
                                     opt_yes,
                                     error))
    return FALSE;

  /* Runtime */
  g_print ("Dependency Runtime: %s %s\n", self->runtime, builder_manifest_get_runtime_version (self));
  if (!builder_manifest_install_dep (self, context, remote, opt_user, opt_installation,
                                     self->runtime, builder_manifest_get_runtime_version (self),
                                     opt_yes,
                                     error))
    return FALSE;

  if (self->base)
    {
      g_print ("Dependency Base: %s %s\n", self->base, builder_manifest_get_base_version (self));
      if (!builder_manifest_install_dep (self, context, remote, opt_user, opt_installation,
                                         self->base, builder_manifest_get_base_version (self),
                                         opt_yes,
                                         error))
        return FALSE;
    }

  if (!builder_manifest_install_extension_deps (self, context,
                                                self->sdk, self->sdk_extensions,
                                                remote,opt_user, opt_installation,
                                                opt_yes,
                                                error))
    return FALSE;

  if (!builder_manifest_install_extension_deps (self, context,
                                                self->runtime, self->platform_extensions,
                                                remote, opt_user, opt_installation,
                                                opt_yes,
                                                error))
    return FALSE;

  for (l = self->add_build_extensions; l != NULL; l = l->next)
    {
      BuilderExtension *extension = l->data;
      const char *name = builder_extension_get_name (extension);
      const char *version = builder_extension_get_version (extension);

      if (name == NULL || version == NULL)
        continue;

      g_print ("Dependency Extension: %s %s\n", name, version);
      if (!builder_manifest_install_dep (self, context, remote, opt_user, opt_installation,
                                         name, version,
                                         opt_yes,
                                         error))
        return FALSE;
    }

  return TRUE;
}

gboolean
builder_manifest_run (BuilderManifest *self,
                      BuilderContext  *context,
                      FlatpakContext  *arg_context,
                      char           **argv,
                      int              argc,
                      gboolean         log_session_bus,
                      gboolean         log_system_bus,
                      GError         **error)
{
  g_autoptr(GPtrArray) args = NULL;
  g_autofree char *commandline = NULL;
  g_autofree char *build_dir_path = NULL;
  g_autofree char *ccache_dir_path = NULL;
  g_auto(GStrv) env = NULL;
  g_auto(GStrv) build_args = NULL;
  int i;

  if (!builder_context_enable_rofiles (context, error))
    return FALSE;

  if (!flatpak_mkdir_p (builder_context_get_build_dir (context),
                        NULL, error))
    return FALSE;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build"));
  g_ptr_array_add (args, g_strdup ("--with-appdir"));

  build_dir_path = g_file_get_path (builder_context_get_build_dir (context));
  /* We're not sure what we're building here, so lets set both the /run/build and /run/build-runtime dirs to the build dirs */
  g_ptr_array_add (args, g_strdup_printf ("--bind-mount=/run/build=%s", build_dir_path));
  g_ptr_array_add (args, g_strdup_printf ("--bind-mount=/run/build-runtime=%s", build_dir_path));

  if (g_file_query_exists (builder_context_get_ccache_dir (context), NULL))
    {
      ccache_dir_path = g_file_get_path (builder_context_get_ccache_dir (context));
      g_ptr_array_add (args, g_strdup_printf ("--bind-mount=/run/ccache=%s", ccache_dir_path));
    }

  build_args = builder_options_get_build_args (self->build_options, self, context, error);
  if (build_args == NULL)
    return FALSE;

  for (i = 0; build_args[i] != NULL; i++)
    g_ptr_array_add (args, g_strdup (build_args[i]));

  env = builder_options_get_env (self->build_options, self, context);
  if (env)
    {
      for (i = 0; env[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup_printf ("--env=%s", env[i]));
    }

  /* Just add something so that we get the default rules (own our own id) */
  g_ptr_array_add (args, g_strdup ("--talk-name=org.freedesktop.DBus"));

  if (log_session_bus)
    g_ptr_array_add (args, g_strdup ("--log-session-bus"));

  if (log_system_bus)
    g_ptr_array_add (args, g_strdup ("--log-system-bus"));

  /* Inherit all finish args except --filesystem and some that
   * build doesn't understand so the command gets the same access
   * as the final app
   */
  if (self->finish_args)
    {
      for (i = 0; self->finish_args[i] != NULL; i++)
        {
          const char *arg = self->finish_args[i];
          if (!g_str_has_prefix (arg, "--filesystem") &&
              !g_str_has_prefix (arg, "--extension") &&
              !g_str_has_prefix (arg, "--sdk") &&
              !g_str_has_prefix (arg, "--runtime") &&
              !g_str_has_prefix (arg, "--command") &&
              !g_str_has_prefix (arg, "--extra-data") &&
              !g_str_has_prefix (arg, "--require-version"))
            g_ptr_array_add (args, g_strdup (arg));
        }
    }

  flatpak_context_to_args (arg_context, args);

  g_ptr_array_add (args, g_file_get_path (builder_context_get_app_dir (context)));

  for (i = 0; i < argc; i++)
    g_ptr_array_add (args, g_strdup (argv[i]));
  g_ptr_array_add (args, NULL);

  commandline = flatpak_quote_argv ((const char **) args->pdata);
  g_debug ("Running '%s'", commandline);


  if (flatpak_is_in_sandbox ())
    {
      if (builder_host_spawnv (NULL, NULL, G_SUBPROCESS_FLAGS_STDIN_INHERIT, NULL, (const gchar * const  *)args->pdata))
        exit (1);
      else
        exit (0);
    }
  else
    {
      if (execvp ((char *) args->pdata[0], (char **) args->pdata) == -1)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to start flatpak build");
          return FALSE;
        }
    }

  /* Not reached */
  return TRUE;
}

char **
builder_manifest_get_exclude_dirs (BuilderManifest *self)
{
  g_autoptr(GPtrArray) dirs = NULL;
  GList *l;

  dirs = g_ptr_array_new ();

  for (l = self->add_extensions; l != NULL; l = l->next)
    {
      BuilderExtension *e = l->data;

      if (builder_extension_is_bundled (e))
        g_ptr_array_add (dirs, g_strdup (builder_extension_get_directory (e)));
    }
  g_ptr_array_add (dirs, NULL);

  return (char **)g_ptr_array_free (g_steal_pointer (&dirs), FALSE);
}
