# EHCI for OPENSTEP

EHCI is an OPENSTEP driver for PCI EHCI USB 2.0 controllers.
It supports USB HID boot-protocol mice and keyboards and read-only USB
mass-storage disks and data CD-ROMs using SCSI Bulk-Only Transport, exposed as
up to four removable SCSI targets. High-speed root devices and one tier of
high-speed USB 2.0 hubs are supported, including low/full-speed devices behind
single-TT or multi-TT hubs.

## Requirements

- OPENSTEP 4.2 for Intel processors
- PCI EHCI USB 2.0 controller
- Controller registers and DMA memory addressable below 4 GB
- A routed PCI interrupt for the default INTx mode
- A high-speed USB 2.0 hub for low/full-speed keyboards and mice

Directly connected low/full-speed devices are handed to a companion controller;
this driver does not implement UHCI or OHCI. A high-speed hub may be built into
the chipset, as on the tested Intel 6 Series controllers.

The driver uses shared INTx by default. Keep `Share IRQ Levels` set to `Yes`.
Polling is an explicit compatibility mode that keeps PCI and EHCI interrupt
sources disabled and claims no DriverKit IRQ. INTx failures do not automatically
select Polling. Neither mode requires PCIMSI; MSI and MSI-X are not supported.

| `Interrupt Mode` | Requirement |
| --- | --- |
| `INTx` (default) | Valid PCI interrupt routing; `Share IRQ Levels = Yes` |
| `Polling` | None beyond the controller requirements; nominal 5 ms polling |

## Installation

Open `EHCI.config`.
Configure.app should open and confirm the driver was installed.
Click Add, select `PCI EHCI USB 2.0 Controller`, and add the driver.
If the driver is not shown, check `Show All Installed Drivers` and select it.

If the controller is not automatically detected, click Expert and set `Location`
to the controller's PCI coordinates using this exact syntax:

```text
Dev:<device> Func:<function> Bus:<bus>
```

For example, PCI bus 0, device 26, function 0 is `Dev:26 Func:0 Bus:0`.
Use the bus, device, and function reported for your EHCI controller. If its PCI
ID is absent from `Auto Detect IDs`, add it in device/vendor order: Intel
`8086:1c2d`, for example, is `0x1c2d8086`. The driver also verifies the EHCI PCI
class code `0c0320`.

Keep `Interrupt Mode` set to `INTx`, or explicitly set it to `Polling` if needed.
The driver obtains its IRQ from PCI configuration; do not guess an IRQ number.
Enable `USB Input = Yes` on at most one EHCI instance and disable competing
PS/2, serial mouse or other USB input providers. Set `USB Input = No` on other
EHCI instances, or on all instances if keeping your existing input drivers.
Storage works with USB input disabled. Click Done, click Save, and Quit.

Verify that `/private/Devices/EHCI.config/Instance0.table` contains the expected
`Location`, `Interrupt Mode`, `Share IRQ Levels` and `USB Input` settings.
For additional controllers, check their corresponding `InstanceN.table` files.
In `/private/Devices/System.config/Instance0.table`, load `EHCI` after `PCIBus`
and the existing root-storage controller in `Boot Drivers` to preserve root disk
numbering. For example, `PCIBus Intel824X0 BusMasterIDE EHCI` retains IDE first.

Restart OPENSTEP to load the driver. To update an existing installation,
back up `EHCI.config`, replace its driver and resources with the new bundle,
and preserve your `InstanceN.table` settings before restarting. The driver
does not support live unloading or replacement.

Storage is read-only and limited to LUN 0. Keyboard, mouse and storage attachment
and removal are supported; eject mounted storage before unplugging it. Nested
hubs, arbitrary HID report formats, UAS, isochronous transfers and suspend/resume
are not supported.
