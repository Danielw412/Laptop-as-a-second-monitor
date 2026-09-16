// Derived from microsoft/Windows-driver-samples video/IndirectDisplay/IddSampleApp/main.cpp.
// Creates the software device that loads LaptopMonitorIdd; the virtual monitor exists while this process runs.

#include <iostream>
#include <vector>

#include <windows.h>
#include <swdevice.h>
#include <conio.h>
#include <wrl.h>

struct CreationContext
{
    HANDLE hEvent;
    HRESULT hrCreateResult;
};

VOID WINAPI
CreationCallback(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT hrCreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId
    )
{
    auto* pCreation = static_cast<CreationContext*>(pContext);

    pCreation->hrCreateResult = hrCreateResult;
    SetEvent(pCreation->hEvent);
    UNREFERENCED_PARAMETER(hSwDevice);
    UNREFERENCED_PARAMETER(pszDeviceInstanceId);
}

int __cdecl main(int argc, wchar_t *argv[])
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    CreationContext creation = { CreateEvent(nullptr, FALSE, FALSE, nullptr), E_PENDING };
    HSWDEVICE hSwDevice;
    SW_DEVICE_CREATE_INFO createInfo = { 0 };
    PCWSTR description = L"Laptop Monitor Virtual Display";

    // These match the Pnp id's in the inf file so OS will load the driver when the device is created
    PCWSTR instanceId = L"LaptopMonitorIdd";
    PCWSTR hardwareIds = L"LaptopMonitorIdd\0\0";
    PCWSTR compatibleIds = L"LaptopMonitorIdd\0\0";

    createInfo.cbSize = sizeof(createInfo);
    createInfo.pszzCompatibleIds = compatibleIds;
    createInfo.pszInstanceId = instanceId;
    createInfo.pszzHardwareIds = hardwareIds;
    createInfo.pszDeviceDescription = description;

    createInfo.CapabilityFlags = SWDeviceCapabilitiesRemovable |
                                 SWDeviceCapabilitiesSilentInstall |
                                 SWDeviceCapabilitiesDriverRequired;

    // Create the device
    HRESULT hr = SwDeviceCreate(L"LaptopMonitorIdd",
                                L"HTREE\\ROOT\\0",
                                &createInfo,
                                0,
                                nullptr,
                                CreationCallback,
                                &creation,
                                &hSwDevice);
    if (FAILED(hr))
    {
        printf("SwDeviceCreate failed with 0x%lx\n", hr);
        return 1;
    }

    // Wait for callback to signal that the device has been created
    printf("Waiting for device to be created....\n");
    DWORD waitResult = WaitForSingleObject(creation.hEvent, 10*1000);
    if (waitResult != WAIT_OBJECT_0)
    {
        printf("Wait for device creation failed\n");
        SwDeviceClose(hSwDevice);
        return 1;
    }
    if (FAILED(creation.hrCreateResult))
    {
        printf("Device creation failed with 0x%lx (is the driver package installed?)\n", creation.hrCreateResult);
        SwDeviceClose(hSwDevice);
        return 1;
    }
    printf("Device created\n\n");

    // Now wait for user to indicate the device should be stopped
    printf("Press 'x' to exit and destroy the software device\n");
    bool bExit = false;
    do
    {
        // Wait for key press
        int key = _getch();

        if (key == 'x' || key == 'X')
        {
            bExit = true;
        }
    }while (!bExit);

    // Stop the device, this will cause the driver to be unloaded
    SwDeviceClose(hSwDevice);

    return 0;
}
