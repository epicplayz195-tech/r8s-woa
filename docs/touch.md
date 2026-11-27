# Touch screen (r8stouch.sys)

The S20 FE's panel controller is a **Zinitix ZT7650** at I2C address `0x20`.
`drivers/r8stouch` is a port of the r8sios XNU driver
(`xnu/files/bsd/dev/redwood_zinitix.c`, working under iOS on this phone):

| | |
|---|---|
| Bus | bit-banged on the HSI2C_15 pins: `gpp5-6` SCL, `gpp5-7` SDA (PERIC1 pinctrl `0x10730000`) |
| Power | `tsp_ldo_en` (3.0 V), enabled by `gpm21-0` (CMGP pinctrl `0x15C30000` + `0x2A0`) |
| Attention | `gpa1-0` (ALIVE pinctrl `0x15850000` + `0x20`), low while events are queued; polled |
| Protocol | 16-bit LE registers; vendor power-on (`0x10F0` enable, `0x17F0` chip code `0xE650`, `0x14F0`, `0x12F0`, `0x11F0` program start), then `0x0000` reset, touch mode `0`; 16-byte event records at `0x0200`/`0x0201` |
| Windows | KMDF lower filter under inbox `mshidkmdf.sys`: a HID touch screen, 10 contacts |

ACPI: `\_SB.TSC0`, `_HID` `RWOA0003`, `_CRS` = the three pinctrl blocks.

## Install into the desktop WinPE

1. Unzip the **r8s-drivers-arm64** artifact of *Build Windows drivers (ARM64)* into `D:\woa\drivers`.
2. Administrator PowerShell: `tools\windows\Add-DesktopPe.ps1` (copies the drivers into
   `boot.wim` and runs `X:\r8s\install-drivers.cmd` when the desktop starts).
3. Phone in TWRP: flash a UEFI that has `TSC0`, then `tools\device\Deploy-DesktopPe.ps1 -NewBootMgr`
   (the 23H2 BCD has `testsigning` on, which test-signed drivers need).

## Reading what it did

`X:\r8s\install-log.txt`, or `regedit` →
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RWOA0003\0\Device Parameters`:

| State | Meaning |
|---|---|
| 0 | hardware mapped, polling thread not started yet |
| 1 | running: the controller answered and was initialised |
| 2 | the bus is held low (no power to the controller?) |
| 3 | nothing answers at `0x20` |
| 4 | the software reset was not acknowledged |
| 5 | the touch-mode write was not acknowledged |

Also: `ChipCode` (`0xE650` = 58960 for a ZT7650; `0x17F0` = the firmware did not start),
`Revision`, `Firmware`, `ResolutionX/Y`, `Attempts`, and every ~30 s `Reads`, `Events`,
`FailedReads`.
