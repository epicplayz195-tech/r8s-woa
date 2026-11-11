#include <Library/MemoryMapLib.h>

//
// r8s-woa memory map for the Galaxy S20 FE (r8s, SM-G780F, S5E9830).
//
// Base: upstream Mu-Silicium r8sPkg plus the x1s regions (second EL2 code,
// S2MPU table, USB DRD, CMU HSI1, MSHC, SYSREG CMGP, full UniPro window).
// DRAM, display and EL2 values are taken from the phone itself (r8sios
// reference/r8s: /proc/iomem and the live device tree), and this layout
// boots to the Silicium menu on a bit-24 S20 FE:
//   System RAM 0x80000000-0xBAAFFFFF, 0xC1200000-0xDFFFFFFF,
//              0xE1900000-0xFFFFFFFF, 0x880000000-0x9FFFFFFFF
//   reserved-memory/el2_earlymem: reg = <0x9 0xfec00000 0x1400000>
//

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  {"EL2 Code",           0x83C80000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"EL2 Code",           0xC1400000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK}, // from x1s
  {"S2MPU Table",        0xC1800000, 0x00300000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK}, // from x1s
  {"UEFI FD",            0xE8000000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"DXE Heap",           0xE8200000, 0x03C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"UEFI Stack",         0xEBE00000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  // Panel 1080x2400 (EA8079/S6E3FC3), 4 bytes a pixel = 0x9E3400, rounded up to a page.
  {"Display Reserved",   0xF1000000, 0x009E4000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  {"RAM Partition",      0xF19E4000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK}, // TEMP: Needed for MMU to Pass.
  {"EL2 Early Memory",   0x9FEC00000,0x01400000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK}, // live DT el2_earlymem
  // r8s-woa: Windows -> firmware log handoff (r8slog.sys writes, R8sDiag saves it to the ESP after
  // a reboot). Inside Samsung's debug-snapshot log_platform region, which the bootloader keeps across
  // warm resets.
  {"Windows Log",        0xFDB10000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK},

  // iRAM Regions
  {"IRAM",               0x02020000, 0x00018000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   DEVICE},
  {"NS_IRAM",            0x02038000, 0x00039000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},

  // Register Regions
  {"Chip Info",          0x10000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICD",               0x10101000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICC",               0x10102000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICH",               0x10104000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICV",               0x10106000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SYSREG PERIC0",      0x10420000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SYSREG PERIC1",      0x10720000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SYSREG HSI0",        0x10A20000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB DRD PHY",        0x10C00000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}, // from x1s
  {"USB DRD Link",       0x10E00000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}, // from x1s
  {"CMU HSI1",           0x13000000, 0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}, // from x1s
  {"SYSREG HSI1",        0x13020000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS HCI",            0x13100000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"MSHC",               0x132E0000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}, // from x1s (microSD)
  {"UFS UniPro",         0x13180000, 0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}, // was 0x1000
  {"PMU",                0x15860000, 0x00004000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SYSREG CMGP",        0x15C20000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}, // from x1s
  {"Decon",              0x19050000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Clock Controller",   0x1A330000, 0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
