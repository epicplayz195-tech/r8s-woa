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
    }
}
