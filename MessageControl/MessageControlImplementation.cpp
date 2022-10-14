/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2022 RDK Management
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

#include "MessageOutput.h"
#include "Module.h"
#include <functional>

namespace WPEFramework {

namespace {

    class MessageSettings : public Core::Messaging::MessageUnit::Settings {
    private:
        MessageSettings() {
            Core::Messaging::MessageUnit::Settings::Load();
        };

    public:
        MessageSettings(const MessageSettings&) = delete;
        MessageSettings& operator= (const MessageSettings&) = delete;

        static MessageSettings& Instance() {
            static MessageSettings singleton;
            return (singleton);
        }
    };
}

namespace Plugin {

    class MessageControlImplementation : public Exchange::IMessageControl, public Exchange::IMessageControl::ICollect {
    private:
        class WorkerThread : public Core::Thread {
        public:
            WorkerThread() = delete;
            WorkerThread(const WorkerThread&) = delete;
            WorkerThread& operator= (const WorkerThread&) = delete;

            WorkerThread(MessageControlImplementation& parent)
                : Core::Thread()
                , _parent(parent)
            {
            }
            ~WorkerThread() override = default;

        private:
            uint32_t Worker() override
            {
                _parent.Dispatch();

                return (Core::infinite);
            }

        private:
            MessageControlImplementation& _parent;
        };

    public:
        MessageControlImplementation(const MessageControlImplementation&) = delete;
        MessageControlImplementation& operator=(const MessageControlImplementation&) = delete;

        MessageControlImplementation()
            : _adminLock()
            , _callback(nullptr)
            , _dispatcherIdentifier(MessageSettings::Instance().Identifier())
            , _dispatcherBasePath(MessageSettings::Instance().BasePath())
            , _worker(*this)
            , _client(_dispatcherIdentifier, _dispatcherBasePath, MessageSettings::Instance().SocketPort())
            , _factory()
        {
        }

        ~MessageControlImplementation() override
        {
            if (_callback != nullptr) {
                _worker.Stop();
                _client.SkipWaiting();
                _worker.Wait(Core::Thread::STOPPED, Core::infinite);

                _client.ClearInstances();

                _callback->Release();
            }
        }

    public:
        uint32_t Configure(Exchange::IMessageControl::ICollect::ICallback* callback) override
        {
            ASSERT(callback != nullptr);

            _adminLock.Lock();

            ASSERT(_callback == nullptr);

            if (callback != nullptr) {
                _callback = callback;
                _callback->AddRef();

                _client.AddInstance(0);
                _client.AddFactory(Core::Messaging::MessageType::TRACING, &_factory);
                _client.AddFactory(Core::Messaging::MessageType::LOGGING, &_factory);

                _worker.Run();
            }

            _adminLock.Unlock();

            return (Core::ERROR_NONE);
        }

        uint32_t Attach(const uint32_t id) override
        {
            _adminLock.Lock();
            _client.AddInstance(id);
            _adminLock.Unlock();

            return (Core::ERROR_NONE);
        }

        uint32_t Detach(const uint32_t id) override
        {
            _adminLock.Lock();
            _client.RemoveInstance(id);
            _adminLock.Unlock();

            return (Core::ERROR_NONE);
        }

        uint32_t Enable(const messagetype type, const string& category, const string& module, const bool enabled) override
        {
            _client.Enable({ToMessageType(type), category, module}, enabled);

            return (Core::ERROR_NONE);
        }

        uint32_t Controls(Exchange::IMessageControl::IControlIterator*& controls) const override
        {
            std::list<Exchange::IMessageControl::Control> list;
            Core::Messaging::MessageUnit::Iterator index;

            _client.Controls(index);

            while (index.Next() == true) {
                list.push_back( { ToMessageType(index.Type()), index.Category(), index.Module(), index.Enabled() } );
            }

            using Implementation = RPC::IteratorType<Exchange::IMessageControl::IControlIterator>;
            controls = Core::Service<Implementation>::Create<Exchange::IMessageControl::IControlIterator>(list);

            return (Core::ERROR_NONE);
        }

    public:
        BEGIN_INTERFACE_MAP(MessageControlImplementation)
        INTERFACE_ENTRY(Exchange::IMessageControl)
        INTERFACE_ENTRY(Exchange::IMessageControl::ICollect)
        END_INTERFACE_MAP

    private:
        void Dispatch()
        {
            _client.WaitForUpdates(Core::infinite);

            _client.PopMessagesAndCall([this](const Core::Messaging::Information& info, const Core::ProxyType<Core::Messaging::IEvent>& message) {
                ASSERT(_callback != nullptr);

                // Turn data into piecies to trasfer over the wire
                _callback->Message(ToMessageType(info.MessageMetaData().Type()),
                    info.MessageMetaData().Category(),
                    info.MessageMetaData().Module(),
                    info.FileName(),
                    info.LineNumber(),
                    info.ClassName(),
                    info.TimeStamp(),
                    message->Data());
            });
        }

    private:
        Core::CriticalSection _adminLock;
        IMessageControl::ICollect::ICallback* _callback;

        const string _dispatcherIdentifier;
        const string _dispatcherBasePath;

        WorkerThread _worker;
        Messaging::MessageClient _client;
        Messaging::TraceFactory _factory;
    };

    SERVICE_REGISTRATION(MessageControlImplementation, 1, 0);

} // namespace Plugin
}
