#!/usr/bin/python3

import os

from gi.repository import GLib

from integration_test import Tests


class TM2017Tests(Tests):
    """Regression tests for the TM2017 platform-profile behavior."""

    def _setup_platform_profile(self, initial_profile="balanced"):
        acpi_dir = os.path.join(self.testbed.get_root_dir(), "sys/firmware/acpi/")
        os.makedirs(acpi_dir)

        self.write_file_contents(
            os.path.join(acpi_dir, "platform_profile"),
            initial_profile + "\n",
        )
        self.write_file_contents(
            os.path.join(acpi_dir, "platform_profile_choices"),
            "low-power balanced balanced-performance performance\n",
        )
        return acpi_dir

    def test_balanced_power_source(self):
        """Balanced follows AC/battery using balanced-performance on AC."""
        acpi_dir = self._setup_platform_profile()

        _, upowerd_obj, _ = self.start_dbus_template(
            "upower",
            {"DaemonVersion": "0.99", "OnBattery": False},
        )

        self.start_daemon()

        profiles = self.get_dbus_property("Profiles")
        self.assertEqual(len(profiles), 3)

        self.set_dbus_property(
            "ActiveProfile",
            GLib.Variant.new_string("balanced"),
        )
        self.assertEqual(
            self.read_sysfs_file(os.path.join(
                "sys/firmware/acpi/platform_profile"
            )),
            b"balanced-performance",
        )

        upowerd_obj.Set("org.freedesktop.UPower", "OnBattery", True)
        self.assert_file_eventually_contains(
            os.path.join(acpi_dir, "platform_profile"),
            "balanced",
        )

        upowerd_obj.Set("org.freedesktop.UPower", "OnBattery", False)
        self.assert_file_eventually_contains(
            os.path.join(acpi_dir, "platform_profile"),
            "balanced-performance",
        )

    def test_performance_ignores_power_source(self):
        """Performance stays on hardware performance across AC/battery changes."""
        acpi_dir = self._setup_platform_profile("performance")

        _, upowerd_obj, _ = self.start_dbus_template(
            "upower",
            {"DaemonVersion": "0.99", "OnBattery": False},
        )

        self.start_daemon()
        self.set_dbus_property(
            "ActiveProfile",
            GLib.Variant.new_string("performance"),
        )
        self.assertEqual(
            self.read_sysfs_file(os.path.join(
                "sys/firmware/acpi/platform_profile"
            )),
            b"performance",
        )

        upowerd_obj.Set("org.freedesktop.UPower", "OnBattery", True)
        self.assert_file_eventually_contains(
            os.path.join(acpi_dir, "platform_profile"),
            "performance",
        )

        upowerd_obj.Set("org.freedesktop.UPower", "OnBattery", False)
        self.assert_file_eventually_contains(
            os.path.join(acpi_dir, "platform_profile"),
            "performance",
        )

    def test_power_saver_ignores_power_source(self):
        """Power-saver stays on hardware low-power across AC/battery changes."""
        acpi_dir = self._setup_platform_profile("low-power")

        _, upowerd_obj, _ = self.start_dbus_template(
            "upower",
            {"DaemonVersion": "0.99", "OnBattery": False},
        )

        self.start_daemon()
        self.set_dbus_property(
            "ActiveProfile",
            GLib.Variant.new_string("power-saver"),
        )
        self.assertEqual(
            self.read_sysfs_file(os.path.join(
                "sys/firmware/acpi/platform_profile"
            )),
            b"low-power",
        )

        upowerd_obj.Set("org.freedesktop.UPower", "OnBattery", True)
        self.assert_file_eventually_contains(
            os.path.join(acpi_dir, "platform_profile"),
            "low-power",
        )

        upowerd_obj.Set("org.freedesktop.UPower", "OnBattery", False)
        self.assert_file_eventually_contains(
            os.path.join(acpi_dir, "platform_profile"),
            "low-power",
        )

    def test_failed_activation_preserves_selected_profile(self):
        """A failed platform activation must not change the selected profile."""
        acpi_dir = self._setup_platform_profile()
        profile_path = os.path.join(acpi_dir, "platform_profile")

        self.start_daemon()
        self.set_dbus_property(
            "ActiveProfile",
            GLib.Variant.new_string("balanced"),
        )
        self.assertEqual(self.get_dbus_property("ActiveProfile"), "balanced")
        self.assertEqual(self.read_sysfs_file("sys/firmware/acpi/platform_profile"),
                         b"balanced-performance")

        # Remove the emulated sysfs attribute so activation cannot read or
        # update it. The logical profile must remain unchanged on failure.
        os.remove(profile_path)

        with self.assertRaises(GLib.Error):
            self.set_dbus_property(
                "ActiveProfile",
                GLib.Variant.new_string("performance"),
            )

        self.assertEqual(self.get_dbus_property("ActiveProfile"), "balanced")
