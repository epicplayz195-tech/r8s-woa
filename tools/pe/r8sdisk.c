/*
 * r8sdisk.exe - disk state report for the r8s-woa WinPE test (ARM64 console, no dependencies).
 *
 *   r8sdisk            report every \\.\PhysicalDriveN: bus type, offline/read-only attributes,
 *                      partition layout, then every volume Windows has
 *   r8sdisk online     same, but first clear the OFFLINE and READ-ONLY attributes of each disk
 *                      on a UFS/SCSI bus (not persistent: only for this boot) and ask the
 *                      partition manager to re-read the layout
 *
 * Nothing is written to the disks themselves. Used instead of diskpart, which waits on the
 * Virtual Disk Service and hangs in this PE.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const char *
BusName(STORAGE_BUS_TYPE T)
{
    switch ((int)T) {
    case 1:  return "SCSI";
    case 7:  return "USB";
    case 11: return "SATA";
    case 17: return "NVMe";
    case 19: return "UFS";
    case 21: return "Virtual";
    case 22: return "FileBackedVirtual";
    default: return "other";
    }
}

static void
PrintGuid(const GUID *G)
{
    printf("%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", G->Data1, G->Data2, G->Data3,
           G->Data4[0], G->Data4[1], G->Data4[2], G->Data4[3], G->Data4[4], G->Data4[5], G->Data4[6], G->Data4[7]);
}

static void
Report(HANDLE H, BOOL Online)
{
    BYTE buf[64 * 1024];
    DWORD got;
    STORAGE_PROPERTY_QUERY q;
    STORAGE_BUS_TYPE bus = BusTypeUnknown;
    GET_DISK_ATTRIBUTES attr;
    DRIVE_LAYOUT_INFORMATION_EX *dl;
    DISK_GEOMETRY_EX *geo;
    DWORD i;

    memset(&q, 0, sizeof(q));
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    if (DeviceIoControl(H, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
        bus = d->BusType;
        printf("  device: %s %s, bus %s (%d), removable %d\n",
               d->VendorIdOffset ? (char *)buf + d->VendorIdOffset : "",
               d->ProductIdOffset ? (char *)buf + d->ProductIdOffset : "",
               BusName(d->BusType), (int)d->BusType, d->RemovableMedia);
    } else {
        printf("  device query failed, error %lu\n", GetLastError());
    }

    geo = (DISK_GEOMETRY_EX *)buf;
    if (DeviceIoControl(H, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, buf, sizeof(buf), &got, NULL)) {
        printf("  size %lld bytes, %lu bytes per sector, media type %d\n",
               geo->DiskSize.QuadPart, geo->Geometry.BytesPerSector, (int)geo->Geometry.MediaType);
    } else {
        printf("  geometry failed, error %lu\n", GetLastError());
    }

    memset(&attr, 0, sizeof(attr));
    attr.Version = sizeof(attr);
    if (DeviceIoControl(H, IOCTL_DISK_GET_DISK_ATTRIBUTES, NULL, 0, &attr, sizeof(attr), &got, NULL)) {
        printf("  attributes 0x%llX:%s%s\n", attr.Attributes,
               (attr.Attributes & DISK_ATTRIBUTE_OFFLINE) ? " OFFLINE" : " online",
               (attr.Attributes & DISK_ATTRIBUTE_READ_ONLY) ? " READ-ONLY" : " writable");
    } else {
        printf("  attributes failed, error %lu\n", GetLastError());
    }

    if (Online && (bus == (STORAGE_BUS_TYPE)19 || bus == BusTypeScsi) &&
        (attr.Attributes & (DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY))) {
        SET_DISK_ATTRIBUTES set;

        memset(&set, 0, sizeof(set));
        set.Version = sizeof(set);
        set.Persist = FALSE;
        set.Attributes = 0;
        set.AttributesMask = DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY;
        if (DeviceIoControl(H, IOCTL_DISK_SET_DISK_ATTRIBUTES, &set, sizeof(set), NULL, 0, &got, NULL)) {
            printf("  -> set online and writable (this boot only)\n");
        } else {
            printf("  -> set online failed, error %lu\n", GetLastError());
        }
        if (!DeviceIoControl(H, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &got, NULL)) {
            printf("  -> update properties failed, error %lu\n", GetLastError());
        }
    }

    dl = (DRIVE_LAYOUT_INFORMATION_EX *)buf;
    if (!DeviceIoControl(H, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, buf, sizeof(buf), &got, NULL)) {
        printf("  layout failed, error %lu\n", GetLastError());
        return;
    }
    printf("  layout: style %s, %lu entries\n",
           dl->PartitionStyle == PARTITION_STYLE_GPT ? "GPT" : dl->PartitionStyle == PARTITION_STYLE_MBR ? "MBR" : "RAW",
           dl->PartitionCount);
    if (dl->PartitionStyle == PARTITION_STYLE_GPT) {
        printf("  disk id ");
        PrintGuid(&dl->Gpt.DiskId);
        printf(", usable from %lld, %lld bytes\n", dl->Gpt.StartingUsableOffset.QuadPart, dl->Gpt.UsableLength.QuadPart);
    }
    for (i = 0; i < dl->PartitionCount; i++) {
        PARTITION_INFORMATION_EX *p = &dl->PartitionEntry[i];

        if (p->PartitionLength.QuadPart == 0) {
            continue;
        }
        printf("  #%lu at %lld, %lld MB", p->PartitionNumber, p->StartingOffset.QuadPart,
               p->PartitionLength.QuadPart >> 20);
        if (p->PartitionStyle == PARTITION_STYLE_GPT) {
            printf(", \"%ls\" type ", p->Gpt.Name);
            PrintGuid(&p->Gpt.PartitionType);
            printf(" attr 0x%llX", p->Gpt.Attributes);
        }
        printf("\n");
    }
}

int
main(int argc, char **argv)
{
    BOOL online = argc > 1 && _stricmp(argv[1], "online") == 0;
    WCHAR vol[MAX_PATH], paths[1024];
    HANDLE f;
    int n;
    DWORD san = 0, size = sizeof(san);

    setvbuf(stdout, NULL, _IONBF, 0);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\partmgr\\Parameters", L"SanPolicy",
                     RRF_RT_REG_DWORD, NULL, &san, &size) == ERROR_SUCCESS) {
        printf("[r8sdisk] SAN policy (registry) %lu (1 = online all, 2 = offline shared, 3 = offline all, 4 = offline internal)\n", san);
    } else {
        printf("[r8sdisk] SAN policy not set in the registry (default)\n");
    }

    for (n = 0; n < 8; n++) {
        WCHAR name[64];
        HANDLE h;

        swprintf(name, 64, L"\\\\.\\PhysicalDrive%d", n);
        h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        }
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        printf("[r8sdisk] PhysicalDrive%d\n", n);
        Report(h, online);
        CloseHandle(h);
    }

    if (online) {
        Sleep(3000);
    }
    printf("[r8sdisk] volumes:\n");
    f = FindFirstVolumeW(vol, MAX_PATH);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            DWORD len = 0;
            WCHAR fs[32] = L"", label[64] = L"";
            WCHAR *p;

            paths[0] = 0;
            GetVolumePathNamesForVolumeNameW(vol, paths, 1024, &len);
            GetVolumeInformationW(vol, label, 64, NULL, NULL, NULL, fs, 32);
            printf("  %ls fs %ls label \"%ls\" at", vol, fs[0] ? fs : L"?", label);
            for (p = paths; *p; p += wcslen(p) + 1) {
                printf(" %ls", p);
            }
            printf("\n");
        } while (FindNextVolumeW(f, vol, MAX_PATH));
        FindVolumeClose(f);
    }
    printf("[r8sdisk] done\n");
    return 0;
}
