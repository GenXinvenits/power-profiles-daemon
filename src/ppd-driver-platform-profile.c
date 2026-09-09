/*
 * Copyright (c) 2020 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#define G_LOG_DOMAIN "PlatformDriver"

#include <gudev/gudev.h>
#include <gio/gio.h>

#include "ppd-driver-platform-profile.h"
#include "ppd-utils.h"

#define LAPMODE_SYSFS_NAME "dytc_lapmode"
#define ACPI_PLATFORM_PROFILE_PATH "/sys/firmware/acpi/platform_profile"
#define ACPI_PLATFORM_PROFILE_CHOICES_PATH "/sys/firmware/acpi/platform_profile_choices"

struct _PpdDriverPlatformProfile
{
  PpdDriver  parent_instance;

  PpdProbeResult probe_result;
  GUdevDevice *device;
  int lapmode;
  PpdProfile acpi_platform_profile;
  PpdProfile selected_profile;
  PpdPowerChangedReason power_changed_reason;
  char **profile_choices;
  gboolean has_low_power;
  GFileMonitor *lapmode_mon;
  GFileMonitor *acpi_platform_profile_mon;
  gulong acpi_platform_profile_changed_id;
};

G_DEFINE_TYPE (PpdDriverPlatformProfile, ppd_driver_platform_profile, PPD_TYPE_DRIVER_PLATFORM)

static GObject*
ppd_driver_platform_profile_constructor (GType                  type,
                                         guint                  n_construct_params,
                                         GObjectConstructParam *construct_params)
{
  GObject *object;

  object = G_OBJECT_CLASS (ppd_driver_platform_profile_parent_class)->constructor (type,
                                                                                   n_construct_params,
                                                                                   construct_params);
  g_object_set (object,
                "driver-name", "platform_profile",
                "profiles", PPD_PROFILE_PERFORMANCE | PPD_PROFILE_BALANCED | PPD_PROFILE_POWER_SAVER,
                NULL);

  return object;
}

static const char *
profile_to_acpi_platform_profile_value (PpdDriverPlatformProfile *self,
                                        PpdProfile                profile)
{
  switch (profile) {
  case PPD_PROFILE_POWER_SAVER:
    if (!self->has_low_power)
      return "balanced";
    if (g_strv_contains ((const char * const*) self->profile_choices, "low-power"))
      return "low-power";
    return "quiet";
  case PPD_PROFILE_BALANCED:
    if (self->power_changed_reason == PPD_POWER_CHANGED_REASON_AC &&
        (g_strv_contains ((const char * const*) self->profile_choices, "balanced-performance") ||
         g_strv_contains ((const char * const*) self->profile_choices, "balanced_performance")))
      return g_strv_contains ((const char * const*) self->profile_choices, "balanced-performance") ?
             "balanced-performance" : "balanced_performance";
    if (g_strv_contains ((const char * const*) self->profile_choices, "balanced"))
      return "balanced";
    if (g_strv_contains ((const char * const*) self->profile_choices, "cool"))
      return "cool";
    return "balanced";
  case PPD_PROFILE_PERFORMANCE:
    return "performance";
  }

  g_debug ("Unhandled ACPI platform profile '%d'", profile);
  g_return_val_if_reached (NULL);
}

static PpdProfile
acpi_platform_profile_value_to_profile (const char *str)
{
  if (str == NULL)
    return PPD_PROFILE_UNSET;

  if (g_str_equal (str, "custom"))
    return PPD_PROFILE_UNSET;

  if (g_str_equal (str, "low-power") ||
      g_str_equal (str, "quiet"))
    return PPD_PROFILE_POWER_SAVER;

  if (g_str_equal (str, "balanced") ||
      g_str_equal (str, "balanced-performance") ||
      g_str_equal (str, "balanced_performance") ||
      g_str_equal (str, "cool"))
    return PPD_PROFILE_BALANCED;

  if (g_str_equal (str, "performance"))
    return PPD_PROFILE_PERFORMANCE;

  g_debug ("Unhandled ACPI platform profile '%s'", str);
  g_return_val_if_reached (PPD_PROFILE_UNSET);
}

static PpdProfile
read_platform_profile (void)
{
  g_autofree char *platform_profile_path = NULL;
  g_autofree char *new_profile_str = NULL;
  g_autoptr(GError) error = NULL;
  PpdProfile new_profile;

  platform_profile_path = ppd_utils_get_sysfs_path (ACPI_PLATFORM_PROFILE_PATH);
  if (!g_file_get_contents (platform_profile_path,
                            &new_profile_str, NULL, &error)) {
    g_debug ("Failed to get contents for '%s': %s",
             platform_profile_path,
             error->message);
    return PPD_PROFILE_UNSET;
  }

  new_profile = acpi_platform_profile_value_to_profile (g_strchomp (new_profile_str));
  g_debug ("ACPI performance_profile is now '%s', so profile is detected as %s",
           g_strchomp (new_profile_str),
           ppd_profile_to_str (new_profile));
  return new_profile;
}

static gboolean
save_platform_profile_choices (PpdDriverPlatformProfile *self)
{
  g_autofree char *platform_profile_choices_path = NULL;
  g_autofree char *choices_str = NULL;
  g_autoptr(GError) error = NULL;

  platform_profile_choices_path = ppd_utils_get_sysfs_path (ACPI_PLATFORM_PROFILE_CHOICES_PATH);
  if (!g_file_get_contents (platform_profile_choices_path,
                            &choices_str, NULL, &error)) {
    g_debug ("Failed to get contents for '%s': %s",
             platform_profile_choices_path,
             error->message);
    return FALSE;
  }

  self->profile_choices = g_strsplit_set (choices_str, " \n", -1);
  return TRUE;
}

static PpdProbeResult
verify_acpi_platform_profile_choices (PpdDriverPlatformProfile *self)
{
  const char * const *choices = (const char * const*) self->profile_choices;

  if (g_strv_contains (choices, "balanced") &&
      g_strv_contains (choices, "performance")) {
    if (g_strv_contains (choices, "low-power") ||
        g_strv_contains (choices, "quiet"))
      self->has_low_power = TRUE;
    else
      g_debug ("No \"low-power\" profile for device, will be emulated");
    return PPD_PROBE_RESULT_SUCCESS;
  }
  return PPD_PROBE_RESULT_DEFER;
}

static void
update_dytc_lapmode_state (PpdDriverPlatformProfile *self)
{
  int new_lapmode;

  new_lapmode = g_udev_device_get_sysfs_attr_as_int_uncached (self->device, LAPMODE_SYSFS_NAME);
  if (new_lapmode == self->lapmode)
    return;

  self->lapmode = new_lapmode;
  g_debug ("dytc_lapmode is now %s, so profile is %s",
           self->lapmode ? "on" : "off",
           self->lapmode ? "degraded" : "not degraded");
  g_object_set (G_OBJECT (self),
                "performance-degraded", self->lapmode ? "lap-detected" : NULL,
                NULL);
}

static void
update_acpi_platform_profile_state (PpdDriverPlatformProfile *self)
{
  PpdProfile new_profile;

  new_profile = read_platform_profile ();
  if (new_profile == PPD_PROFILE_UNSET ||
      new_profile == self->acpi_platform_profile)
    return;

  self->acpi_platform_profile = new_profile;
  ppd_driver_emit_profile_changed (PPD_DRIVER (self), new_profile);
}

static void
lapmode_changed (GFileMonitor      *monitor,
                 GFile             *file,
                 GFile             *other_file,
                 GFileMonitorEvent  event_type,
                 gpointer           user_data)
{
  PpdDriverPlatformProfile *self = user_data;

  g_debug (LAPMODE_SYSFS_NAME " attribute changed (event: %d)", event_type);
  g_return_if_fail (event_type != G_FILE_MONITOR_EVENT_DELETED);

  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
    update_dytc_lapmode_state (self);
}

static void
acpi_platform_profile_changed (GFileMonitor      *monitor,
                               GFile             *file,
                               GFile             *other_file,
                               GFileMonitorEvent  event_type,
                               gpointer           user_data)
{
  PpdDriverPlatformProfile *self = user_data;

  g_debug (ACPI_PLATFORM_PROFILE_PATH " changed (%d)", event_type);
  if (self->probe_result == PPD_PROBE_RESULT_DEFER) {
    g_signal_emit_by_name (G_OBJECT (self), "probe-request", 0);
    return;
  }

  g_return_if_fail (event_type != G_FILE_MONITOR_EVENT_DELETED);

  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
    update_acpi_platform_profile_state (self);
}

static gboolean
ppd_driver_platform_profile_activate_profile (PpdDriver                   *driver,
                                              PpdProfile                   profile,
                                              PpdProfileActivationReason   reason,
                                              GError                     **error)
{
  PpdDriverPlatformProfile *self = PPD_DRIVER_PLATFORM_PROFILE (driver);
  g_autoptr(GError) local_error = NULL;
  g_autofree char *platform_profile_path = NULL;
  g_autofree char *current_profile_value = NULL;
  const char *platform_profile_value;

  g_return_val_if_fail (self->acpi_platform_profile_mon, FALSE);

  platform_profile_value = profile_to_acpi_platform_profile_value (self, profile);

  platform_profile_path = ppd_utils_get_sysfs_path (ACPI_PLATFORM_PROFILE_PATH);
  if (!g_file_get_contents (platform_profile_path,
                            &current_profile_value, NULL, &local_error)) {
    g_debug ("Failed to read current acpi_platform_profile: %s", local_error->message);
    g_propagate_prefixed_error (error, g_steal_pointer (&local_error),
                                "Failed to read acpi_platform_profile: ");
    return FALSE;
  }

  g_strchomp (current_profile_value);

  if (g_str_equal (current_profile_value, platform_profile_value)) {
    g_debug ("Platform profile already set to %s for logical profile %s",
             platform_profile_value,
             ppd_profile_to_str (profile));
    self->acpi_platform_profile = profile;
    self->selected_profile = profile;
    return TRUE;
  }

  /* Preserve PPD's hardware-profile emulation for devices such as HP-WMI.
   * On those systems, "cool" represents the logical "balanced" profile.
   * A device exposing balanced-performance is handled specially because
   * balanced must follow the current AC/battery state. */
  if (self->acpi_platform_profile == acpi_platform_profile_value_to_profile (platform_profile_value) &&
      !(profile == PPD_PROFILE_BALANCED &&
        (g_strv_contains ((const char * const*) self->profile_choices, "balanced-performance") ||
         g_strv_contains ((const char * const*) self->profile_choices, "balanced_performance")))) {
    g_debug ("Not switching to platform_profile %s, emulating for %s, already there",
             platform_profile_value,
             ppd_profile_to_str (profile));
    self->selected_profile = profile;
    return TRUE;
  }

  g_signal_handler_block (G_OBJECT (self->acpi_platform_profile_mon), self->acpi_platform_profile_changed_id);
  if (!ppd_utils_write (platform_profile_path,
                        platform_profile_value, &local_error)) {
    g_debug ("Failed to write to acpi_platform_profile: %s", local_error->message);
    g_propagate_prefixed_error (error, g_steal_pointer (&local_error),
                                "Failed to write to acpi_platform_profile: ");
    g_signal_handler_unblock (G_OBJECT (self->acpi_platform_profile_mon), self->acpi_platform_profile_changed_id);
    return FALSE;
  }
  g_signal_handler_unblock (G_OBJECT (self->acpi_platform_profile_mon), self->acpi_platform_profile_changed_id);

  g_debug ("Successfully switched to hardware profile %s for logical profile %s",
           platform_profile_value,
           ppd_profile_to_str (profile));
  self->acpi_platform_profile = profile;
  self->selected_profile = profile;
  return TRUE;
}

static gboolean
ppd_driver_platform_profile_power_changed (PpdDriver                  *driver,
                                           PpdPowerChangedReason       reason,
                                           GError                    **error)
{
  PpdDriverPlatformProfile *self = PPD_DRIVER_PLATFORM_PROFILE (driver);

  self->power_changed_reason = reason;

  if (self->selected_profile != PPD_PROFILE_BALANCED)
    return TRUE;

  if (reason != PPD_POWER_CHANGED_REASON_AC &&
      reason != PPD_POWER_CHANGED_REASON_BATTERY)
    return TRUE;

  g_debug ("Power source changed to %s while balanced is selected, updating platform profile",
           ppd_power_changed_reason_to_str (reason));

  return ppd_driver_platform_profile_activate_profile (driver,
                                                        PPD_PROFILE_BALANCED,
                                                        PPD_PROFILE_ACTIVATION_REASON_INTERNAL,
                                                        error);
}

static int
find_dytc (GUdevDevice *dev,
           gpointer     user_data)
{
  if (g_strcmp0 (g_udev_device_get_name (dev), "thinkpad_acpi") != 0)
    return 1;

  if (!g_udev_device_get_sysfs_attr (dev, LAPMODE_SYSFS_NAME))
    return 1;

  return 0;
}

static PpdProbeResult
ppd_driver_platform_profile_probe (PpdDriver  *driver)
{
  PpdDriverPlatformProfile *self = PPD_DRIVER_PLATFORM_PROFILE (driver);
  g_autoptr(GFile) acpi_platform_profile = NULL;
  g_autofree char *platform_profile_path = NULL;

  g_return_val_if_fail (self->probe_result == PPD_PROBE_RESULT_UNSET, PPD_PROBE_RESULT_FAIL);

  /* Profile interface */
  platform_profile_path = ppd_utils_get_sysfs_path (ACPI_PLATFORM_PROFILE_PATH);
  if (!g_file_test (platform_profile_path, G_FILE_TEST_EXISTS)) {
    g_debug ("No platform_profile sysfs file");
    return PPD_PROBE_RESULT_FAIL;
  }
  if (!save_platform_profile_choices (self))
    return PPD_PROBE_RESULT_FAIL;
  self->probe_result = verify_acpi_platform_profile_choices (self);
  if (self->probe_result == PPD_PROBE_RESULT_FAIL) {
    g_debug ("No supported platform_profile choices");
    return self->probe_result;
  }

  acpi_platform_profile = g_file_new_for_path (platform_profile_path);
  self->acpi_platform_profile_mon = g_file_monitor (acpi_platform_profile,
                                                    G_FILE_MONITOR_NONE,
                                                    NULL,
                                                    NULL);
  self->acpi_platform_profile_changed_id =
    g_signal_connect (G_OBJECT (self->acpi_platform_profile_mon), "changed",
                      G_CALLBACK (acpi_platform_profile_changed), self);
  if (self->probe_result == PPD_PROBE_RESULT_DEFER) {
    g_debug ("Monitoring platform_profile sysfs file");
    return self->probe_result;
  }

  /* Lenovo-specific proximity sensor */
  self->device = ppd_utils_find_device ("platform",
                                        (GCompareFunc) find_dytc,
                                        NULL);
  if (!self->device)
    goto out;

  self->lapmode_mon = ppd_utils_monitor_sysfs_attr (self->device,
                                                    LAPMODE_SYSFS_NAME,
                                                    NULL);
  g_signal_connect_object (G_OBJECT (self->lapmode_mon), "changed",
                           G_CALLBACK (lapmode_changed), self, 0);
  update_dytc_lapmode_state (self);

out:
  update_acpi_platform_profile_state (self);

  g_debug ("%s a dytc_lapmode sysfs attribute to thinkpad_acpi",
           self->device ? "Found" : "Didn't find");
  return PPD_PROBE_RESULT_SUCCESS;
}

static void
ppd_driver_platform_profile_finalize (GObject *object)
{
  PpdDriverPlatformProfile *driver;

  driver = PPD_DRIVER_PLATFORM_PROFILE (object);
  g_clear_signal_handler (&driver->acpi_platform_profile_changed_id,
                          driver->acpi_platform_profile_mon);
  g_clear_pointer (&driver->profile_choices, g_strfreev);
  g_clear_object (&driver->device);
  g_clear_object (&driver->lapmode_mon);
  g_clear_object (&driver->acpi_platform_profile_mon);
  G_OBJECT_CLASS (ppd_driver_platform_profile_parent_class)->finalize (object);
}

static void
ppd_driver_platform_profile_class_init (PpdDriverPlatformProfileClass *klass)
{
  GObjectClass *object_class;
  PpdDriverClass *driver_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->constructor = ppd_driver_platform_profile_constructor;
  object_class->finalize = ppd_driver_platform_profile_finalize;

  driver_class = PPD_DRIVER_CLASS (klass);
  driver_class->probe = ppd_driver_platform_profile_probe;
  driver_class->activate_profile = ppd_driver_platform_profile_activate_profile;
  driver_class->power_changed = ppd_driver_platform_profile_power_changed;
}

static void
ppd_driver_platform_profile_init (PpdDriverPlatformProfile *self)
{
  self->probe_result = PPD_PROBE_RESULT_UNSET;
  self->selected_profile = PPD_PROFILE_UNSET;
  self->power_changed_reason = PPD_POWER_CHANGED_REASON_UNKNOWN;
  self->acpi_platform_profile = PPD_PROFILE_UNSET;
  self->lapmode = -1;
}

PpdDriver *
ppd_driver_platform_profile_new (void)
{
  return PPD_DRIVER (g_object_new (PPD_TYPE_DRIVER_PLATFORM_PROFILE, NULL));
}
