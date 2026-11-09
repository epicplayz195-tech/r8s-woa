# Windows driver plan (Exynos 990)

exynos9810-woa ships three Windows drivers: `Exynos9810Ufs`, `Exynos9810Hsi2c`
and `S6SY761Touch`. None runs on r8s as-is; this is what each needs.

| Driver | Reuse from exynos9810-woa | r8s work |
|---|---|---|
| **UFS** (blocks full Windows) | Storport miniport structure, build script, test-signing flow | New register base `0x13100000` (HCI), `0x13180000` (UniPro), CMU HSI1 `0x13000000`; S5E9830 UFS PHY init sequence; IRQ from the downstream `universal9830` DT; ACPI device node in DSDT |
| **HSI2C** | Most of it — Exynos HSI2C IP is shared across generations | Bus base / IRQ / USI mode for the touch bus from the r8s DT |
| **Touch** | Not the S6SY761 code | r8s appears to use an STM FTS5CU56A (same panel family as r8q — **verify** with `i2cdetect` / the downstream DT). The [r8q-arch](https://github.com/Itzkristis/r8q-arch) project has a small from-scratch FTS5CU56A driver documenting the protocol |

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
