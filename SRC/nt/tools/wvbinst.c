/* wvbinst - test helper: install WINVBE.INF for the display adapter
   (Windows 2000/XP: UpdateDriverForPlugAndPlayDevices) */
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <stdio.h>
static const GUID DISP = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
int main(int argc, char **argv)
{
    char inf[MAX_PATH], hw[1024];
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    BOOL reboot = FALSE;
    DWORD i;
    GetFullPathNameA(argc > 1 ? argv[1] : "WINVBE.INF", MAX_PATH, inf, NULL);
    h = SetupDiGetClassDevsA(&DISP, NULL, NULL, DIGCF_PRESENT);
    d.cbSize = sizeof(d);
    for (i = 0; SetupDiEnumDeviceInfo(h, i, &d); i++) {
        if (!SetupDiGetDeviceRegistryPropertyA(h, &d, SPDRP_HARDWAREID, NULL, (BYTE *)hw, sizeof(hw), NULL)) continue;
        printf("device %s\n", hw);
        if (UpdateDriverForPlugAndPlayDevicesA(NULL, hw, inf, INSTALLFLAG_FORCE, &reboot))
            printf("installed, reboot=%d\n", reboot);
        else
            printf("failed %lu\n", GetLastError());
        break;
    }
    return 0;
}
