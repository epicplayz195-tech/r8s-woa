# Windows driver plan (Exynos 990)

exynos9810-woa ships three Windows drivers: `Exynos9810Ufs`, `Exynos9810Hsi2c`
and `S6SY761Touch`. None runs on r8s as-is; this is what each needs.

| Driver | Reuse from exynos9810-woa | r8s work |
|---|---|---|
| **UFS** (blocks full Windows) | Storport miniport structure, build script, test-signing flow. Hardware logic: r8sios `redwood_ufshc.c` (iOS kernel UFS driver, already working on this phone) | New register base `0x13100000` (HCI), `0x13180000` (UniPro), CMU HSI1 `0x13000000`; S5E9830 UFS PHY init sequence; IRQ from the downstream `universal9830` DT; ACPI device node in DSDT |
| **HSI2C** | Most of it — Exynos HSI2C IP is shared across generations | Bus base / IRQ / USI mode for the touch bus from the r8s DT |
| **Touch** | The HID-over-driver structure of `S6SY761Touch`, not its protocol code | r8s uses a **Zinitix ZT7650** on I2C bus 5, address `0x20`. Already working under iOS: r8sios `xnu/files/bsd/dev/redwood_zinitix.c`. Protocol: vendor commands 0x10F0/0x17F0/0x14F0/0x12F0/0x11F0, touch mode 0, 16-byte events at 0x0200/0x0201. Power: the driver must turn on `tsp_ldo_en` (GPIO gpm21-0) itself |

## Best reference: the r8sios UFS driver

r8sios has a working Exynos 990 UFS driver for the iOS kernel
(`kernel/xnu/files/bsd/dev/redwood_ufshc.c`, notes in `docs/plans/09-standalone.md`).
It can't be loaded by Windows, but its approach carries straight over to a
Storport miniport: take over the controller UEFI's `UfsDxe` already
initialised (no PHY bring-up from scratch), build a new transfer request list,
and handle the Exynos quirks it documents (response/PRDT layout, clearing
UTRCS before IS). Rewrite that logic as Windows code rather than copying it.

## Touch reference: the r8sios Zinitix driver

`redwood_zinitix.c` is a working ZT7650 driver on this phone; `docs/r8s.md`
step 11 lists the protocol details. A Windows version is a KMDF HID
mini-driver on top of the HSI2C bus driver, with the same command sequence.
ACPI needs an I2C device node for bus 5 / 0x20, its interrupt GPIO and the
`tsp_ldo_en` power GPIO.

## Other hardware notes from r8sios

- Wi-Fi/Bluetooth is a Qualcomm **QCA6390** over PCIe (not Broadcom like
  other S20s); not ported to iOS either.
- HSI1 is shared between PCIe and UFS: don't gate HSI1 clocks from a PCIe driver.

## Getting the hardware facts

From TWRP on the phone:
```sh
cat /proc/iomem
ls /sys/bus/i2c/devices/ ; cat /sys/bus/i2c/devices/*/name
cat /proc/interrupts
```
Downstream kernel source: `android_kernel_samsung_universal9830` (r8s branches),
`arch/arm64/boot/dts/exynos/exynos9830.dtsi` and the r8s board DTs.

## Order of work

1. UFS miniport (without it Windows only works as WinPE in RAM).
2. ACPI DSDT entries for UFS/HSI2C/touch in `Silicium-ACPI` r8s copy.
3. HSI2C + touch.
4. Multicore (PSCI in MADT), then everything else.

Drivers go under `drivers/<Name>/` with the same layout and test-signing as
exynos9810-woa; mind the licenses (touch derived from GPL code stays GPL-2.0).
