#include "audio/AudioDevices.h"

#include <windows.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <initguid.h>  // define as PROPERTYKEYs abaixo neste arquivo
#include <functiondiscoverykeys_devpkey.h>

std::vector<AudioDeviceEntry> listCaptureDevices() {
    std::vector<AudioDeviceEntry> devices;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return devices;

    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) return devices;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        AudioDeviceEntry entry;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            entry.id = id;
            CoTaskMemFree(id);
        }

        Microsoft::WRL::ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
                entry.name = name.pwszVal;
            }
            PropVariantClear(&name);
        }
        if (entry.name.empty()) entry.name = L"Microfone " + std::to_wstring(i + 1);
        if (!entry.id.empty()) devices.push_back(entry);
    }
    return devices;
}
