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

namespace WPEFramework {
namespace Plugin {

    SERVICE_REGISTRATION(DeviceInfo, 1, 0);

    static Core::ProxyPoolType<Web::JSONBodyType<DeviceInfo::Data>> jsonResponseFactory(4);

    /* virtual */ const string DeviceInfo::Initialize(PluginHost::IShell* service)
    {
        ASSERT(_service == nullptr);
        ASSERT(_subSystem == nullptr);
        ASSERT(_implementation == nullptr);
        ASSERT(service != nullptr);
        ASSERT(_connectionId == 0);

        string message(EMPTY_STRING);
        Config config;
        config.FromString(service->ConfigLine());
        _skipURL = static_cast<uint8_t>(service->WebPrefix().length());
        _service = service;
        _service->AddRef();
        _service->Register(&_notification);
        _subSystem = service->SubSystems();

        if (_subSystem == nullptr) {
            message = _T("DeviceInfo Susbsystem could not be obtained");
        } else {
            _subSystem->AddRef();
            _subSystem->Register(&_notification);

            _implementation = _service->Root<Exchange::IDeviceCapabilities>(_connectionId, 2000, _T("DeviceInfoImplementation"));

            if (_implementation == nullptr) {
                message = _T("DeviceInfo could not be instantiated");
                SYSLOG(Logging::Startup, (_T("DeviceInfo could not be instantiated")));
            } else {
                RegisterAll();
                _implementation->Configure(_service);
                _deviceMetadataInterface = _implementation->QueryInterface<Exchange::IDeviceMetadata>();
                if(_deviceMetadataInterface == nullptr) {
                    message = _T("DeviceInfo MetaData Interface could not be instantiated");
                }
            }
        }

        if(message.length() != 0) {
            Deinitialize(service);
        }

        // On success return empty, to indicate there is no error text.
        return message;
    }

    /* virtual */ void DeviceInfo::Deinitialize(PluginHost::IShell* service VARIABLE_IS_NOT_USED)
    {
        ASSERT(_service == service);

        _service->Unregister(&_notification);

        if(_subSystem != nullptr) {
            _subSystem->Unregister(&_notification);
            _subSystem->Release();
            _subSystem = nullptr;
        }

        if(_connectionId != 0){
            ASSERT(_implementation != nullptr);
            UnregisterAll();
            RPC::IRemoteConnection* connection(_service->RemoteConnection(_connectionId));

            if(_deviceMetadataInterface != nullptr){
                _deviceMetadataInterface->Release();
                _deviceMetadataInterface = nullptr;
            }

            VARIABLE_IS_NOT_USED uint32_t result = _implementation->Release();
            _implementation = nullptr;
            // It should have been the last reference we are releasing,
            // so it should endup in a DESTRUCTION_SUCCEEDED, if not we
            // are leaking...
            ASSERT(result == Core::ERROR_DESTRUCTION_SUCCEEDED);
             // The process can disappear in the meantime...
            if (connection != nullptr) {
                // But if it did not dissapear in the meantime, forcefully terminate it. Shoot to kill :-)
                connection->Terminate();
                connection->Release();
            }
        }

        _service->Release();
        _service = nullptr;
        _connectionId = 0;
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
            result->Body(Core::ProxyType<Web::IBody>(response));
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
        systemInfo.Devicename = singleton.GetHostName();
        systemInfo.Cpuload = Core::NumberType<uint32_t>(static_cast<uint32_t>(singleton.GetCpuLoad())).Text();

        _adminLock.Lock();
        systemInfo.Serialnumber = _deviceId;
        _adminLock.Unlock();
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

    void DeviceInfo::CapabilitiesInfo(JsonData::DeviceInfo::CapabilitiesData& response) const
    {
        ASSERT(_implementation != nullptr);

        bool supportsHdr = false;
        if (_implementation->HDR(supportsHdr) == Core::ERROR_NONE) {
            response.Hdr = supportsHdr;
        }

        bool supportsAtmos = false;
        if (_implementation->Atmos(supportsAtmos) == Core::ERROR_NONE) {
            response.Atmos = supportsAtmos;
        }

        bool supportsCec = false;
        if (_implementation->CEC(supportsCec) == Core::ERROR_NONE) {
            response.Cec = supportsCec;
        }

        Exchange::IDeviceCapabilities::CopyProtection hdcp(Exchange::IDeviceCapabilities::CopyProtection::HDCP_UNAVAILABLE);
        if (_implementation->HDCP(hdcp) == Core::ERROR_NONE) {
            response.Hdcp = static_cast<JsonData::DeviceInfo::CapabilitiesData::Copy_protectionType>(hdcp);
        }

        Exchange::IDeviceCapabilities::IAudioOutputIterator* audioIt = nullptr;
        Exchange::IDeviceCapabilities::AudioOutput audio(Exchange::IDeviceCapabilities::AudioOutput::AUDIO_OTHER);
        Core::JSON::EnumType<JsonData::DeviceInfo::CapabilitiesData::Audio_outputType> jsonAudio;
        if (_implementation->AudioOutputs(audioIt) == Core::ERROR_NONE && audioIt != nullptr) {
            while (audioIt->Next(audio)) {
                response.Audio_outputs.Add(jsonAudio = static_cast<JsonData::DeviceInfo::CapabilitiesData::Audio_outputType>(audio));
            }
        }

        Exchange::IDeviceCapabilities::IVideoOutputIterator* videoIt = nullptr;
        Exchange::IDeviceCapabilities::VideoOutput video(Exchange::IDeviceCapabilities::VideoOutput::VIDEO_OTHER);
        Core::JSON::EnumType<JsonData::DeviceInfo::CapabilitiesData::Video_outputType> jsonVideo;
        if (_implementation->VideoOutputs(videoIt) == Core::ERROR_NONE && videoIt != nullptr) {
            while (videoIt->Next(video)) {
                response.Video_outputs.Add(jsonVideo = static_cast<JsonData::DeviceInfo::CapabilitiesData::Video_outputType>(video));
            }
        }

        Exchange::IDeviceCapabilities::IOutputResolutionIterator* resolutionIt = nullptr;
        Exchange::IDeviceCapabilities::OutputResolution resolution(Exchange::IDeviceCapabilities::OutputResolution::RESOLUTION_UNKNOWN);
        Core::JSON::EnumType<JsonData::DeviceInfo::CapabilitiesData::Output_resolutionType> jsonResolution;
        if (_implementation->Resolutions(resolutionIt) == Core::ERROR_NONE && resolutionIt != nullptr) {
            while (resolutionIt->Next(resolution)) {
                response.Output_resolutions.Add(jsonResolution = static_cast<JsonData::DeviceInfo::CapabilitiesData::Output_resolutionType>(resolution));
            }
        }
    }

    void DeviceInfo::MetadataInfo(JsonData::DeviceInfo::MetadataData& metadatainfo) const
    {
        ASSERT(_deviceMetadataInterface != nullptr);
        string localresult ;

        if (_deviceMetadataInterface->ModelName(localresult) == Core::ERROR_NONE) {
            metadatainfo.ModelName = localresult;
        }

        uint16_t year = 0;
        if (_deviceMetadataInterface->ModelYear(year) == Core::ERROR_NONE) {
            metadatainfo.ModelYear = year;
        }

        if (_deviceMetadataInterface->FriendlyName(localresult) == Core::ERROR_NONE) {
            metadatainfo.FriendlyName = localresult;
        }

        if (_deviceMetadataInterface->SystemIntegratorName(localresult) == Core::ERROR_NONE) {
            metadatainfo.SystemIntegratorName = localresult;
        }

        if (_deviceMetadataInterface->PlatformName(localresult) == Core::ERROR_NONE) {
            metadatainfo.PlatformName = localresult;
        }
    }

    void DeviceInfo::UpdateDeviceIdentifier()
    {
        if ((_subSystem != nullptr) &&
            (_subSystem->IsActive(PluginHost::ISubSystem::IDENTIFIER) == true)) {
            string deviceId = GetDeviceId();
            _adminLock.Lock();
            _deviceId = deviceId;
            _adminLock.Unlock();
        }
    }

    string DeviceInfo::GetDeviceId() const
    {
        string deviceId;
        const PluginHost::ISubSystem::IIdentifier* identifier(_subSystem->Get<PluginHost::ISubSystem::IIdentifier>());
        if (identifier != nullptr) {
            uint8_t buffer[64];

            buffer[0] = static_cast<const PluginHost::ISubSystem::IIdentifier*>(identifier)
                        ->Identifier(sizeof(buffer) - 1, &(buffer[1]));

            if (buffer[0] != 0) {
                deviceId = Core::SystemInfo::Instance().Id(buffer, ~0);
            }

            identifier->Release();
        }
        return deviceId;
    }

    void DeviceInfo::Deactivated(RPC::IRemoteConnection* connection)
    {
        if (_connectionId == connection->Id()) {

            ASSERT(_service != nullptr);
            Core::IWorkerPool::Instance().Submit(PluginHost::IShell::Job::Create(_service, PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
        }
    }
} // namespace Plugin
} // namespace WPEFramework
