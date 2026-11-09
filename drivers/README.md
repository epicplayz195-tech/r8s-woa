# drivers/

Windows drivers for the Galaxy S20 FE (Exynos 990) go here, one folder per
driver (`Exynos990Ufs/`, `Exynos990Hsi2c/`, `Fts5cu56aTouch/`), following the
layout of `ntdevlabs/exynos9810-woa/drivers`.

Nothing is implemented yet. Start with [../docs/driver-plan.md](../docs/driver-plan.md).

## Known register bases (S5E9830, from the UEFI memory map)

| Block | Base | Size |
|---|---|---|
| GICD / GICC | 0x10101000 / 0x10102000 | 0x1000 each |
| USB DRD PHY / Link | 0x10C00000 / 0x10E00000 | 0x1000 / 0x10000 |
| CMU HSI1 | 0x13000000 | 0x8000 |
| SYSREG HSI1 | 0x13020000 | 0x10000 |
| UFS HCI | 0x13100000 | 0x10000 |
| UFS UniPro | 0x13180000 | 0x8000 |
| MSHC (microSD) | 0x132E0000 | 0x2000 |
| PMU | 0x15860000 | 0x4000 |
| DECON | 0x19050000 | 0x10000 |

IRQ numbers are not known yet; take them from the downstream DT.
