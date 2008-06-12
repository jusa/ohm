/**
 * @file profile.c
 * @brief OHM Profile plugin 
 * @author ismo.h.puustinen@nokia.com
 *
 * Copyright (C) 2008, Nokia. All rights reserved.
 */

#include "profile.h"

profile_plugin *profile_plugin_p;

static void
plugin_init(OhmPlugin * plugin)
{
    g_print("> Profile plugin init\n");
    /* should we ref the connection? */
    profile_plugin_p = init_profile();
    g_print("< Profile plugin init\n");
    return;
}

static void
plugin_exit(OhmPlugin * plugin)
{
    if (profile_plugin_p) {
        deinit_profile(profile_plugin_p);
    }
    g_free(profile_plugin_p);
    return;
}

OHM_PLUGIN_DESCRIPTION("profile",
        "0.0.1",
        "ismo.h.puustinen@nokia.com",
        OHM_LICENSE_NON_FREE, plugin_init, plugin_exit,
        NULL);


static gboolean profile_create_fact(const char *profile, profileval_t *values) {

    OhmFactStore *fs = ohm_fact_store_get_fact_store();
    GSList *list = NULL;
    OhmFact *fact = NULL;
    GValue *gval = NULL;

    if (!profile)
        return FALSE;

    /* get the previous fact with the profile name */

    list = ohm_fact_store_get_facts_by_name(fs, FACTSTORE_PROFILE);

    if (g_slist_length(list) > 1) {
        g_print("Error: multiple profile facts\n");
        return FALSE;
    }
    else if (g_slist_length(list) == 1) {
        fact = list->data;
        if (fact) {
            /* TODO: if it exists, empty it */
        }

    }
    else {
        /* no previous fact */
        fact = ohm_fact_new(FACTSTORE_PROFILE);
    }

    /* fill the fact with the profile name and the values */

    g_print("setting key %s with value %s\n", PROFILE_NAME_KEY, profile);
    gval = ohm_str_value(profile);
    ohm_fact_set(fact, PROFILE_NAME_KEY, gval);

    while (values->pv_key) {
        if (values->pv_val) {
            g_print("setting key %s with value %s\n", values->pv_key, values->pv_val);
            gval = ohm_str_value(values->pv_val);
            ohm_fact_set(fact, values->pv_key, gval);
        }
        values++;
    }

    /* put the fact in the factstore */

    return ohm_fact_store_insert(fs, fact);
}

static void profile_value_change(const char *profile, const char *key, const char *val, const char *type)
{
    /* TODO */

    /* get the previous fact with the profile name */

    /* get the previous value from the fact */

    /* see that the type matches */

    /* change the value */

    return;
}

static void profile_name_change(const char *profile)
{
    /* get values for the new profile */

    profileval_t *values = NULL;

    if (!profile)
        return;

    profile_get_values(profile);

    /* empty 'values' means that the profile is empty */

    /* change profile data */

    profile_create_fact(profile, values);

    profile_free_values(values);
}


profile_plugin * init_profile()
{
    profile_plugin *plugin = g_new0(profile_plugin, 1);
    char *profile = NULL;

    if (!plugin) {
        return NULL;
    }

    /* get current profile */

    profile = profile_get_profile();

    if (!profile) {
        g_free(plugin);
        return NULL;
    }

    /* subscribe to the profile change notification */
    
    profile_track_set_profile_cb(profile_name_change);

    /* subscribe to the value change in profile notification */
    
    profile_track_set_active_cb(profile_value_change);

    if (profile_tracker_init() < 0) {
        g_free(plugin);
        plugin = NULL;
        goto end;
    }
    
    /* get the initial values */
    
    profileval_t *values = profile_get_values(profile);

    if (!profile_create_fact(profile, values)) {
        g_free(plugin);
        plugin = NULL;
        goto end;
    }

end:

    free(profile);
    profile_free_values(values);

    return plugin;
}

void deinit_profile(profile_plugin *plugin)
{
    /* unregister to the notifications */
    profile_tracker_quit();
}


/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim:set expandtab shiftwidth=4:
 */