/* akvirtualcamera, virtual camera for Mac and Windows.
 * Copyright (C) 2020  Gonzalo Exequiel Pedone
 *
 * akvirtualcamera is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * akvirtualcamera is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with akvirtualcamera. If not, see <http://www.gnu.org/licenses/>.
 *
 * Web-Site: http://webcamoid.github.io/
 */

#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#endif

#include "service.h"
#include "PlatformUtils/src/preferences.h"
#include "VCamUtils/src/logger.h"
#include "VCamUtils/src/message.h"
#include "VCamUtils/src/messageserver.h"
#include "VCamUtils/src/servicemsg.h"
#include "VCamUtils/src/videoformat.h"
#include "VCamUtils/src/videoframe.h"

namespace AkVCam
{
    struct Peer
    {
        uint64_t clientId {0};
        uint64_t pid {0};

        Peer(uint64_t clientId=0, uint64_t pid=0):
            clientId(clientId),
            pid(pid)
        {

        }
    };

    struct BroadcastSlot
    {
        Peer broadcaster;
        std::vector<Peer> listeners;
        VideoFrame frame;
        bool frameReady {false};
    };

    typedef std::map<std::string, BroadcastSlot> Broadcasts;

    class ServicePrivate
    {
        public:
            MessageServer m_messageServer;

            // Broadcasting and listen
            Broadcasts m_broadcasts;
            std::condition_variable_any m_frameAvailable;
            std::mutex m_peerMutex;

            ServicePrivate();
            static void removeClientById(void *userData, uint64_t clientId);
            bool clients(uint64_t clientId,
                         const Message &inMessage,
                         Message &outMessage);
            bool broadcast(uint64_t clientId,
                           const Message &inMessage,
                           Message &outMessage);
            bool listen(uint64_t clientId,
                        const Message &inMessage,
                        Message &outMessage);
    };
}

AkVCam::Service::Service()
{
    this->d = new ServicePrivate;
}

AkVCam::Service::~Service()
{
    delete this->d;
}

int AkVCam::Service::run()
{
    AkLogFunction();

    return this->d->m_messageServer.run();
}

void AkVCam::Service::stop()
{
    AkLogFunction();
    this->d->m_messageServer.stop();
}

#define BIND(member) \
    std::bind(&member, \
              this, \
              std::placeholders::_1, \
              std::placeholders::_2, \
              std::placeholders::_3)

AkVCam::ServicePrivate::ServicePrivate()
{
    AkLogFunction();

    this->m_messageServer.setPort(Preferences::servicePort());

    this->m_messageServer.subscribe(AKVCAM_SERVICE_MSG_CLIENTS  , BIND(ServicePrivate::clients)  );
    this->m_messageServer.subscribe(AKVCAM_SERVICE_MSG_BROADCAST, BIND(ServicePrivate::broadcast));
    this->m_messageServer.subscribe(AKVCAM_SERVICE_MSG_LISTEN   , BIND(ServicePrivate::listen)   );

    this->m_messageServer.connectConnectionClosed(this, &ServicePrivate::removeClientById);
}

void AkVCam::ServicePrivate::removeClientById(void *userData,
                                              uint64_t clientId)
{
    AkLogFunction();
    AkLogDebug() << "Removing client: " << clientId << std::endl;
    auto self = reinterpret_cast<ServicePrivate *>(userData);

    self->m_peerMutex.lock();
    std::string removeDevice;

    for (auto &slot: self->m_broadcasts) {
        if (slot.second.broadcaster.clientId == clientId) {
            slot.second.broadcaster = {0, 0};

            if (slot.second.listeners.empty())
                removeDevice = slot.first;

            break;
        } else {
            auto it = std::find_if(slot.second.listeners.begin(),
                                   slot.second.listeners.end(),
                                   [&clientId] (const Peer &peer) -> bool {
                return peer.clientId == clientId;
            });

            if (it != slot.second.listeners.end()) {
                slot.second.listeners.erase(it);

                if (slot.second.broadcaster.pid == 0
                    && slot.second.listeners.empty()) {
                    removeDevice = slot.first;
                }

                break;
            }
        }
    }

    if (!removeDevice.empty())
        self->m_broadcasts.erase(removeDevice);

    self->m_peerMutex.unlock();
}

bool AkVCam::ServicePrivate::clients(uint64_t clientId,
                                     const Message &inMessage,
                                     Message &outMessage)
{
    AkLogFunction();
    UNUSED(clientId);
    MsgClients msgClients(inMessage);
    std::vector<uint64_t> clients;

    this->m_peerMutex.lock();

    for (auto &slot: this->m_broadcasts) {
        if (msgClients.clientType() == MsgClients::ClientType_Any
            && slot.second.broadcaster.pid
            && std::find(clients.begin(),
                         clients.end(),
                         slot.second.broadcaster.pid) == clients.end()) {
            clients.push_back(slot.second.broadcaster.pid);
        }

        for (auto &client: slot.second.listeners)
            if (std::find(clients.begin(),
                          clients.end(),
                          client.pid) == clients.end())
                clients.push_back(client.pid);
    }

    this->m_peerMutex.unlock();
    outMessage = MsgClients(msgClients.clientType(),
                            clients,
                            inMessage.queryId()).toMessage();

    return true;
}

bool AkVCam::ServicePrivate::broadcast(uint64_t clientId,
                                       const Message &inMessage,
                                       Message &outMessage)
{
    AkLogFunction();
    MsgBroadcast msgBroadcast(inMessage);
    MsgStatus status(-1, inMessage.queryId());
    this->m_peerMutex.lock();

    bool isBroadcasting = this->m_broadcasts.count(msgBroadcast.device()) < 1;
    AkLogDebug() << "Device" << msgBroadcast.device() << "is broadcasting?:" << (isBroadcasting? "YES": "NO") << std::endl;

    if (isBroadcasting) {
        AkLogDebug() << "Adding device slot:" << std::endl;
        AkLogDebug() << "    Device ID:" << msgBroadcast.device() << std::endl;
        AkLogDebug() << "    Client ID:" << clientId << std::endl;
        AkLogDebug() << "    Client PID:" << msgBroadcast.pid() << std::endl;

        this->m_broadcasts[msgBroadcast.device()] =
            {{clientId, msgBroadcast.pid()}, {}, {}};
    }

    AkLogDebug() << "Get slot" << std::endl;
    auto &slot = this->m_broadcasts[msgBroadcast.device()];

    if (slot.broadcaster.pid == 0) {
        AkLogDebug() << "Set client as broadcaster" << std::endl;
        slot.broadcaster = {clientId, msgBroadcast.pid()};
    }

    if (slot.broadcaster.pid == msgBroadcast.pid()
        && slot.broadcaster.clientId == clientId) {
        AkLogDebug() << "Save frame" << std::endl;
        slot.frame = msgBroadcast.frame();
        slot.frameReady = true;
        status = MsgStatus(0, inMessage.queryId());
        this->m_frameAvailable.notify_all();
    }

    this->m_peerMutex.unlock();

    AkLogDebug() << "Sending the response" << std::endl;
    outMessage = status.toMessage();

    return status.status() == 0;
}

// ✅ 日志记录函数：记录 Service listen() 的每一步
static void writeServiceListenLog(const std::string &step,
                                   const std::string &info = "",
                                   long long durationUs = -1)
{
    static std::ofstream logFile;
    static std::mutex logMutex;
    static bool initialized = false;
    
    if (!initialized) {
#ifdef _WIN32
        CHAR tempPath[MAX_PATH];
        GetTempPathA(MAX_PATH, tempPath);
        std::string logFilePath = std::string(tempPath) + "AkVCam_Service_Listen_Log.txt";
#else
        const char *tempPath = getenv("TMPDIR");
        if (!tempPath) tempPath = "/tmp";
        std::string logFilePath = std::string(tempPath) + "/AkVCam_Service_Listen_Log.txt";
#endif
        
        logFile.open(logFilePath, std::ios_base::out | std::ios_base::app);
        initialized = true;
    }
    
    if (logFile.is_open()) {
        std::lock_guard<std::mutex> lock(logMutex);
        
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &time_t);
#else
        localtime_r(&time_t, &tm_buf);
#endif
        
        char timeStr[64];
        auto msValue = static_cast<long long>(ms.count());
        std::snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                     tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, msValue);
        
        logFile << timeStr << " - [SERVICE LISTEN] " << step;
        if (!info.empty()) {
            logFile << " | " << info;
        }
        if (durationUs >= 0) {
            if (durationUs < 1000) {
                logFile << " | 耗时: " << durationUs << "μs";
            } else {
                logFile << " | 耗时: " << (durationUs / 1000) << "ms";
            }
        }
        logFile << std::endl;
        logFile.flush();
    }
}

bool AkVCam::ServicePrivate::listen(uint64_t clientId,
                                    const Message &inMessage,
                                    Message &outMessage)
{
    auto functionStartTime = std::chrono::high_resolution_clock::now();
    
    AkLogFunction();
    MsgListen msgListen(inMessage);
    auto deviceId = msgListen.device();
    writeServiceListenLog("1. 函数开始", "设备: " + deviceId + " | ClientID: " + std::to_string(clientId) + " | PID: " + std::to_string(msgListen.pid()));
    
    bool ok = false;

    auto lockMutexStartTime = std::chrono::high_resolution_clock::now();
    this->m_peerMutex.lock();
    auto lockMutexEndTime = std::chrono::high_resolution_clock::now();
    auto lockMutexDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        lockMutexEndTime - lockMutexStartTime).count();
    writeServiceListenLog("2. 获取peerMutex", "", lockMutexDuration);

    auto checkDeviceStartTime = std::chrono::high_resolution_clock::now();
    bool deviceExists = this->m_broadcasts.count(msgListen.device()) >= 1;
    auto checkDeviceEndTime = std::chrono::high_resolution_clock::now();
    auto checkDeviceDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        checkDeviceEndTime - checkDeviceStartTime).count();
    writeServiceListenLog("3. 检查设备是否存在", deviceExists ? "存在" : "不存在", checkDeviceDuration);

    if (!deviceExists) {
        auto createSlotStartTime = std::chrono::high_resolution_clock::now();
        this->m_broadcasts[msgListen.device()] = {};
        auto createSlotEndTime = std::chrono::high_resolution_clock::now();
        auto createSlotDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            createSlotEndTime - createSlotStartTime).count();
        writeServiceListenLog("4. 创建设备slot", "", createSlotDuration);
    }

    auto getSlotStartTime = std::chrono::high_resolution_clock::now();
    auto &slot = this->m_broadcasts[msgListen.device()];
    auto getSlotEndTime = std::chrono::high_resolution_clock::now();
    auto getSlotDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        getSlotEndTime - getSlotStartTime).count();
    writeServiceListenLog("5. 获取slot引用", "", getSlotDuration);
    
    auto addListenerStartTime = std::chrono::high_resolution_clock::now();
    slot.listeners.push_back({clientId, msgListen.pid()});
    auto addListenerEndTime = std::chrono::high_resolution_clock::now();
    auto addListenerDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        addListenerEndTime - addListenerStartTime).count();
    writeServiceListenLog("6. 添加listener", "当前listeners数量: " + std::to_string(slot.listeners.size()), addListenerDuration);

    auto checkFrameReadyStartTime = std::chrono::high_resolution_clock::now();
    bool frameReady = slot.frameReady;
    bool hasBroadcaster = (slot.broadcaster.pid != 0);
    auto checkFrameReadyEndTime = std::chrono::high_resolution_clock::now();
    auto checkFrameReadyDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        checkFrameReadyEndTime - checkFrameReadyStartTime).count();
    writeServiceListenLog("7. 检查frameReady状态", 
                          "frameReady: " + std::string(frameReady ? "是" : "否") + 
                          " | hasBroadcaster: " + std::string(hasBroadcaster ? "是" : "否"),
                          checkFrameReadyDuration);

    // ✅ 在共享内存模式下，broadcast() 可能不会被调用（数据直接写入共享内存）
    // 因此我们不等待 frameReady 标志，而是立即返回，让驱动可以持续请求帧
    // 这样即使 frameReady 为 false，驱动也会收到响应，可以继续请求下一帧
    // 修改：完全移除等待逻辑，立即返回，让驱动可以高频率请求帧
    // 即使 slot.frameReady 为 false，也立即返回，因为数据可能已经通过共享内存传输了
    writeServiceListenLog("8. 跳过等待frameReady", "立即返回（共享内存模式）");

    auto createMessageStartTime = std::chrono::high_resolution_clock::now();
    outMessage = MsgFrameReady(msgListen.device(),
                               slot.frame,
                               slot.broadcaster.pid != 0,
                               inMessage.queryId()).toMessage();
    auto createMessageEndTime = std::chrono::high_resolution_clock::now();
    auto createMessageDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        createMessageEndTime - createMessageStartTime).count();
    writeServiceListenLog("9. 创建MsgFrameReady消息", 
                          "帧大小: " + std::to_string(slot.frame.size()) + " bytes" +
                          " | isActive: " + std::string(slot.broadcaster.pid != 0 ? "是" : "否"),
                          createMessageDuration);
    
    auto resetFrameReadyStartTime = std::chrono::high_resolution_clock::now();
    slot.frameReady = false;
    auto resetFrameReadyEndTime = std::chrono::high_resolution_clock::now();
    auto resetFrameReadyDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        resetFrameReadyEndTime - resetFrameReadyStartTime).count();
    writeServiceListenLog("10. 重置frameReady标志", "", resetFrameReadyDuration);
    
    ok = true;
    
    auto unlockMutexStartTime = std::chrono::high_resolution_clock::now();
    this->m_peerMutex.unlock();
    auto unlockMutexEndTime = std::chrono::high_resolution_clock::now();
    auto unlockMutexDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        unlockMutexEndTime - unlockMutexStartTime).count();
    writeServiceListenLog("11. 释放peerMutex", "", unlockMutexDuration);

    auto functionEndTime = std::chrono::high_resolution_clock::now();
    auto functionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        functionEndTime - functionStartTime).count();
    writeServiceListenLog("12. 函数结束", "总耗时: " + std::to_string(functionDuration) + "ms | 返回: " + std::string(ok ? "true" : "false"));

    return ok;
}
