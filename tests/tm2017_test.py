#!/usr/bin/python3

import os

from gi.repository import GLib

from integration_test import Tests


class TM2017Tests(Tests):
    """Regression tests for the TM2017 platform-profile behavior."""

    def test_balanced_power_source(self):
        """Balanced follows AC/battery using balanced-performance on AC."""
        acpi_dir = os.path.join(self.testbed.get_root_dir(), "sys/firmware/acpi/")
        os.makedirs(acpi_dir)

        self.write_file_contents(
            os.path.join(acpi_dir, "platform_profile"),
            "balanced\n",
        )
        self.write_file_contents(
            os.path.join(acpi_dir, "platform_profile_choices"),
            "low-power balanced balanced-performance performance\n",
        )

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

        upowerd_obj.Set(
            "org.freedesktop.UPower",
            "OnBattery",
            True,
        )
        self.assert_file_eventually_contains(
            os.path.join(acpi_dir, "platform_profile"),
            "balanced",
        )

        upowerd_obj.Set(
            "org.freedesktop.UPower",
            "OnBattery",
            False,
        )
        self.assert_file_eventually_contains(
            os.path.join(acpi_dir, "platform_profile"),
            "balanced-performance",
        )
