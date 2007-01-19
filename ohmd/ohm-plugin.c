/*
 * Copyright (C) 2007 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* Provides the bridge between the .so plugin and intraprocess communication */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include <glib/gi18n.h>
#include <gmodule.h>

#include "ohm-debug.h"
#include "ohm-plugin.h"
#include "ohm-conf.h"
#include "ohm-marshal.h"

#define OHM_PLUGIN_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), OHM_TYPE_PLUGIN, OhmPluginPrivate))

struct OhmPluginPrivate
{
	OhmConf			*conf;
	OhmPluginInfo		*info;
	GModule			*handle;
	gchar			*name;
};

enum {
	ADD_INTERESTED,
	ADD_REQUIRE,
	ADD_SUGGEST,
	ADD_PREVENT,
	LAST_SIGNAL
};

static guint	     signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE (OhmPlugin, ohm_plugin, G_TYPE_OBJECT)

/**
 * ohm_plugin_get_key:
 **/
gboolean
ohm_plugin_require (OhmPlugin   *plugin,
		    const gchar *name)
{
	ohm_debug ("emitting add-require '%s'", name);
	g_signal_emit (plugin, signals[ADD_REQUIRE], 0, name);
	return TRUE;
}

/**
 * ohm_plugin_add_notify_key:
 **/
gboolean
ohm_plugin_suggest (OhmPlugin   *plugin,
		    const gchar *name)
{
	ohm_debug ("emitting add-suggest '%s'", name);
	g_signal_emit (plugin, signals[ADD_SUGGEST], 0, name);
	return TRUE;
}

/**
 * ohm_plugin_set_key:
 *
 **/
gboolean
ohm_plugin_prevent (OhmPlugin   *plugin,
		    const gchar *name)
{
	ohm_debug ("emitting add-prevent '%s'", name);
	g_signal_emit (plugin, signals[ADD_PREVENT], 0, name);
	return TRUE;
}

gboolean
ohm_plugin_load (OhmPlugin *plugin, const gchar *name)
{
	gchar *path;
	GModule *handle;

	OhmPluginInfo * (*ohm_init_plugin) (OhmPlugin *);

	g_return_val_if_fail (name != NULL, FALSE);

	path = g_build_filename (LIBDIR, name, NULL);
	handle = g_module_open (path, 0);
	if (!handle) {
		ohm_debug ("opening module %s failed : %s", name, g_module_error ());
		g_free (path);
		return FALSE;
	}
	g_free (path);

	if (!g_module_symbol (handle, "ohm_init_plugin", (gpointer) &ohm_init_plugin)) {
		g_module_close (handle);
		g_error ("could not find init function in plugin");
	}

	plugin->priv->handle = handle;
	plugin->priv->name = g_strdup (name);
	plugin->priv->info = ohm_init_plugin (plugin);

	if (plugin->priv->info->load != NULL) {
		plugin->priv->info->load (plugin);
	}

	return TRUE;
}

const gchar *
ohm_plugin_get_name (OhmPlugin * plugin)
{
	g_return_val_if_fail (plugin != NULL, NULL);

	return plugin->priv->name;
}

const gchar *
ohm_plugin_get_version (OhmPlugin * plugin)
{
	g_return_val_if_fail (plugin != NULL, NULL);

	return plugin->priv->info->version;
}

const gchar *
ohm_plugin_get_author (OhmPlugin * plugin)
{
	g_return_val_if_fail (plugin != NULL, NULL);

	return plugin->priv->info->author;
}

G_MODULE_EXPORT gboolean
ohm_plugin_conf_provide (OhmPlugin *plugin,
			 const gchar *name)
{
	ohm_debug ("%s provides %s", plugin->priv->name, name);
	/* TODO; check that nothing else provides this key */
	return FALSE;
}

G_MODULE_EXPORT gboolean
ohm_plugin_conf_get_key (OhmPlugin   *plugin,
			 const gchar *key,
			 int         *value)
{
	return ohm_conf_get_key (plugin->priv->conf, key, value, NULL);
}

G_MODULE_EXPORT gboolean
ohm_plugin_conf_set_key (OhmPlugin   *plugin,
			 const gchar *key,
			 int          value)
{
	return ohm_conf_set_key_internal (plugin->priv->conf, key, value, TRUE, NULL);
}

G_MODULE_EXPORT gboolean
ohm_plugin_conf_notify (OhmPlugin   *plugin,
			int          id,
			int          value)
{
	plugin->priv->info->conf_notify (plugin, id, value);
	return TRUE;
}

G_MODULE_EXPORT gboolean
ohm_plugin_conf_interested (OhmPlugin   *plugin,
			    const gchar	*key,
			    gint         id)
{
	ohm_debug ("%s provides wants notification of %s on signal %i", plugin->priv->name, key, id);
	g_signal_emit (plugin, signals[ADD_INTERESTED], 0, key, id);
	return TRUE;
}

/**
 * ohm_plugin_finalize:
 **/
static void
ohm_plugin_finalize (GObject *object)
{
	OhmPlugin *plugin;
	g_return_if_fail (object != NULL);
	g_return_if_fail (OHM_IS_PLUGIN (object));
	plugin = OHM_PLUGIN (object);

	g_object_unref (plugin->priv->conf);

	if (plugin->priv->info != NULL) {
		if (plugin->priv->info->unload != NULL) {
			plugin->priv->info->unload (plugin);
		}
	}
	if (plugin->priv->handle != NULL)
		g_module_close (plugin->priv->handle);

	if (plugin->priv->name != NULL)
		g_free (plugin->priv->name);

	g_return_if_fail (plugin->priv != NULL);
	G_OBJECT_CLASS (ohm_plugin_parent_class)->finalize (object);
}

/**
 * ohm_plugin_class_init:
 **/
static void
ohm_plugin_class_init (OhmPluginClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize	   = ohm_plugin_finalize;

	signals[ADD_INTERESTED] =
		g_signal_new ("add-interested",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (OhmPluginClass, add_interested),
			      NULL, NULL,
			      ohm_marshal_VOID__STRING_INT,
			      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

	signals[ADD_REQUIRE] =
		g_signal_new ("add-require",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (OhmPluginClass, add_require),
			      NULL, NULL,
			      ohm_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

	signals[ADD_SUGGEST] =
		g_signal_new ("add-suggest",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (OhmPluginClass, add_suggest),
			      NULL, NULL,
			      ohm_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

	signals[ADD_PREVENT] =
		g_signal_new ("add-prevent",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (OhmPluginClass, add_prevent),
			      NULL, NULL,
			      ohm_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

	g_type_class_add_private (klass, sizeof (OhmPluginPrivate));
}

/**
 * ohm_plugin_init:
 **/
static void
ohm_plugin_init (OhmPlugin *plugin)
{
	plugin->priv = OHM_PLUGIN_GET_PRIVATE (plugin);

	plugin->priv->conf = ohm_conf_new ();
}

/**
 * ohm_plugin_new:
 **/
OhmPlugin *
ohm_plugin_new (void)
{
	OhmPlugin *plugin;
	plugin = g_object_new (OHM_TYPE_PLUGIN, NULL);
	return OHM_PLUGIN (plugin);
}
