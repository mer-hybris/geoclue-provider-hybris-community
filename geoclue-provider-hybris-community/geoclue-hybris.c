/*
 * Geoclue-provider-hybris
 * geoclue-hybris.c - A hybris-based GPS-provider
 *
 * Author: Matti Lehtimäki <matti.lehtimaki@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include <config.h>

#include <string.h>

#include <android-config.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <getopt.h>
#include <math.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib-bindings.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <syslog.h>

#include <hardware/gps.h>

#include <geoclue/gc-provider.h>
#include <geoclue/geoclue-error.h>
#include <geoclue/gc-iface-geoclue.h>
#include <geoclue/gc-iface-position.h>
#include <geoclue/gc-iface-satellite.h>
#include <geoclue/gc-iface-velocity.h>

#define GEOCLUE_TYPE_HYBRIS (geoclue_hybris_get_type ())
#define GEOCLUE_HYBRIS(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GEOCLUE_TYPE_HYBRIS, GeoclueHybris))

typedef struct {
    GcProvider parent;
    GMainLoop *loop;

    char *owner;
    int last_timestamp;
    double last_altitude;
    double last_bearing;
    double last_latitude;
    double last_longitude;
    double last_speed;
    int last_satellite_used;
    int last_satellite_visible;
    GArray *last_used_prn;
    GPtrArray *last_sat_info;
    GeoclueAccuracy *last_accuracy;
    GeocluePositionFields last_pos_fields;
    GeoclueVelocityFields last_velo_fields;
    GeoclueStatus last_status;
    GHashTable *connections;
    DBusConnection *conn;
} GeoclueHybris;

typedef struct {
    GcProviderClass parent_class;
} GeoclueHybrisClass;

static void geoclue_hybris_init (GeoclueHybris *obj);
static void geoclue_hybris_geoclue_init (GcIfaceGeoclueClass *iface);
static void geoclue_hybris_position_init (GcIfacePositionClass *iface);
static void geoclue_hybris_satellite_init (GcIfaceSatelliteClass *iface);
static void geoclue_hybris_velocity_init (GcIfaceVelocityClass *iface);
static void geoclue_hybris_update_position (GeoclueHybris *hybris, GpsLocation* location);
static void geoclue_hybris_update_velocity (GeoclueHybris *hybris, GpsLocation* location);
static void geoclue_hybris_update_satellites (GeoclueHybris *hybris, GpsSvStatus* sv_info);
static void geoclue_hybris_update_status (GeoclueHybris *hybris, GeoclueStatus status);

G_DEFINE_TYPE_WITH_CODE (GeoclueHybris, geoclue_hybris, GC_TYPE_PROVIDER,
                         G_IMPLEMENT_INTERFACE (GC_TYPE_IFACE_GEOCLUE,
                                                geoclue_hybris_geoclue_init)
                         G_IMPLEMENT_INTERFACE (GC_TYPE_IFACE_POSITION,
                                                geoclue_hybris_position_init)
                         G_IMPLEMENT_INTERFACE (GC_TYPE_IFACE_SATELLITE,
                                                geoclue_hybris_satellite_init)
                         G_IMPLEMENT_INTERFACE (GC_TYPE_IFACE_VELOCITY,
                                                geoclue_hybris_velocity_init))

GeoclueHybris *hybris = NULL;

/* Hybris GPS */

const GpsInterface* gps = NULL;

static const GpsInterface*
get_gps_interface()
{
    syslog(LOG_INFO, "get_gps_interface start");
    int error;
    hw_module_t* module;
    const GpsInterface* interface = NULL;
    struct gps_device_t *device;

    error = hw_get_module(GPS_HARDWARE_MODULE_ID, (hw_module_t const**)&module);

    if (!error)
    {
        syslog(LOG_INFO, "GPS device info\n id = %s\n name = %s\n author = %s\n",
               module->id, module->name, module->author);
        error = module->methods->open(module, GPS_HARDWARE_MODULE_ID,
                                      (struct hw_device_t **) &device);

        if (!error)
        {
            interface = device->get_gps_interface(device);
        }
        else
        {
            syslog(LOG_ERR, "Unable to get GPS interface\n");
        }
    }
    else
    {
        syslog(LOG_ERR, "GPS interface not found, terminating\n");
        exit(1);
    }
    syslog(LOG_INFO, "get_gps_interface end");

    return interface;
}

static void
location_callback(GpsLocation* location)
{
    geoclue_hybris_update_status (hybris, GEOCLUE_STATUS_AVAILABLE);
    geoclue_hybris_update_position (hybris, location);
    geoclue_hybris_update_velocity (hybris, location);
}

static void
status_callback(GpsStatus* status)
{
    switch (status->status)
    {
        case GPS_STATUS_NONE:
        geoclue_hybris_update_status (hybris, GEOCLUE_STATUS_UNAVAILABLE);
        break;
        case GPS_STATUS_SESSION_BEGIN:
        syslog(LOG_INFO, "GPS session started");
        geoclue_hybris_update_status (hybris, GEOCLUE_STATUS_ACQUIRING);
        break;
        case GPS_STATUS_SESSION_END:
        syslog(LOG_INFO, "GPS session stopped");
        geoclue_hybris_update_status (hybris, GEOCLUE_STATUS_UNAVAILABLE);
        break;
        case GPS_STATUS_ENGINE_ON:
        geoclue_hybris_update_status (hybris, GEOCLUE_STATUS_ACQUIRING);
        break;
        case GPS_STATUS_ENGINE_OFF:
        geoclue_hybris_update_status (hybris, GEOCLUE_STATUS_UNAVAILABLE);
        break;
        default:
        break;
    }
}

static void
sv_status_callback(GpsSvStatus* sv_info)
{
    geoclue_hybris_update_satellites (hybris, sv_info);
}

static void
nmea_callback(GpsUtcTime timestamp, const char* nmea, int length)
{
    /* do nothing */
}

static void
set_capabilities_callback(uint32_t capabilities)
{
    syslog(LOG_INFO, "GPS hal supported capabilities:");
    int bitmask = capabilities;
    int mask = 1;
    while(bitmask)
    {
        switch (capabilities & mask)
        {
            case GPS_CAPABILITY_SCHEDULING:
            syslog(LOG_INFO, "Scheduling");
            break;
            /** GPS supports MS-Based AGPS mode */
            case GPS_CAPABILITY_MSB:
            syslog(LOG_INFO, "MS-Based AGPS");
            break;
            /** GPS supports MS-Assisted AGPS mode */
            case GPS_CAPABILITY_MSA:
            syslog(LOG_INFO, "MS-Assisted AGPS");
            break;
            /** GPS supports single-shot fixes */
            case GPS_CAPABILITY_SINGLE_SHOT:
            syslog(LOG_INFO, "Single-shot fixes");
            break;
            /** GPS supports on demand time injection */
            case GPS_CAPABILITY_ON_DEMAND_TIME:
            syslog(LOG_INFO, "On demand time injection");
            break;
            /** GPS supports Geofencing  */
            case GPS_CAPABILITY_GEOFENCING:
            syslog(LOG_INFO, "Geofencing");
            break;
            default:
            break;
        }
        bitmask &= ~mask;
        mask <<= 1;
    }
}

static void
acquire_wakelock_callback()
{
    /* do nothing */
}

static void
release_wakelock_callback()
{
    /* do nothing */
}

struct ThreadWrapperContext {
    void (*func)(void *);
    void *user_data;
};

static void *
thread_wrapper_context_main_func(void *user_data)
{
  syslog(LOG_INFO, "thread_wrapper_context_main_func start");
  struct ThreadWrapperContext *ctx = (struct ThreadWrapperContext *)user_data;

  ctx->func(ctx->user_data);

  free(ctx);
  syslog(LOG_INFO, "thread_wrapper_context_main_func end");
  return NULL;
}

static pthread_t
create_thread_callback(const char* name, void (*start)(void *), void* arg)
{
  syslog(LOG_INFO, "create_thread_callback start");
  pthread_t thread_id;
  int error = 0;

  /* Wrap thread function, so we can return void * to pthread and log start/end of thread */
  struct ThreadWrapperContext *ctx = calloc(1, sizeof(struct ThreadWrapperContext));
  ctx->func = start;
  ctx->user_data = arg;

  /* Do not use a pthread_attr_t (we'd have to take care of bionic/glibc differences) */
  error = pthread_create(&thread_id, NULL, thread_wrapper_context_main_func, ctx);

  if(error != 0) {
    syslog(LOG_INFO, "create_thread_callback end 1");
    return 0;
  }

  syslog(LOG_INFO, "create_thread_callback end");
  return thread_id;
}

/* Hybris GPS callbacks */
GpsCallbacks callbacks = {
  sizeof(GpsCallbacks),
  location_callback,
  status_callback,
  sv_status_callback,
  nmea_callback,
  set_capabilities_callback,
  acquire_wakelock_callback,
  release_wakelock_callback,
  create_thread_callback,
};

/* Geoclue interfaces implementations */

static gboolean
equal_or_nan (double a, double b)
{
    if (isnan (a) && isnan (b)) {
        return TRUE;
    }
    return a == b;
}

/* General  */

static void
geoclue_hybris_update_status (GeoclueHybris *hybris, GeoclueStatus status)
{
    syslog(LOG_INFO, "geoclue_hybris_update_status start");
    if (status != hybris->last_status) {
        switch (status)
        {
            case GEOCLUE_STATUS_ACQUIRING:
            syslog(LOG_INFO, "GPS acquiring location");
            break;
            case GEOCLUE_STATUS_AVAILABLE:
            syslog(LOG_INFO, "GPS location acquired");
            break;
            case GEOCLUE_STATUS_UNAVAILABLE:
            syslog(LOG_INFO, "GPS location unavailable");
            break;
            case GEOCLUE_STATUS_ERROR:
            syslog(LOG_INFO, "GPS error");
            break;
            default:
            break;
        }
        hybris->last_status = status;
        /* make position and velocity invalid if no fix */
        if (status != GEOCLUE_STATUS_AVAILABLE) {
            hybris->last_pos_fields = GEOCLUE_POSITION_FIELDS_NONE;
            hybris->last_velo_fields = GEOCLUE_VELOCITY_FIELDS_NONE;
        }
        gc_iface_geoclue_emit_status_changed (GC_IFACE_GEOCLUE (hybris),
                                              status);
    }
    syslog(LOG_INFO, "geoclue_hybris_update_status end");
}

static gboolean
geoclue_hybris_get_status (GcIfaceGeoclue *iface,
                          GeoclueStatus  *status,
                          GError        **error)
{
    syslog(LOG_INFO, "geoclue_hybris_get_status start");
    GeoclueHybris *hybris = GEOCLUE_HYBRIS (iface);

    *status = hybris->last_status;

    syslog(LOG_INFO, "geoclue_hybris_get_status end");
    return TRUE;
}

static gboolean
set_options (GcIfaceGeoclue *gc,
             GHashTable     *options,
             GError        **error)
{
    return TRUE;
}

/* Deinitialization */

static void
shutdown (GcProvider *provider)
{
    syslog(LOG_INFO, "shutdown");
    GeoclueHybris *hybris = GEOCLUE_HYBRIS (provider);

    g_main_loop_quit (hybris->loop);
}

static void
geoclue_hybris_dispose (GObject *obj)
{
    syslog(LOG_INFO, "geoclue_hybris_dispose");
    ((GObjectClass *) geoclue_hybris_parent_class)->dispose (obj);
}

static void
finalize (GObject *obj)
{
    syslog(LOG_INFO, "geoclue_hybris_finalize start");
    GeoclueHybris *hybris = GEOCLUE_HYBRIS (obj);
    int i = 0;

    if (gps) {
        gps->stop();
        gps->cleanup();
        gps = NULL;
    }

    if (hybris->last_used_prn->len) {
        g_array_remove_range (hybris->last_used_prn, 0, hybris->last_used_prn->len);
    }
    if (hybris->last_sat_info->len) {
        for (i = 0; i < hybris->last_sat_info->len; i++) {
            g_value_array_free(g_ptr_array_index(hybris->last_sat_info, i));
        }
        g_ptr_array_remove_range (hybris->last_sat_info, 0, hybris->last_sat_info->len);
    }
    g_array_free (hybris->last_used_prn, TRUE);
    hybris->last_used_prn = NULL;
    g_ptr_array_free (hybris->last_sat_info, TRUE);
    hybris->last_sat_info = NULL;
    geoclue_accuracy_free (hybris->last_accuracy);
    hybris->last_accuracy = NULL;
    g_hash_table_destroy (hybris->connections);
    hybris->connections = NULL;
    free(hybris->owner);
    hybris->owner = NULL;

    syslog(LOG_INFO, "geoclue_hybris_finalize end");
    ((GObjectClass *) geoclue_hybris_parent_class)->finalize (obj);
}

/* Position interface */

static void
geoclue_hybris_update_position (GeoclueHybris *hybris, GpsLocation* location)
{
    syslog(LOG_INFO, "geoclue_hybris_update_position start");
    if (!hybris->last_accuracy) {
        syslog(LOG_INFO, "geoclue_hybris_update_position end 1");
        return;
    }
    if (equal_or_nan (location->latitude, hybris->last_latitude) &&
        equal_or_nan (location->longitude, hybris->last_longitude) &&
        equal_or_nan (location->altitude, hybris->last_altitude)) {
        /* position has not changed */
        syslog(LOG_INFO, "geoclue_hybris_update_position end 2");
        return;
    }

    hybris->last_latitude = location->latitude;
    hybris->last_longitude = location->longitude;
    hybris->last_altitude = location->altitude;
    hybris->last_timestamp = (int)(location->timestamp/1000+0.5);
    hybris->last_pos_fields = GEOCLUE_POSITION_FIELDS_LATITUDE | GEOCLUE_POSITION_FIELDS_LONGITUDE | GEOCLUE_POSITION_FIELDS_ALTITUDE;
    geoclue_accuracy_set_details (hybris->last_accuracy,
                                  GEOCLUE_ACCURACY_LEVEL_DETAILED,
                                  location->accuracy, location->accuracy);

    hybris->last_pos_fields = GEOCLUE_POSITION_FIELDS_NONE;
    hybris->last_pos_fields |= (isnan (location->latitude)) ?
                             0 : GEOCLUE_POSITION_FIELDS_LATITUDE;
    hybris->last_pos_fields |= (isnan (location->longitude)) ?
                             0 : GEOCLUE_POSITION_FIELDS_LONGITUDE;
    hybris->last_pos_fields |= (isnan (location->altitude)) ?
                             0 : GEOCLUE_POSITION_FIELDS_ALTITUDE;

    gc_iface_position_emit_position_changed
        (GC_IFACE_POSITION (hybris),
         GEOCLUE_POSITION_FIELDS_LATITUDE | GEOCLUE_POSITION_FIELDS_LONGITUDE | GEOCLUE_POSITION_FIELDS_ALTITUDE,
         (int)(location->timestamp/1000+0.5),
         location->latitude, location->longitude, location->altitude,
         hybris->last_accuracy);
    syslog(LOG_INFO, "geoclue_hybris_update_position end");
}

static gboolean
get_position (GcIfacePosition *gc,
              GeocluePositionFields *fields,
              int                   *timestamp,
              double                *latitude,
              double                *longitude,
              double                *altitude,
              GeoclueAccuracy      **accuracy,
              GError               **error)
{
    syslog(LOG_INFO, "get_position start");
    if (!hybris->last_accuracy) {
        return FALSE;
    }
    *timestamp = (int)(hybris->last_timestamp+0.5);
    *fields =  hybris->last_pos_fields;
    *accuracy = geoclue_accuracy_copy (hybris->last_accuracy);
    *latitude = hybris->last_latitude;
    *longitude = hybris->last_longitude;
    *altitude = hybris->last_altitude;

    syslog(LOG_INFO, "get_position end");
    return TRUE;
}

/* Velocity interface */

static void
geoclue_hybris_update_velocity (GeoclueHybris *hybris, GpsLocation* location)
{
    syslog(LOG_INFO, "geoclue_hybris_update_velocity start");
    if (equal_or_nan (location->speed, hybris->last_speed) &&
        equal_or_nan (location->bearing, hybris->last_bearing)) {
        /* velocity has not changed */
        syslog(LOG_INFO, "geoclue_hybris_update_velocity end 1");
        return;
    }

    hybris->last_speed = location->speed;
    hybris->last_bearing = location->bearing;

    hybris->last_velo_fields = GEOCLUE_VELOCITY_FIELDS_NONE;
    hybris->last_velo_fields |= (isnan (hybris->last_bearing)) ?
        0 : GEOCLUE_VELOCITY_FIELDS_DIRECTION;
    hybris->last_velo_fields |= (isnan (hybris->last_speed)) ?
        0 : GEOCLUE_VELOCITY_FIELDS_SPEED;

    gc_iface_velocity_emit_velocity_changed
        (GC_IFACE_VELOCITY (hybris), hybris->last_velo_fields,
         (int)(hybris->last_timestamp+0.5),
         hybris->last_speed, hybris->last_bearing, 0);
    syslog(LOG_INFO, "geoclue_hybris_update_velocity end");
}

static gboolean
get_velocity (GcIfaceVelocity       *gc,
              GeoclueVelocityFields *fields,
              int                   *timestamp,
              double                *speed,
              double                *direction,
              double                *climb,
              GError               **error)
{
    syslog(LOG_INFO, "get_velocity start");
    GeoclueHybris *hybris = GEOCLUE_HYBRIS (gc);

    *timestamp = (int)(hybris->last_timestamp+0.5);
    *speed = hybris->last_speed;
    *direction = hybris->last_bearing;
    *climb = 0;
    *fields = hybris->last_velo_fields;

    syslog(LOG_INFO, "get_velocity end");
    return TRUE;
}

/* Satellite interface */

static void
geoclue_hybris_update_satellites (GeoclueHybris *hybris, GpsSvStatus* sv_info)
{
    syslog(LOG_INFO, "geoclue_hybris_update_satellites start");
    int i = 0;
    GValue val = G_VALUE_INIT;
    g_value_init (&val, G_TYPE_INT);

    if (!hybris->last_sat_info || !hybris->last_used_prn) {
        syslog(LOG_INFO, "geoclue_hybris_update_satellites end 1");
        return;
    }

    if (hybris->last_sat_info->len) {
        for (i = 0; i < hybris->last_sat_info->len; i++) {
            g_value_array_free(g_ptr_array_index(hybris->last_sat_info, i));
        }
        g_ptr_array_remove_range (hybris->last_sat_info, 0, hybris->last_sat_info->len);
    }
    if (hybris->last_used_prn->len) {
        g_array_remove_range (hybris->last_used_prn, 0, hybris->last_used_prn->len);
    }

    i = 0;
    for(i=0; i < sv_info->num_svs; i++)
    {
        if (sv_info->used_in_fix_mask & (1 << (sv_info->sv_list[i].prn-1))) {
            g_array_append_val (hybris->last_used_prn, sv_info->sv_list[i].prn);
        }
        GValueArray *sat = g_value_array_new (4);
        g_value_set_int (&val, sv_info->sv_list[i].prn);
        g_value_array_append (sat, &val);
        g_value_set_int (&val, sv_info->sv_list[i].azimuth);
        g_value_array_append (sat, &val);
        g_value_set_int (&val, sv_info->sv_list[i].elevation);
        g_value_array_append (sat, &val);
        g_value_set_int (&val, sv_info->sv_list[i].snr);
        g_value_array_append (sat, &val);
        g_ptr_array_add (hybris->last_sat_info, sat);
    }
    g_value_unset (&val);

    hybris->last_satellite_used = hybris->last_used_prn->len,
    hybris->last_satellite_visible = sv_info->num_svs,

    gc_iface_satellite_emit_satellite_changed (GC_IFACE_SATELLITE(hybris),
        (int)(hybris->last_timestamp+0.5),
        hybris->last_satellite_used,
        hybris->last_satellite_visible,
        hybris->last_used_prn,
        hybris->last_sat_info);
    syslog(LOG_INFO, "geoclue_hybris_update_satellites end");
}

static gboolean
get_satellite (GcIfaceSatellite *gc,
               int              *timestamp,
               int              *satellite_used,
               int              *satellite_visible,
               GArray          **used_prn,
               GPtrArray       **sat_info,
               GError          **error)
{
    syslog(LOG_INFO, "get_satellite start");
    if (!hybris->last_sat_info || !hybris->last_used_prn) {
        syslog(LOG_INFO, "get_satellite end 1");
        return FALSE;
    }
    *timestamp = (int)(hybris->last_timestamp+0.5);
    *satellite_used =  hybris->last_satellite_used;
    *satellite_visible = hybris->last_satellite_visible;
    *used_prn = hybris->last_used_prn;
    *sat_info = hybris->last_sat_info;

    syslog(LOG_INFO, "get_satellite end");
    return TRUE;
}

static gboolean
get_last_satellite (GcIfaceSatellite *gc,
                    int              *timestamp,
                    int              *satellite_used,
                    int              *satellite_visible,
                    GArray          **used_prn,
                    GPtrArray       **sat_info,
                    GError          **error)
{
    syslog(LOG_INFO, "get_last_satellite start");
    if (!hybris->last_sat_info || !hybris->last_used_prn) {
        syslog(LOG_INFO, "get_last_satellite end 1");
        return FALSE;
    }
    *timestamp = (int)(hybris->last_timestamp+0.5);
    *satellite_used =  hybris->last_satellite_used;
    *satellite_visible = hybris->last_satellite_visible;
    *used_prn = hybris->last_used_prn;
    *sat_info = hybris->last_sat_info;

    syslog(LOG_INFO, "get_satellite end");
    return TRUE;
}

/* Geoclue interface */

static gboolean
get_provider_info (GcIfaceGeoclue  *gc,
                   gchar          **name,
                   gchar          **description,
                   GError         **error)
{
    syslog(LOG_INFO, "get_provider_info start");
    if (name) {
        *name = g_strdup ("Hybris");
    }
    if (description) {
        *description = g_strdup ("Hybris GPS provider");
    }
    if (error) {
        *error = NULL;
    }

    syslog(LOG_INFO, "get_provider_info end");
    return TRUE;
}

static void
add_reference (GcIfaceGeoclue        *gc,
               DBusGMethodInvocation *context)
{
    syslog(LOG_INFO, "add_reference start");
    char *sender;
    int *pcount;
    if (!hybris->connections)
        return;

    /* Update the hash of open connections */
    sender = dbus_g_method_get_sender (context);
    pcount = g_hash_table_lookup (hybris->connections, sender);
    if (!pcount) {
        pcount = g_malloc0 (sizeof (int));
        g_hash_table_insert (hybris->connections, sender, pcount);
    }
    else {
        free(sender);
    }
    (*pcount)++;
    if (g_hash_table_size (hybris->connections) == 1 && *pcount == 1) {
        hybris->owner = strdup(sender);
    }
    syslog(LOG_INFO, "add_reference end");
    dbus_g_method_return (context);
}

static void
remove_reference (GcIfaceGeoclue        *gc,
                  DBusGMethodInvocation *context)
{
    syslog(LOG_INFO, "remove_reference start");
    char *sender;
    int *pcount;
    if (!hybris->connections)
        return;

    sender = dbus_g_method_get_sender (context);
    pcount = g_hash_table_lookup (hybris->connections, sender);
    if (!pcount) {
        free (sender);
        return;
    }

    (*pcount)--;
    if (*pcount == 0) {
        g_hash_table_remove (hybris->connections, sender);
    }
    if (g_hash_table_size (hybris->connections) == 0 || strcmp(sender, hybris->owner) == 0) {
        gps->stop();
        g_main_loop_quit (hybris->loop);
    }
    free (sender);
    syslog(LOG_INFO, "remove_reference end");
    dbus_g_method_return (context);
}

static void
process_property_name_value(DBusMessageIter *iter)
{
    const char *property;
    gboolean state = 0;
    DBusMessageIter sub;
    dbus_message_iter_get_basic(iter, &property);
    dbus_message_iter_next (iter);
    if (strcmp(property, "Powered") == 0) {
        if (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(iter, &sub);
        }
        dbus_message_iter_get_basic(&sub, &state);
        syslog(LOG_INFO, "GPS %s from settings", state ? "enabled" : "disabled");
        if (state) {
            gps->start();
        }
        else {
            gps->stop();
        }
    }
}

static void
process_property_message(DBusMessage *msg)
{
    DBusMessageIter main_iter;
    DBusMessageIter iter;
    dbus_message_iter_init (msg, &main_iter);

    if (dbus_message_iter_get_arg_type (&main_iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&main_iter, &iter);
    }
    else {
        dbus_message_iter_init (msg, &iter);
    }
    do {
        if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter dict_iter;
            dbus_message_iter_recurse(&iter, &dict_iter);
            process_property_name_value(&dict_iter);
        }
        else {
            process_property_name_value(&iter);
        }
    }
    while (dbus_message_iter_next(&iter));
}

static DBusHandlerResult
property_changed_signal(DBusConnection *connection,
                        DBusMessage *msg, void *user_data)
{
    syslog(LOG_INFO, "property_changed_signal start");
    if (dbus_message_is_signal(msg, "net.connman.Technology",
                               "PropertyChanged")) {
        process_property_message(msg);
    }
    syslog(LOG_INFO, "property_changed_signal end");
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
get_properties_cb (DBusPendingCall *pc, gpointer data)
{
    syslog(LOG_INFO, "get_properties_cb start");
    DBusMessage *message = dbus_pending_call_steal_reply (pc);

    if (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_ERROR) {
        syslog(LOG_ERR, "No reply from DBus");
        //finalize((GObject *)hybris);
        g_main_loop_quit (hybris->loop);
    }
    else {
        process_property_message(message);
    }

    dbus_message_unref (message);
    dbus_pending_call_unref (pc);
    syslog(LOG_INFO, "get_properties_cb end");
}

/* Initialization */

static void
geoclue_hybris_class_init (GeoclueHybrisClass *klass)
{
    GObjectClass *o_class = (GObjectClass *)klass;
    GcProviderClass *p_class = (GcProviderClass *)klass;

    o_class->finalize = finalize;
    o_class->dispose = geoclue_hybris_dispose;

    p_class->shutdown = shutdown;
    p_class->set_options = set_options;
    p_class->get_status = geoclue_hybris_get_status;
}

static void
geoclue_hybris_init (GeoclueHybris *hybris)
{
    syslog(LOG_INFO, "geoclue_hybris_init start");
    gc_provider_set_details (GC_PROVIDER (hybris),
                            "org.freedesktop.Geoclue.Providers.Hybris",
                            "/org/freedesktop/Geoclue/Providers/Hybris",
                            "Hybris", "Hybris GPS provider");

    struct timeval tv;
    DBusError error;
    DBusMessage *methodcall;
    DBusPendingCall *pending;
    int initok = 0;
    hybris->last_accuracy = geoclue_accuracy_new (GEOCLUE_ACCURACY_LEVEL_NONE, 0, 0);
    hybris->last_latitude = 1.0;
    hybris->last_longitude = 1.0;
    hybris->last_altitude = 1.0;
    hybris->last_speed = 1.0;
    hybris->last_bearing = 1.0;
    hybris->last_timestamp = time(NULL);
    hybris->last_pos_fields = GEOCLUE_POSITION_FIELDS_NONE;
    hybris->last_velo_fields = GEOCLUE_VELOCITY_FIELDS_NONE;
    hybris->connections = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, g_free);

    hybris->last_satellite_used = 0;
    hybris->last_satellite_visible = 0;
    hybris->last_sat_info = g_ptr_array_new ();
    hybris->last_used_prn = g_array_new (FALSE, FALSE, sizeof (gint));

    dbus_error_init(&error);

    hybris->conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);

    if (dbus_error_is_set(&error)) {
        syslog(LOG_ERR, "Cannot get System BUS connection: %s", error.message);
        dbus_error_free(&error);
        g_main_loop_quit (hybris->loop);
    }
    dbus_connection_setup_with_g_main(hybris->conn, NULL);

    char *rule = "type='signal',interface='net.connman.Technology',path='/net/connman/technology/gps',member='PropertyChanged'";
    dbus_bus_add_match(hybris->conn, rule, &error);

    if (dbus_error_is_set(&error)) {
        syslog(LOG_ERR, "Cannot add D-BUS match rule, cause: %s", error.message);
        dbus_error_free(&error);
        g_main_loop_quit (hybris->loop);
    }
    dbus_error_free(&error);

    dbus_connection_add_filter(hybris->conn, property_changed_signal, NULL, NULL);

    gps = get_gps_interface();

    initok = gps->init(&callbacks);

    /* need to be done before starting gps or no info will come out */
    if (!initok)
        gps->set_position_mode(GPS_POSITION_MODE_MS_BASED,
                               GPS_POSITION_RECURRENCE_PERIODIC, 1000, 0, 0);
    else
        gps->set_position_mode(GPS_POSITION_MODE_STANDALONE,
                               GPS_POSITION_RECURRENCE_PERIODIC, 1000, 0, 0);

    /* help gps by injecting time information */
    gettimeofday(&tv, NULL);
    gps->inject_time(tv.tv_sec, tv.tv_sec, 0);

    geoclue_hybris_update_status (hybris, GEOCLUE_STATUS_UNAVAILABLE);

    /* get connman gps properties to check whether gps is enabled */
    methodcall = dbus_message_new_method_call("net.connman",
                                              "/net/connman/technology/gps",
                                              "net.connman.Technology",
                                              "GetProperties");

    if (methodcall == NULL) {
        syslog(LOG_ERR, "Cannot allocate DBus message!\n");
    }
    /* now do a sync call and expect reply using pending call object */
    if (!dbus_connection_send_with_reply(hybris->conn, methodcall, &pending, -1)) {
        syslog(LOG_ERR, "Failed to send DBus message!\n");
    }
    dbus_connection_flush(hybris->conn);
    dbus_message_unref(methodcall);
    methodcall = NULL;

    if (dbus_pending_call_get_completed (pending)) {
        get_properties_cb (pending, NULL);
    }

    if (!dbus_pending_call_set_notify (pending, get_properties_cb, NULL, NULL)) {
        syslog(LOG_ERR, "Out of memory");
    }
    syslog(LOG_INFO, "geoclue_hybris_init end");
}

static void
geoclue_hybris_geoclue_init (GcIfaceGeoclueClass *iface)
{
    iface->get_provider_info = get_provider_info;
    iface->add_reference = add_reference;
    iface->remove_reference = remove_reference;
}

static void
geoclue_hybris_position_init (GcIfacePositionClass *iface)
{
    iface->get_position = get_position;
}

static void
geoclue_hybris_velocity_init (GcIfaceVelocityClass *iface)
{
    iface->get_velocity = get_velocity;
}

static void
geoclue_hybris_satellite_init (GcIfaceSatelliteClass *iface)
{
    iface->get_satellite = get_satellite;
    iface->get_last_satellite = get_last_satellite;
}

int
main()
{
    syslog(LOG_INFO, "main start");
    g_type_init();
    hybris = g_object_new (GEOCLUE_TYPE_HYBRIS, NULL);

    hybris->loop = g_main_loop_new (NULL, TRUE);

    g_main_loop_run (hybris->loop);
    syslog(LOG_INFO, "main loop existed");

    g_main_loop_unref (hybris->loop);
    g_object_unref (hybris);

    syslog(LOG_INFO, "Terminated successfully");

    return 0;
}
