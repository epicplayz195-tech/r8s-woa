// r8s-woa: Silicium-ACPI S5E9830 DSDT + devices for the Galaxy S20 FE (r8s).
DefinitionBlock ("DSDT.aml", "DSDT", 2, "SAMSUN", "S5E9830 ", 4)
{
    Scope (\_SB)
    {
        Device (CPU0)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU1)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 1)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU2)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 2)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU3)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 3)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU4)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 4)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU5)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 5)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU6)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 6)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        Device (CPU7)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 7)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        //
        // r8s-woa: USB DRD controller (Synopsys DWC3 @ 0x10E00000, DT
        // usb@10E00000/dwc3, GIC SPI 310 = GSIV 342, level-high). UsbHostDxe
        // leaves it in host mode at ExitBootServices; its xHCI block is driven
        // by Windows' inbox usbxhci through the PNP0D10 compatible ID.
        // DMA is not cache-coherent on this SoC (no dma-coherent in the DT).
        //
        Device (USB0)
        {
            Name (_HID, "RWOA0001")
            Name (_CID, "PNP0D10")
            Name (_UID, 0)
            Name (_CCA, 0)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x10E00000, 0x00010000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 342 }
            })
        }

        //
        // r8s-woa: RAM log handed from Windows to the firmware across a reboot
        // (r8slog.sys -> \\.\R8sLog; R8sDiag saves it as \r8s-winlog.txt).
        // Must match "Windows Log" in r8sPkg MemoryMapLib.
        //
        Device (WLOG)
        {
            Name (_HID, "RWOA0002")
            Name (_UID, 0)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0xFDB10000, 0x00400000)
            })
        }

        //
        // r8s-woa: touch screen, Zinitix ZT7650 at I2C 0x20 on gpp5-6/7 (the HSI2C_15
        // pins), bit-banged by r8stouch.sys. Power: gpm21-0 (tsp_ldo_en). Attention:
        // gpa1-0, polled. The pinctrl blocks it uses, in this order: PERIC1, ALIVE, CMGP.
        //
        Device (TSC0)
        {
            Name (_HID, "RWOA0003")
            Name (_UID, 0)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x10730000, 0x00001000)
                Memory32Fixed (ReadWrite, 0x15850000, 0x00001000)
                Memory32Fixed (ReadWrite, 0x15C30000, 0x00001000)
            })
        }

        //
        // r8s-woa: UFS host controller (Samsung UFSHCI @ 0x13100000; DT ufs@0x13100000:
        // HCI +0x000, vendor +0x1100, GIC SPI 321 = GSIV 353; dma-coherent). Driven by
        // r8sufs.sys, polled, on the link UEFI's UfsDxe brought up.
        //
        Device (UFS0)
        {
            Name (_HID, "RWOA0004")
            Name (_UID, 0)
            Name (_CCA, 1)

            Method (_STA, 0, NotSerialized) { Return (0x0F) }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x13100000, 0x00002000)
                //
                // Storport will not start an adapter without an interrupt resource,
                // even a polled one (code 10, 0xC0000225), and connects it as
                // level-sensitive only (Galaxy S9+ project, Exynos9810 DSDT). This is
                // the UFS line itself (DT: <0 141 4>, level high); it never asserts:
                // r8sufs keeps UFS IE = 0 and clears the request IOC bit.
                //
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 353 }
            })
        }
    }
}
