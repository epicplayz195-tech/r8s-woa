/** @file
  r8s-woa: hand the DWC3 USB controller to Windows in host mode.

  UEFI on the r8s only uses USB in device mode (FFU), and only when asked.
  Windows has no Exynos USB driver, but the DWC3 contains a standard xHCI
  host controller, which Windows' inbox usbxhci drives once the controller is
  in host mode and its PHY is powered. At ExitBootServices this powers the
  PHY (same sequence as the S5E9830 PlatformUsbfnLib), resets the core and
  selects host mode. The ACPI DSDT describes the controller as PNP0D10.

  VBUS (5 V to the USB device) is NOT switched on: that needs the PMIC /
  charger driver. Use a self-powered USB hub or a powered OTG adapter.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/PlatformUsbfnLib.h>
#include <Device/Dwc3.h>

STATIC EFI_EVENT  mExitBootServicesEvent;

STATIC
VOID
EFIAPI
UsbHostOnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  USBFN_PLATFORM_DATA   Platform;
  EFI_PHYSICAL_ADDRESS  Base;
  UINT32                Reg;

  if (EFI_ERROR (UsbfnPlatformGetData (&Platform)) || (Platform.ControllerBase == 0)) {
    return;
  }

  Base = Platform.ControllerBase;

  if (EFI_ERROR (UsbfnPlatformPhyInit ())) {
    return;
  }

  // Not a DWC3? Leave it alone.
  Reg = MmioRead32 (Base + DWC3_GSNPSID);
  if (((Reg & DWC3_GSNPSID_MASK) != DWC3_GSNPSID_USB3) && ((Reg & DWC3_GSNPSID_MASK) != DWC3_GSNPSID_USB31)) {
    return;
  }

  // Core + PHY soft reset (host-mode style: GCTL.CORESOFTRESET).
  MmioOr32 (Base + DWC3_GCTL, DWC3_GCTL_CORESOFTRESET);
  if (!Platform.SuperSpeedPhyDisabled) {
    MmioOr32 (Base + DWC3_GUSB3PIPECTL (0), DWC3_GUSB3PIPECTL_PHYSOFTRST);
  }

  MmioOr32 (Base + DWC3_GUSB2PHYCFG (0), DWC3_GUSB2PHYCFG_PHYSOFTRST);
  gBS->Stall (100);
  if (!Platform.SuperSpeedPhyDisabled) {
    MmioAnd32 (Base + DWC3_GUSB3PIPECTL (0), ~(UINT32)DWC3_GUSB3PIPECTL_PHYSOFTRST);
  }

  MmioAnd32 (Base + DWC3_GUSB2PHYCFG (0), ~(UINT32)DWC3_GUSB2PHYCFG_PHYSOFTRST);
  gBS->Stall (100);
  MmioAnd32 (Base + DWC3_GCTL, ~(UINT32)DWC3_GCTL_CORESOFTRESET);
  gBS->Stall (100);

  // PHY interface: UTMI width, no PHY suspend (Linux does the same quirks).
  Reg  = MmioRead32 (Base + DWC3_GUSB2PHYCFG (0));
  Reg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK | DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
  Reg &= ~(DWC3_GUSB2PHYCFG_SUSPHY | DWC3_GUSB2PHYCFG_ENBLSLPM);
  if (Platform.UtmiWidth == UsbfnUtmiWidth16Bit) {
    Reg |= DWC3_GUSB2PHYCFG_PHYIF (UTMI_PHYIF_16_BIT) | DWC3_GUSB2PHYCFG_USBTRDTIM (USBTRDTIM_UTMI_16_BIT);
  } else {
    Reg |= DWC3_GUSB2PHYCFG_USBTRDTIM (USBTRDTIM_UTMI_8_BIT);
  }

  MmioWrite32 (Base + DWC3_GUSB2PHYCFG (0), Reg);

  if (Platform.SuperSpeedPhyDisabled) {
    MmioOr32 (Base + DWC3_GUSB3PIPECTL (0), DWC3_GUSB3PIPECTL_SUSPHY);
  } else {
    MmioAnd32 (Base + DWC3_GUSB3PIPECTL (0), ~(UINT32)DWC3_GUSB3PIPECTL_SUSPHY);
  }

  // Host mode.
  Reg  = MmioRead32 (Base + DWC3_GCTL);
  Reg &= ~(DWC3_GCTL_SCALEDOWN_MASK | DWC3_GCTL_DISSCRAMBLE | DWC3_GCTL_GBLHIBERNATIONEN | DWC3_GCTL_DSBLCLKGTNG);
  Reg &= ~DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_OTG);
  Reg |= DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_HOST);
  MmioWrite32 (Base + DWC3_GCTL, Reg);
  gBS->Stall (100);
}

EFI_STATUS
EFIAPI
UsbHostDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_NOTIFY,
                UsbHostOnExitBootServices,
                NULL,
                &gEfiEventExitBootServicesGuid,
                &mExitBootServicesEvent
                );
}
