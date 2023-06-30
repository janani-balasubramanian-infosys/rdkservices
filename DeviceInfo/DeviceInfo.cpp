/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "DeviceInfo.h"

#define API_VERSION_NUMBER_MAJOR 1
#define API_VERSION_NUMBER_MINOR 0
#define API_VERSION_NUMBER_PATCH 3

namespace WPEFramework {
namespace {
    static Plugin::Metadata<Plugin::DeviceInfo> metadata(
        // Version (Major, Minor, Patch)
        API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH,
        // Preconditions
        {},
        // Terminations
        {},
        // Controls
        {}
    );
}

namespace Plugin {

    SERVICE_REGISTRATION(DeviceInfo, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

    static Core::ProxyPoolType<Web::JSONBodyType<DeviceInfo::Data>> jsonResponseFactory(4);

    /* virtual */ const string DeviceInfo::Initialize(PluginHost::IShell* service)
    {
        ASSERT(_service == nullptr);
        ASSERT(service != nullptr);

        ASSERT(_subSystem == nullptr);

        _skipURL = static_cast<uint8_t>(service->WebPrefix().length());
        _subSystem = service->SubSystems();
        _service = service;
//        _systemId = Core::SystemInfo::Instance().Id(Core::SystemInfo::Instance().RawDeviceId(), ~0);

        ASSERT(_subSystem != nullptr);

        _deviceInfo = service->Root<Exchange::IDeviceInfo>(_connectionId, 2000, _T("DeviceInfoImplementation"));
        _deviceAudioCapabilityInterface = service->Root<Exchange::IDeviceAudioCapabilities>(_connectionId, 2000, _T("DeviceAudioCapabilities"));
        _deviceVideoCapabilityInterface = service->Root<Exchange::IDeviceVideoCapabilities>(_connectionId, 2000, _T("DeviceVideoCapabilities"));
        _firmwareVersion = service->Root<Exchange::IFirmwareVersion>(_connectionId, 2000, _T("FirmwareVersion"));

        ASSERT(_deviceInfo != nullptr);
        ASSERT(_deviceAudioCapabilities != nullptr);
        ASSERT(_deviceVideoCapabilities != nullptr);
        ASSERT(_firmwareVersion != nullptr);

        // On success return empty, to indicate there is no error text.

        return ((_subSystem != nullptr)
                   && (_deviceInfo != nullptr)
                   && (_deviceAudioCapabilityInterface != nullptr)
                   && (_deviceVideoCapabilityInterface != nullptr)
                   && (_firmwareVersion != nullptr))
            ? EMPTY_STRING
            : _T("Could not retrieve System Information.");
    }

    /* virtual */ void DeviceInfo::Deinitialize(PluginHost::IShell* service)
    {
        ASSERT(_service == service);

        _deviceInfo->Release();
        _deviceAudioCapabilityInterface->Release();
        _deviceVideoCapabilityInterface->Release();
        _firmwareVersion->Release();

        if (_subSystem != nullptr) {
            _subSystem->Release();
            _subSystem = nullptr;
        }

        _service = nullptr;
    }

    /* virtual */ string DeviceInfo::Information() const
    {
        // No additional info to report.
        return (string());
    }

    /* virtual */ void DeviceInfo::Inbound(Web::Request& /* request */)
    {
    }

    /* virtual */ Core::ProxyType<Web::Response> DeviceInfo::Process(const Web::Request& request)
    {
        ASSERT(_skipURL <= request.Path.length());

        Core::ProxyType<Web::Response> result(PluginHost::IFactories::Instance().Response());

        // By default, we assume everything works..
        result->ErrorCode = Web::STATUS_OK;
        result->Message = "OK";

        // <GET> - currently, only the GET command is supported, returning system info
        if (request.Verb == Web::Request::HTTP_GET) {

            Core::ProxyType<Web::JSONBodyType<Data>> response(jsonResponseFactory.Element());

            Core::TextSegmentIterator index(Core::TextFragment(request.Path, _skipURL, static_cast<uint32_t>(request.Path.length()) - _skipURL), false, '/');

            // Always skip the first one, it is an empty part because we start with a '/' if there are more parameters.
            index.Next();

            if (index.Next() == false) {
                AddressInfo(response->Addresses);
                SysInfo(response->SystemInfo);
                SocketPortInfo(response->Sockets);
            } else if (index.Current() == "Adresses") {
                AddressInfo(response->Addresses);
            } else if (index.Current() == "System") {
                SysInfo(response->SystemInfo);
            } else if (index.Current() == "Sockets") {
                SocketPortInfo(response->Sockets);
            }
            // TODO RB: I guess we should do something here to return other info (e.g. time) as well.

            result->ContentType = Web::MIMETypes::MIME_JSON;
            result->Body(Core::proxy_cast<Web::IBody>(response));
        } else {
            result->ErrorCode = Web::STATUS_BAD_REQUEST;
            result->Message = _T("Unsupported request for the [DeviceInfo] service.");
        }

        return result;
    }

    void DeviceInfo::SysInfo(JsonData::DeviceInfo::SysteminfoData& systemInfo) const
    {
        Core::SystemInfo& singleton(Core::SystemInfo::Instance());

        systemInfo.Time = Core::Time::Now().ToRFC1123(true);
        systemInfo.Version = _service->Version() + _T("#") + _subSystem->BuildTreeHash();
        systemInfo.Uptime = singleton.GetUpTime();
        systemInfo.Freeram = singleton.GetFreeRam();
        systemInfo.Totalram = singleton.GetTotalRam();
        systemInfo.Totalswap = singleton.GetTotalSwap();
        systemInfo.Freeswap = singleton.GetFreeSwap();
        systemInfo.Devicename = singleton.GetHostName();
        systemInfo.Cpuload = Core::NumberType<uint32_t>(static_cast<uint32_t>(singleton.GetCpuLoad())).Text();
//        systemInfo.Serialnumber = _systemId;

        auto cpuloadavg = singleton.GetCpuLoadAvg();
        if (cpuloadavg != nullptr) {
            systemInfo.Cpuloadavg.Avg1min = *(cpuloadavg);
            if (++cpuloadavg != nullptr) {
                systemInfo.Cpuloadavg.Avg5min = *(cpuloadavg);
                if (++cpuloadavg != nullptr) {
                    systemInfo.Cpuloadavg.Avg15min = *(cpuloadavg);
                }
            }
        }
    }

    void DeviceInfo::AddressInfo(Core::JSON::ArrayType<JsonData::DeviceInfo::AddressesData>& addressInfo) const
    {
        // Get the point of entry on WPEFramework..
        Core::AdapterIterator interfaces;

        while (interfaces.Next() == true) {

            JsonData::DeviceInfo::AddressesData newElement;
            newElement.Name = interfaces.Name();
            newElement.Mac = interfaces.MACAddress(':');
            JsonData::DeviceInfo::AddressesData& element(addressInfo.Add(newElement));

            // get an interface with a public IP address, then we will have a proper MAC address..
            Core::IPV4AddressIterator selectedNode(interfaces.IPV4Addresses());

            while (selectedNode.Next() == true) {
                Core::JSON::String nodeName;
                nodeName = selectedNode.Address().HostAddress();

                element.Ip.Add(nodeName);
            }
        }
    }

    void DeviceInfo::SocketPortInfo(JsonData::DeviceInfo::SocketinfoData& socketPortInfo) const
    {
        socketPortInfo.Runs = Core::ResourceMonitor::Instance().Runs();
    }

    uint32_t DeviceInfo::AudioOutputs(AudioOutputTypes& audioOutputs) const
    {
        Exchange::IDeviceAudioCapabilities::IAudioOutputIterator* audioIt;

        uint32_t status = _deviceAudioCapabilityInterface->AudioOutputs(audioIt);
        if ((status == Core::ERROR_NONE) && (audioIt != nullptr)) {
            Exchange::IDeviceAudioCapabilities::AudioOutput audioOutput;
            Core::JSON::EnumType<JsonData::DeviceInfo::AudioportType> jsonAudioOutput;
            while (audioIt->Next(audioOutput) == true) {
                jsonAudioOutput = static_cast<JsonData::DeviceInfo::AudioportType>(audioOutput);
                audioOutputs.Add(jsonAudioOutput);
            }
            audioIt->Release();
        }
        return status;
    }

    uint32_t DeviceInfo::AudioCapabilities(const Exchange::IDeviceAudioCapabilities::AudioOutput audioOutput, AudioCapabilityTypes& audioCapabilityTypes) const
    {
        Exchange::IDeviceAudioCapabilities::IAudioCapabilityIterator* audioCapabilityIt = nullptr;
        Exchange::IDeviceAudioCapabilities::AudioCapability audioCapabilty(
            Exchange::IDeviceAudioCapabilities::AudioCapability::AUDIOCAPABILITY_NONE);

        uint32_t status = _deviceAudioCapabilityInterface->AudioCapabilities(audioOutput, audioCapabilityIt);
        if ((status == Core::ERROR_NONE) && (audioCapabilityIt != nullptr)) {
            Core::JSON::EnumType<JsonData::DeviceInfo::AudiocapabilityType> jsonAudioCapability;
            while (audioCapabilityIt->Next(audioCapabilty)) {
                jsonAudioCapability = static_cast<JsonData::DeviceInfo::AudiocapabilityType>(audioCapabilty);
                audioCapabilityTypes.Add(jsonAudioCapability);
            }
            audioCapabilityIt->Release();
        }

        return (status);
    }

    uint32_t DeviceInfo::Ms12Capabilities(const Exchange::IDeviceAudioCapabilities::AudioOutput audioOutput, Ms12CapabilityTypes& ms12CapabilityTypes) const
    {
        Exchange::IDeviceAudioCapabilities::IMS12CapabilityIterator* ms12CapabilityIt = nullptr;
        Exchange::IDeviceAudioCapabilities::MS12Capability ms12Capabilty(
            Exchange::IDeviceAudioCapabilities::MS12Capability::MS12CAPABILITY_NONE);

        uint32_t status = _deviceAudioCapabilityInterface->MS12Capabilities(audioOutput, ms12CapabilityIt);
        if ((status == Core::ERROR_NONE) && (ms12CapabilityIt != nullptr)) {
            Core::JSON::EnumType<JsonData::DeviceInfo::Ms12capabilityType> jsonMs12Capability;
            while (ms12CapabilityIt->Next(ms12Capabilty)) {
                jsonMs12Capability = static_cast<JsonData::DeviceInfo::Ms12capabilityType>(ms12Capabilty);
                ms12CapabilityTypes.Add(jsonMs12Capability);
            }
            ms12CapabilityIt->Release();
        }

        return (status);
    }

    uint32_t DeviceInfo::Ms12Profiles(const Exchange::IDeviceAudioCapabilities::AudioOutput audioOutput, Ms12ProfileTypes& ms12ProfileTypes) const
    {
        Exchange::IDeviceAudioCapabilities::IMS12ProfileIterator* ms12ProfileIt = nullptr;
        Exchange::IDeviceAudioCapabilities::MS12Profile ms12Profile(
            Exchange::IDeviceAudioCapabilities::MS12Profile::MS12PROFILE_NONE);

        uint32_t status = _deviceAudioCapabilityInterface->MS12AudioProfiles(audioOutput, ms12ProfileIt);
        if ((status == Core::ERROR_NONE) && (ms12ProfileIt != nullptr)) {
            Core::JSON::EnumType<JsonData::DeviceInfo::Ms12profileType> jsonMs12Profile;
            while (ms12ProfileIt->Next(ms12Profile)) {
                jsonMs12Profile = static_cast<JsonData::DeviceInfo::Ms12profileType>(ms12Profile);
                ms12ProfileTypes.Add(jsonMs12Profile);
            }
            ms12ProfileIt->Release();
        }

        return (status);
    }

    uint32_t DeviceInfo::VideoOutputs(VideoOutputTypes& videoOutputs) const
    {
        Exchange::IDeviceVideoCapabilities::IVideoOutputIterator* videoIt;

        uint32_t status = _deviceVideoCapabilityInterface->VideoOutputs(videoIt);
        if ((status == Core::ERROR_NONE) && (videoIt != nullptr)) {
            Exchange::IDeviceVideoCapabilities::VideoOutput videoOutput;
            Core::JSON::EnumType<JsonData::DeviceInfo::VideodisplayType> jsonVideoOutput;
            while (videoIt->Next(videoOutput) == true) {
                jsonVideoOutput = static_cast<JsonData::DeviceInfo::VideodisplayType>(videoOutput);
                videoOutputs.Add(jsonVideoOutput);
            }
            videoIt->Release();
        }
        return status;
    }

    uint32_t DeviceInfo::DefaultResolution(const Exchange::IDeviceVideoCapabilities::VideoOutput videoOutput, ScreenResolutionType& screenResolutionType) const
    {
        Exchange::IDeviceVideoCapabilities::ScreenResolution defaultResolution(
            Exchange::IDeviceVideoCapabilities::ScreenResolution::ScreenResolution_Unknown);
        uint32_t status = _deviceVideoCapabilityInterface->DefaultResolution(videoOutput, defaultResolution);

        if (status == Core::ERROR_NONE) {
            screenResolutionType = static_cast<JsonData::DeviceInfo::Output_resolutionType>(defaultResolution);
        }
        return (status);
    }

    uint32_t DeviceInfo::Resolutions(const Exchange::IDeviceVideoCapabilities::VideoOutput videoOutput, ScreenResolutionTypes& screenResolutionTypes) const
    {
        Exchange::IDeviceVideoCapabilities::IScreenResolutionIterator* resolutionIt = nullptr;
        Exchange::IDeviceVideoCapabilities::ScreenResolution resolution(
            Exchange::IDeviceVideoCapabilities::ScreenResolution::ScreenResolution_Unknown);

        uint32_t status = _deviceVideoCapabilityInterface->Resolutions(videoOutput, resolutionIt);
        if ((status == Core::ERROR_NONE) && (resolutionIt != nullptr)) {
            Core::JSON::EnumType<JsonData::DeviceInfo::Output_resolutionType> jsonResolution;
            while (resolutionIt->Next(resolution)) {
                jsonResolution = static_cast<JsonData::DeviceInfo::Output_resolutionType>(resolution);
                screenResolutionTypes.Add(jsonResolution);
            }
            resolutionIt->Release();
        }
        return (status);
    }

    uint32_t DeviceInfo::Hdcp(const Exchange::IDeviceVideoCapabilities::VideoOutput videoOutput, CopyProtectionType& copyProtectionType) const
    {
        Exchange::IDeviceVideoCapabilities::CopyProtection hdcp(
            Exchange::IDeviceVideoCapabilities::CopyProtection::HDCP_UNAVAILABLE);

        uint32_t status = _deviceVideoCapabilityInterface->Hdcp(videoOutput, hdcp);
        if (status == Core::ERROR_NONE) {
            copyProtectionType = static_cast<JsonData::DeviceInfo::CopyprotectionType>(hdcp);
        }
        return (status);
    }

} // namespace Plugin
} // namespace WPEFramework
