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
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <thread>

#include "PlatformUtils/src/preferences.h"
#include "PlatformUtils/src/utils.h"
#include "VCamUtils/src/ipcbridge.h"
#include "VCamUtils/src/logger.h"
#include "VCamUtils/src/message.h"
#include "VCamUtils/src/messageclient.h"
#include "VCamUtils/src/servicemsg.h"
#include "VCamUtils/src/sharedmemory.h"
#include "VCamUtils/src/timer.h"
#include "VCamUtils/src/utils.h"
#include "VCamUtils/src/videoformat.h"
#include "VCamUtils/src/videoframe.h"

#define AKVCAM_BIND_FUNC(member) \
    std::bind(&member, this, std::placeholders::_1)

#define AKVCAM_BIND_HACK_FUNC(member) \
    std::bind(&member, this, std::placeholders::_1)

namespace AkVCam
{
    using RegisterServerFunc = HRESULT (WINAPI *)();
    
    // ✅ 自定义日志文件写入函数（驱动读取）
    static void writeFrameReadLog(const std::string &deviceId, 
                                   long long readDuration, 
                                   size_t dataSize)
    {
        static std::ofstream logFile;
        static std::mutex logMutex;
        static bool initialized = false;
        static std::map<std::string, std::chrono::high_resolution_clock::time_point> lastReadTime;
        
        if (!initialized) {
            // 自定义日志文件路径：Windows 临时目录下的 AkVCam_Frame_Read_Log.txt
            std::string logFilePath = tempPath() + "AkVCam_Frame_Read_Log.txt";
            
            logFile.open(logFilePath, std::ios_base::out | std::ios_base::app);
            initialized = true;
        }
        
        if (logFile.is_open()) {
            std::lock_guard<std::mutex> lock(logMutex);
            
            // 获取当前时间戳
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
            
            std::tm tm_buf;
            localtime_s(&tm_buf, &time_t);
            
            char timeStr[64];
            auto msValue = static_cast<long long>(ms.count());
            std::snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                         tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                         tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, msValue);
            
            // 计算距上次读取的间隔
            auto nowHighRes = std::chrono::high_resolution_clock::now();
            long long intervalMs = 0;
            if (lastReadTime.find(deviceId) != lastReadTime.end()) {
                auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
                    nowHighRes - lastReadTime[deviceId]);
                intervalMs = interval.count();
            }
            lastReadTime[deviceId] = nowHighRes;
            
            logFile << timeStr << " - [FRAME READ] Device: " << deviceId
                   << " | 读取耗时: " << readDuration << "ms"
                   << " | 距上次读取: " << intervalMs << "ms"
                   << " | 数据大小: " << dataSize << " bytes"
                   << std::endl;
            logFile.flush();
        }
    }
    
    // ✅ 自定义日志文件写入函数（共享内存写入）
    static void writeSharedMemoryWriteLog(const std::string &deviceId,
                                          long long lockDuration,
                                          bool success,
                                          size_t dataSize)
    {
        static std::ofstream logFile;
        static std::mutex logMutex;
        static bool initialized = false;
        static std::map<std::string, std::chrono::high_resolution_clock::time_point> lastWriteTime;
        
        if (!initialized) {
            // 自定义日志文件路径：Windows 临时目录下的 AkVCam_SharedMemory_Write_Log.txt
            std::string logFilePath = tempPath() + "AkVCam_SharedMemory_Write_Log.txt";
            
            logFile.open(logFilePath, std::ios_base::out | std::ios_base::app);
            initialized = true;
        }
        
        if (logFile.is_open()) {
            std::lock_guard<std::mutex> lock(logMutex);
            
            // 获取当前时间戳
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
            
            std::tm tm_buf;
            localtime_s(&tm_buf, &time_t);
            
            char timeStr[64];
            auto msValue = static_cast<long long>(ms.count());
            std::snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                         tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                         tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, msValue);
            
            // 计算距上次写入的间隔
            auto nowHighRes = std::chrono::high_resolution_clock::now();
            long long intervalMs = 0;
            if (lastWriteTime.find(deviceId) != lastWriteTime.end()) {
                auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
                    nowHighRes - lastWriteTime[deviceId]);
                intervalMs = interval.count();
            }
            lastWriteTime[deviceId] = nowHighRes;
            
            logFile << timeStr << " - [SHARED MEMORY WRITE] Device: " << deviceId
                   << " | 等待 mutex 耗时: " << lockDuration << "ms"
                   << " | 结果: " << (success ? "成功" : "超时/失败")
                   << " | 距上次写入: " << intervalMs << "ms"
                   << " | 数据大小: " << dataSize << " bytes"
                   << std::endl;
            logFile.flush();
        }
    }

    class Hack
    {
        public:
            using HackFunc = std::function<int (const std::vector<std::string> &args)>;

            std::string name;
            std::string description;
            bool isSafe {false};
            bool needsRoot {false};
            HackFunc func;

            Hack();
            Hack(const std::string &name,
                 const std::string &description,
                 bool isSafe,
                 bool needsRoot,
                 const HackFunc &func);
            Hack(const Hack &other);
            Hack &operator =(const Hack &other);
    };

    struct BroadcastSlot
    {
        IpcBridge::StreamType type;
        std::future<bool> messageFuture;
        VideoFrame frame;
        std::condition_variable_any frameAvailable;
        std::mutex frameMutex;
        SharedMemory sharedMemory;
        void *sharedMemoryBuffer {nullptr};  // ✅ 保存 buffer 指针，用于无锁覆盖写入
        bool available {false};
        bool run {false};

        BroadcastSlot()
        {
        }

        BroadcastSlot(std::future<bool> &messageFuture)
        {
        }

        BroadcastSlot(const BroadcastSlot &other)
        {

        }

        BroadcastSlot &operator =(const BroadcastSlot &other)
        {
            UNUSED(other);

            return *this;
        }
    };

    struct SharedFrame
    {
        PixelFormat format;
        int width;
        int height;
        uint8_t data[1];
    };

    struct DirectModeStatus
    {
        bool directMode {false};
        VideoFormat format;

        DirectModeStatus()
        {
        }

        DirectModeStatus(const std::string &deviceId)
        {
            auto cameraId = Preferences::cameraFromId(deviceId);

            if (cameraId >= 0
                && Preferences::cameraDirectMode(size_t(cameraId))) {
                this->directMode = true;
                this->format = Preferences::cameraFormat(size_t(cameraId), 0);
            }
        }

        DirectModeStatus(const DirectModeStatus &other):
            directMode(other.directMode),
            format(other.format)
        {

        }

        DirectModeStatus &operator =(const DirectModeStatus &other)
        {
            if (this != &other) {
                this->directMode = other.directMode;
                this->format = other.format;
            }

            return *this;
        }

        inline bool isValid(const VideoFormat &format) const
        {
            return !this->directMode || format.isSameFormat(this->format);
        }
    };

    class IpcBridgePrivate
    {
        public:
            IpcBridge *self;
            MessageClient m_messageClient;
            std::map<std::string, BroadcastSlot> m_broadcasts;
            std::map<std::string, std::map<std::string, int>> m_controlValues;
            std::map<std::string, DirectModeStatus> m_directModeStatus;
            std::vector<std::string> m_devices;
            std::string m_picture;
            std::mutex m_broadcastsMutex;
            std::mutex m_statusMutex;
            Timer m_messagesTimer;
            int m_logLevel {-1};
            DataMode m_dataMode {DataMode_SharedMemory};
            size_t m_pageSize {0};

            explicit IpcBridgePrivate(IpcBridge *self);
            ~IpcBridgePrivate();

            void updateDevices();
            bool isServiceRunning();
            bool isMFServiceRunning();
            bool launchService();
            inline const std::vector<DeviceControl> &controls() const;

            // Message handling methods
            bool frameRequired(const std::string &deviceId, Message &message);
            bool frameReady(const Message &message);
            static void checkStatus(void *userData);

            // Utility methods
            bool isRoot() const;

            // Hacks
            const std::vector<Hack> &hacks();
    };
}

AkVCam::IpcBridge::IpcBridge()
{
    AkLogFunction();
    this->d = new IpcBridgePrivate(this);
}

AkVCam::IpcBridge::~IpcBridge()
{
    AkLogFunction();
    AkLogDebug() << "Stopping the devices:" << std::endl;

    for (auto &device: this->devices())
        this->deviceStop(device);

    delete this->d;
}

std::string AkVCam::IpcBridge::picture() const
{
    return this->d->m_picture;
}

void AkVCam::IpcBridge::setPicture(const std::string &picture)
{
    AkLogFunction();
    this->d->m_picture = picture;
    Preferences::setPicture(picture);
}

int AkVCam::IpcBridge::logLevel() const
{
    return this->d->m_logLevel;
}

void AkVCam::IpcBridge::setLogLevel(int logLevel)
{
    AkLogFunction();
    this->d->m_logLevel = logLevel;
    Preferences::setLogLevel(logLevel);
    Logger::setLogLevel(logLevel);
}

AkVCam::DataMode AkVCam::IpcBridge::dataMode()
{
    return this->d->m_dataMode;
}

void AkVCam::IpcBridge::setDataMode(DataMode dataMode)
{
    AkLogFunction();
    this->d->m_dataMode = dataMode;
    Preferences::setDataMode(dataMode);
}

size_t AkVCam::IpcBridge::pageSize()
{
    return this->d->m_pageSize;
}

void AkVCam::IpcBridge::setPageSize(size_t pageSize)
{
    AkLogFunction();
    this->d->m_pageSize = pageSize;
    Preferences::setPageSize(pageSize);
}

void AkVCam::IpcBridge::stopNotifications()
{
    AkLogFunction();
    this->d->m_messagesTimer.stop();
}

std::vector<std::string> AkVCam::IpcBridge::devices() const
{
    return this->d->m_devices;
}

std::string AkVCam::IpcBridge::description(const std::string &deviceId) const
{
    AkLogFunction();
    auto cameraIndex = Preferences::cameraFromId(deviceId);

    if (cameraIndex < 0)
        return {};

    return Preferences::cameraDescription(size_t(cameraIndex));
}

void AkVCam::IpcBridge::setDescription(const std::string &deviceId,
                                       const std::string &description)
{
    AkLogFunction();
    auto cameraIndex = Preferences::cameraFromId(deviceId);

    if (cameraIndex >= 0)
        Preferences::cameraSetDescription(size_t(cameraIndex), description);
}

std::vector<AkVCam::PixelFormat> AkVCam::IpcBridge::supportedPixelFormats(StreamType type) const
{
    if (type == StreamType_Input)
        return VideoFormat::supportedPixelFormats();

    return {
        PixelFormat_bgrx,
        PixelFormat_rgb24,
        PixelFormat_uyvy422,
        PixelFormat_yuyv422,
        PixelFormat_nv12
    };
}

AkVCam::PixelFormat AkVCam::IpcBridge::defaultPixelFormat(StreamType type) const
{
    return type == StreamType_Input?
                PixelFormat_rgb24:
                PixelFormat_yuyv422;
}

std::vector<AkVCam::VideoFormat> AkVCam::IpcBridge::formats(const std::string &deviceId) const
{
    AkLogFunction();
    auto cameraIndex = Preferences::cameraFromId(deviceId);

    if (cameraIndex < 0)
        return {};

    return Preferences::cameraFormats(size_t(cameraIndex));
}

void AkVCam::IpcBridge::setFormats(const std::string &deviceId,
                                   const std::vector<VideoFormat> &formats)
{
    AkLogFunction();
    auto cameraIndex = Preferences::cameraFromId(deviceId);

    if (cameraIndex >= 0)
        Preferences::cameraSetFormats(size_t(cameraIndex), formats);
}

std::vector<AkVCam::DeviceControl> AkVCam::IpcBridge::controls(const std::string &deviceId)
{
    AkLogFunction();
    auto cameraIndex = Preferences::cameraFromId(deviceId);

    if (cameraIndex < 0)
        return {};

    std::vector<DeviceControl> controls;

    for (auto &control: this->d->controls()) {
        controls.push_back(control);
        controls.back().value =
                Preferences::cameraControlValue(size_t(cameraIndex), control.id);
    }

    return controls;
}

void AkVCam::IpcBridge::setControls(const std::string &deviceId,
                                    const std::map<std::string, int> &controls)
{
    AkLogFunction();
    auto cameraIndex = Preferences::cameraFromId(deviceId);

    if (cameraIndex < 0)
        return;

    for (auto &control: this->d->controls()) {
        auto oldValue =
                Preferences::cameraControlValue(size_t(cameraIndex),
                                                control.id);

        if (controls.count(control.id)) {
            auto newValue = controls.at(control.id);

            if (newValue != oldValue)
                Preferences::cameraSetControlValue(size_t(cameraIndex),
                                                   control.id,
                                                   newValue);
        }
    }
}

std::vector<uint64_t> AkVCam::IpcBridge::clientsPids() const
{
    AkLogFunction();

    Message msgClients;

    if (!this->d->m_messageClient.send(MsgClients(MsgClients::ClientType_VCams).toMessage(), msgClients))
        return {};

    auto clients = MsgClients(msgClients).clients();
    auto it = std::find(clients.begin(), clients.end(), currentPid());

    if (it != clients.end())
        clients.erase(it);

    return clients;
}

std::string AkVCam::IpcBridge::clientExe(uint64_t pid) const
{
    return exePath(pid);
}

std::string AkVCam::IpcBridge::addDevice(const std::string &description,
                                         const std::string &deviceId)
{
    AkLogFunction();
    auto device = Preferences::addDevice(description, deviceId);
    this->updateDevices();

    return device;
}

void AkVCam::IpcBridge::removeDevice(const std::string &deviceId)
{
    AkLogFunction();
    Preferences::removeCamera(deviceId);
    this->updateDevices();
}

void AkVCam::IpcBridge::addFormat(const std::string &deviceId,
                                  const VideoFormat &format,
                                  int index)
{
    AkLogFunction();
    auto cameraIndex = Preferences::cameraFromId(deviceId);

    if (cameraIndex >= 0)
        Preferences::cameraAddFormat(size_t(cameraIndex),
                                     format,
                                     index);
}

void AkVCam::IpcBridge::removeFormat(const std::string &deviceId, int index)
{
    AkLogFunction();
    auto cameraIndex = Preferences::cameraFromId(deviceId);

    if (cameraIndex >= 0)
        Preferences::cameraRemoveFormat(size_t(cameraIndex),
                                        index);
}

void AkVCam::IpcBridge::updateDevices()
{
    AkLogFunction();
    std::string pluginPath = supportsMediaFoundationVCam()?
                                 locateMFPluginPath():
                                 locatePluginPath();
    AkLogDebug() << "Plugin binary: " << pluginPath << std::endl;

    if (!fileExists(pluginPath)) {
        AkLogError() << "Plugin binary not found: " << pluginPath << std::endl;

        return;
    }

    if (auto hmodule = LoadLibraryA(pluginPath.c_str())) {
        auto registerServer =
                RegisterServerFunc(GetProcAddress(hmodule, "DllRegisterServer"));

        if (registerServer) {
            AkLogDebug() << "Registering server" << std::endl;
            auto result = (*registerServer)();
            AkLogDebug() << "Server registered with code " << result << std::endl;
            auto lockFileName = tempPath() + "\\akvcam_update.lck";

            if (!fileExists(lockFileName)) {
                std::ofstream lockFile;
                lockFile.open(lockFileName);
                lockFile << std::endl;
                lockFile.close();
                auto altManager = locateAltManagerPath();

                if (!altManager.empty())
                    exec({altManager, "update"});

                remove(lockFileName.c_str());
            }
        } else {
            AkLogError() << "Can't locate DllRegisterServer function." << std::endl;
        }

        FreeLibrary(hmodule);
    } else {
        AkLogError() << "Error loading plugin binary: " << pluginPath << std::endl;
    }
}

bool AkVCam::IpcBridge::deviceStart(StreamType type,
                                    const std::string &deviceId)
{
    AkLogFunction();
    AkLogDebug() << "Starting device: "
                 << deviceId
                 << " with type: "
                 << (type == StreamType_Input? "Input": "Output")
                 << std::endl;

    this->d->m_broadcastsMutex.lock();

    if (this->d->m_broadcasts.count(deviceId) > 0) {
        this->d->m_broadcastsMutex.unlock();
        AkLogError() << '\'' << deviceId << "' is busy." << std::endl;

        return false;
    }

    this->d->m_broadcasts[deviceId] = {};
    auto &slot = this->d->m_broadcasts[deviceId];
    slot.type = StreamType_Input;
    slot.run = true;

    /* NOTE: When the data mode is configured as SharedMemory, the socket
     * channel is not used to send/receive data, but to indicate that the
     * virtual camera is in use.
     */

    if (type == StreamType_Input) {
        slot.messageFuture =
            this->d->m_messageClient.send(MsgListen(deviceId, currentPid()).toMessage(),
                                          std::bind(&IpcBridgePrivate::frameReady,
                                                    this->d,
                                                    std::placeholders::_1));
        AkLogDebug() << "Started input stream for device: " << deviceId << std::endl;
    } else {
        slot.messageFuture =
            this->d->m_messageClient.send([this, deviceId] (Message &message) -> bool {
            return this->d->frameRequired(deviceId, message);
        });
        AkLogDebug() << "Started output stream for device: " << deviceId << std::endl;
    }

    if (this->d->m_dataMode == DataMode_SharedMemory) {
        slot.sharedMemory.setName(deviceId + "Shm");
        slot.sharedMemory.open(this->d->m_pageSize,
                               type == StreamType_Input?
                                   SharedMemory::OpenModeRead:
                                   SharedMemory::OpenModeWrite);
    }

    this->d->m_broadcastsMutex.unlock();

    return true;
}

void AkVCam::IpcBridge::deviceStop(const std::string &deviceId)
{
    AkLogFunction();
    AkLogDebug() << "Stopping device: " << deviceId << std::endl;

    std::future<bool> messageFuture;

    {
        std::lock_guard<std::mutex> lock(this->d->m_broadcastsMutex);

        if (this->d->m_broadcasts.count(deviceId) < 1) {
            AkLogDebug() << "Device " << deviceId << " not found in broadcasts" << std::endl;

            return;
        }

        auto &slot = this->d->m_broadcasts[deviceId];
        slot.sharedMemory.close();
        slot.run = false;
        messageFuture = std::move(slot.messageFuture); // Move the future
        AkLogDebug() << "Set run = false for device: " << deviceId << std::endl;
    } // m_broadcastsMutex is released here

    // Wait for the connection loop to end
    if (messageFuture.valid()) {
        AkLogDebug() << "Waiting for messageFuture for device: " << deviceId << std::endl;
        auto status = messageFuture.wait_for(std::chrono::seconds(5));

        if (status == std::future_status::timeout)
            AkLogWarning() << "Timeout waiting for messageFuture in deviceStop for deviceId: " << deviceId << std::endl;
        else
            AkLogDebug() << "messageFuture completed for device: " << deviceId << std::endl;
    } else {
        AkLogWarning() << "Invalid messageFuture for device: " << deviceId << std::endl;
    }

    // Remove the device after the future is complete
    {
        std::lock_guard<std::mutex> lock(this->d->m_broadcastsMutex);
        this->d->m_broadcasts.erase(deviceId);
        AkLogDebug() << "Device " << deviceId << " removed from broadcasts" << std::endl;
    }
}

// ✅ 日志记录函数：记录 write() 的每一步
static void writeBridgeWriteLog(const std::string &step, 
                                  const std::string &info = "",
                                  long long durationUs = -1)
{
    static std::ofstream logFile;
    static std::mutex logMutex;
    static bool initialized = false;
    
    if (!initialized) {
        CHAR tempPath[MAX_PATH];
        GetTempPathA(MAX_PATH, tempPath);
        std::string logFilePath = std::string(tempPath) + "AkVCam_Bridge_Write_Log.txt";
        
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
        localtime_s(&tm_buf, &time_t);
        
        char timeStr[64];
        auto msValue = static_cast<long long>(ms.count());
        std::snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                     tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, msValue);
        
        logFile << timeStr << " - [BRIDGE WRITE] " << step;
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

bool AkVCam::IpcBridge::write(const std::string &deviceId,
                              const VideoFrame &frame)
{
    auto functionStartTime = std::chrono::high_resolution_clock::now();
    writeBridgeWriteLog("1. 函数开始", "设备: " + deviceId);

    auto lockBroadcastsStartTime = std::chrono::high_resolution_clock::now();
    this->d->m_broadcastsMutex.lock();
    auto lockBroadcastsEndTime = std::chrono::high_resolution_clock::now();
    auto lockBroadcastsDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        lockBroadcastsEndTime - lockBroadcastsStartTime).count();
    writeBridgeWriteLog("2. 获取broadcastsMutex", "", lockBroadcastsDuration);

    auto checkDirectModeStartTime = std::chrono::high_resolution_clock::now();
    if (!this->d->m_directModeStatus.contains(deviceId))
        this->d->m_directModeStatus[deviceId] = {deviceId};
    auto checkDirectModeEndTime = std::chrono::high_resolution_clock::now();
    auto checkDirectModeDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        checkDirectModeEndTime - checkDirectModeStartTime).count();
    writeBridgeWriteLog("3. 检查directMode", "", checkDirectModeDuration);

    if (!this->d->m_directModeStatus[deviceId].isValid(frame.format())) {
        writeBridgeWriteLog("3. 函数结束", "错误: directMode无效");
        this->d->m_broadcastsMutex.unlock();
        return false;
    }

    auto checkBroadcastsStartTime = std::chrono::high_resolution_clock::now();
    bool deviceExists = this->d->m_broadcasts.count(deviceId) >= 1;
    auto checkBroadcastsEndTime = std::chrono::high_resolution_clock::now();
    auto checkBroadcastsDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        checkBroadcastsEndTime - checkBroadcastsStartTime).count();
    writeBridgeWriteLog("4. 检查设备是否存在", deviceExists ? "存在" : "不存在", checkBroadcastsDuration);

    if (!deviceExists) {
        writeBridgeWriteLog("4. 函数结束", "错误: 设备不存在");
        this->d->m_broadcastsMutex.unlock();
        return false;
    }

    auto &slot = this->d->m_broadcasts[deviceId];

    auto checkStreamTypeStartTime = std::chrono::high_resolution_clock::now();
    bool isInputType = (slot.type == StreamType_Input);
    auto checkStreamTypeEndTime = std::chrono::high_resolution_clock::now();
    auto checkStreamTypeDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        checkStreamTypeEndTime - checkStreamTypeStartTime).count();
    writeBridgeWriteLog("5. 检查流类型", isInputType ? "Input" : "Output", checkStreamTypeDuration);

    if (!isInputType) {
        writeBridgeWriteLog("5. 函数结束", "错误: 流类型不是Input");
        this->d->m_broadcastsMutex.unlock();
        return false;
    }

    // ✅ 使用 try_lock() 快速尝试获取锁，不阻塞
    // 如果获取失败，说明 frameReady() 正在读取，我们直接写入共享内存即可
    auto tryFrameMutexStartTime = std::chrono::high_resolution_clock::now();
    bool frameMutexLocked = slot.frameMutex.try_lock();
    auto tryFrameMutexEndTime = std::chrono::high_resolution_clock::now();
    auto tryFrameMutexDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        tryFrameMutexEndTime - tryFrameMutexStartTime).count();
    writeBridgeWriteLog("6. 尝试获取frameMutex", frameMutexLocked ? "成功" : "失败", tryFrameMutexDuration);
    if (!frameMutexLocked) {
        // ✅ 无法获取 frameMutex：frameReady() 正在读取
        // 直接写入共享内存（无锁覆盖），不更新 slot.frame（因为 frameReady() 会从共享内存读取）
        // 这样不会阻塞，完全由推送方控制频率
        auto checkSharedMemoryStartTime = std::chrono::high_resolution_clock::now();
        bool sharedMemoryOpen = slot.sharedMemory.isOpen();
        bool hasBuffer = (slot.sharedMemoryBuffer != nullptr);
        auto checkSharedMemoryEndTime = std::chrono::high_resolution_clock::now();
        auto checkSharedMemoryDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            checkSharedMemoryEndTime - checkSharedMemoryStartTime).count();
        writeBridgeWriteLog("7. 检查共享内存", 
                            "isOpen: " + std::string(sharedMemoryOpen ? "是" : "否") + 
                            " | hasBuffer: " + std::string(hasBuffer ? "是" : "否"), 
                            checkSharedMemoryDuration);
        
        if (sharedMemoryOpen && hasBuffer) {
            // ✅ 直接无锁覆盖写入共享内存
            auto lockFreeWriteStartTime = std::chrono::high_resolution_clock::now();
            auto sharedFrame = reinterpret_cast<SharedFrame *>(slot.sharedMemoryBuffer);
            auto dataSize = std::min(slot.sharedMemory.pageSize()
                                     - sizeof(SharedFrame)
                                     + sizeof(void *),
                                     frame.size());
            
            sharedFrame->format = frame.format().format();
            sharedFrame->width = frame.format().width();
            sharedFrame->height = frame.format().height();
            
            if (dataSize > 0)
                memcpy(sharedFrame->data, frame.constData(), dataSize);
            
            auto lockFreeWriteEndTime = std::chrono::high_resolution_clock::now();
            auto lockFreeWriteDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                lockFreeWriteEndTime - lockFreeWriteStartTime).count();
            writeBridgeWriteLog("8. 无锁覆盖写入", "数据大小: " + std::to_string(dataSize) + " bytes", lockFreeWriteDuration);
            
            // ✅ 记录日志
            writeSharedMemoryWriteLog(deviceId, 0, true, dataSize);
            
            auto unlockBroadcastsStartTime = std::chrono::high_resolution_clock::now();
            this->d->m_broadcastsMutex.unlock();
            auto unlockBroadcastsEndTime = std::chrono::high_resolution_clock::now();
            auto unlockBroadcastsDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                unlockBroadcastsEndTime - unlockBroadcastsStartTime).count();
            writeBridgeWriteLog("9. 释放broadcastsMutex", "", unlockBroadcastsDuration);
            
            auto functionEndTime = std::chrono::high_resolution_clock::now();
            auto functionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                functionEndTime - functionStartTime).count();
            writeBridgeWriteLog("10. 函数结束(无锁路径)", "总耗时: " + std::to_string(functionDuration) + "ms | 返回: true");
            
            return true;  // ✅ 成功写入（无锁），直接返回
        } else {
            // ✅ 共享内存未初始化或 buffer 未缓存，无法写入
            writeBridgeWriteLog("7. 函数结束", "错误: 共享内存未打开或buffer未缓存");
            this->d->m_broadcastsMutex.unlock();
            return false;
        }
    }

    auto checkSharedMemoryOpenStartTime = std::chrono::high_resolution_clock::now();
    bool sharedMemoryIsOpen = slot.sharedMemory.isOpen();
    auto checkSharedMemoryOpenEndTime = std::chrono::high_resolution_clock::now();
    auto checkSharedMemoryOpenDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        checkSharedMemoryOpenEndTime - checkSharedMemoryOpenStartTime).count();
    writeBridgeWriteLog("7. 检查共享内存是否打开", sharedMemoryIsOpen ? "是" : "否", checkSharedMemoryOpenDuration);
    
    if (sharedMemoryIsOpen) {
        // ✅ 记录写入端等待 mutex 的时间
        auto writeLockStartTime = std::chrono::high_resolution_clock::now();
        writeBridgeWriteLog("8. 开始尝试获取sharedMemory锁", "timeout=0");
        
        // ✅ 尝试立即获取锁（timeout=0，不等待）
        // 如果成功，正常写入；如果失败，直接覆盖（推送方负责时间控制）
        auto sharedFrame = reinterpret_cast<SharedFrame *>(slot.sharedMemory.lock(0));
        
        auto writeLockEndTime = std::chrono::high_resolution_clock::now();
        auto writeLockDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
            writeLockEndTime - writeLockStartTime).count();
        writeBridgeWriteLog("9. 尝试获取sharedMemory锁完成", 
                            sharedFrame ? "成功" : "失败", 
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                writeLockEndTime - writeLockStartTime).count());
        
        // ✅ 计算数据大小（用于日志）
        auto calcDataSizeStartTime = std::chrono::high_resolution_clock::now();
        auto dataSize = std::min(slot.sharedMemory.pageSize()
                                 - sizeof(SharedFrame)
                                 + sizeof(void *),
                                 frame.size());
        auto calcDataSizeEndTime = std::chrono::high_resolution_clock::now();
        auto calcDataSizeDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            calcDataSizeEndTime - calcDataSizeStartTime).count();
        writeBridgeWriteLog("10. 计算数据大小", "大小: " + std::to_string(dataSize) + " bytes", calcDataSizeDuration);
        
        bool usedLockedWrite = false;  // 标记是否使用了锁写入
        
        // ✅ 完全无锁写入：推送方完全控制时间，不等待任何锁
        auto decideWriteModeStartTime = std::chrono::high_resolution_clock::now();
        if (!sharedFrame && slot.sharedMemoryBuffer) {
            // ✅ 有缓存的 buffer 指针：直接无锁覆盖写入
            sharedFrame = reinterpret_cast<SharedFrame *>(slot.sharedMemoryBuffer);
            writeLockDuration = 0;  // 无锁写入，耗时 0
            writeBridgeWriteLog("11. 决定写入模式", "无锁覆盖写入");
            // 不需要 unlock，因为没获取锁
        } else if (sharedFrame) {
            // ✅ 成功获取锁：第一次时保存 buffer 指针供后续无锁写入使用
            if (!slot.sharedMemoryBuffer) {
                slot.sharedMemoryBuffer = sharedFrame;
                writeBridgeWriteLog("11. 决定写入模式", "加锁写入(首次，保存buffer)");
            } else {
                writeBridgeWriteLog("11. 决定写入模式", "加锁写入");
            }
            usedLockedWrite = true;  // 标记使用了锁写入
        } else {
            // ✅ 第一次写入且 lock 失败：直接返回失败（不阻塞，不等待，不重试）
            // Node.js 端会继续推送下一帧，下次写入时应该能成功（因为第一次写入通常能获取锁）
            // 如果这次写入失败，只是跳过这一帧，不影响后续推送
            sharedFrame = nullptr;  // 确保为 nullptr，表示写入失败
            writeLockDuration = 0;  // 无等待，耗时 0
            writeBridgeWriteLog("11. 决定写入模式", "失败: 无buffer且无法获取锁");
        }
        auto decideWriteModeEndTime = std::chrono::high_resolution_clock::now();
        auto decideWriteModeDuration = std::chrono::duration_cast<std::chrono::microseconds>(
            decideWriteModeEndTime - decideWriteModeStartTime).count();
        writeBridgeWriteLog("11. 决定写入模式完成", "", decideWriteModeDuration);
        
        // ✅ 记录共享内存写入日志（所有调用，包括成功和失败）
        writeSharedMemoryWriteLog(deviceId, writeLockDuration, sharedFrame != nullptr, dataSize);
        
        // 如果等待时间较长或超时，记录框架日志
        if (writeLockDuration > 5 || !sharedFrame) {
            AkLogInfo() << "[FRAME WRITE] Device: " << deviceId
                       << " | 等待 mutex 耗时: " << writeLockDuration << "ms"
                       << " | 结果: " << (sharedFrame ? (usedLockedWrite ? "加锁写入" : "无锁覆盖") : "失败")
                       << std::endl;
        }

        if (sharedFrame) {
            // ✅ 直接覆盖写入（推送方负责时间控制，显示最新帧）
            auto writeToSharedMemoryStartTime = std::chrono::high_resolution_clock::now();
            sharedFrame->format = frame.format().format();
            sharedFrame->width = frame.format().width();
            sharedFrame->height = frame.format().height();

            if (dataSize > 0)
                memcpy(sharedFrame->data, frame.constData(), dataSize);
            auto writeToSharedMemoryEndTime = std::chrono::high_resolution_clock::now();
            auto writeToSharedMemoryDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                writeToSharedMemoryEndTime - writeToSharedMemoryStartTime).count();
            writeBridgeWriteLog("12. 写入共享内存", "", writeToSharedMemoryDuration);

            // ✅ 如果使用了锁写入，需要释放锁
            auto unlockSharedMemoryStartTime = std::chrono::high_resolution_clock::now();
            if (usedLockedWrite) {
                slot.sharedMemory.unlock();
            }
            auto unlockSharedMemoryEndTime = std::chrono::high_resolution_clock::now();
            auto unlockSharedMemoryDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                unlockSharedMemoryEndTime - unlockSharedMemoryStartTime).count();
            if (usedLockedWrite) {
                writeBridgeWriteLog("13. 释放sharedMemory锁", "", unlockSharedMemoryDuration);
            }
            
            // ✅ 立即更新 slot.frame，确保 frameReady 能读取到最新帧
            // 这样即使 frameReady 延迟触发，也能读取到最新写入的帧
            auto updateSlotFrameStartTime = std::chrono::high_resolution_clock::now();
            VideoFormat format(sharedFrame->format, sharedFrame->width, sharedFrame->height);
            if (!format.isSameFormat(slot.frame.format())) {
                slot.frame = VideoFrame(format);
            }
            if (dataSize > 0 && slot.frame.size() > 0) {
                auto copySize = std::min(dataSize, slot.frame.size());
                memcpy(slot.frame.data(), sharedFrame->data, copySize);
            }
            auto updateSlotFrameEndTime = std::chrono::high_resolution_clock::now();
            auto updateSlotFrameDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                updateSlotFrameEndTime - updateSlotFrameStartTime).count();
            writeBridgeWriteLog("14. 更新slot.frame", "", updateSlotFrameDuration);
            
            auto notifyStartTime = std::chrono::high_resolution_clock::now();
            slot.available = true;
            slot.frameAvailable.notify_all();
            auto notifyEndTime = std::chrono::high_resolution_clock::now();
            auto notifyDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                notifyEndTime - notifyStartTime).count();
            writeBridgeWriteLog("15. 通知frameAvailable", "", notifyDuration);
        } else {
            // ✅ 无法写入（共享内存未初始化）
            if (frameMutexLocked) {
                slot.frameMutex.unlock();
            }
            this->d->m_broadcastsMutex.unlock();
            return false;
        }
    } else {
        // ✅ 共享内存未打开，使用内部 frame 存储
        slot.frame = frame;
        slot.available = true;
        slot.frameAvailable.notify_all();
    }

    // ✅ 释放 frameMutex（如果之前成功获取）
    auto unlockFrameMutexStartTime = std::chrono::high_resolution_clock::now();
    if (frameMutexLocked) {
        slot.frameMutex.unlock();
    }
    auto unlockFrameMutexEndTime = std::chrono::high_resolution_clock::now();
    auto unlockFrameMutexDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        unlockFrameMutexEndTime - unlockFrameMutexStartTime).count();
    if (frameMutexLocked) {
        writeBridgeWriteLog("16. 释放frameMutex", "", unlockFrameMutexDuration);
    }

    auto unlockBroadcastsFinalStartTime = std::chrono::high_resolution_clock::now();
    this->d->m_broadcastsMutex.unlock();
    auto unlockBroadcastsFinalEndTime = std::chrono::high_resolution_clock::now();
    auto unlockBroadcastsFinalDuration = std::chrono::duration_cast<std::chrono::microseconds>(
        unlockBroadcastsFinalEndTime - unlockBroadcastsFinalStartTime).count();
    writeBridgeWriteLog("17. 释放broadcastsMutex(最终)", "", unlockBroadcastsFinalDuration);

    auto functionEndTime = std::chrono::high_resolution_clock::now();
    auto functionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        functionEndTime - functionStartTime).count();
    writeBridgeWriteLog("18. 函数结束", "总耗时: " + std::to_string(functionDuration) + "ms | 返回: true");

    return true;
}

bool AkVCam::IpcBridge::isBusyFor(const std::string &operation) const
{
    static const std::vector<std::string> operations {
        "add-device",
        "add-format",
        "load",
        "remove-device",
        "remove-devices",
        "remove-format",
        "remove-formats",
        "set-description",
        "update",
        "hack"
    };

    auto it = std::find(operations.begin(), operations.end(), operation);

    return it != operations.end() && !this->clientsPids().empty();
}

bool AkVCam::IpcBridge::needsRoot(const std::string &operation) const
{
    static const std::vector<std::string> operations {
        "add-device",
        "add-format",
        "load",
        "remove-device",
        "remove-devices",
        "remove-format",
        "remove-formats",
        "set-description",
        "set-loglevel",
        "update"
    };

    auto it = std::find(operations.begin(), operations.end(), operation);

    return it != operations.end() && !this->d->isRoot();
}

std::vector<std::string> AkVCam::IpcBridge::hacks() const
{
    std::vector<std::string> hacks;

    for (auto &hack: this->d->hacks())
        hacks.push_back(hack.name);

    return hacks;
}

std::string AkVCam::IpcBridge::hackDescription(const std::string &hack) const
{
    for (auto &hck: this->d->hacks())
        if (hck.name == hack)
            return hck.description;

    return {};
}

bool AkVCam::IpcBridge::hackIsSafe(const std::string &hack) const
{
    for (auto &hck: this->d->hacks())
        if (hck.name == hack)
            return hck.isSafe;

    return true;
}

bool AkVCam::IpcBridge::hackNeedsRoot(const std::string &hack) const
{
    for (auto &hck: this->d->hacks())
        if (hck.name == hack)
            return hck.needsRoot && !this->d->isRoot();

    return false;
}

int AkVCam::IpcBridge::execHack(const std::string &hack,
                                const std::vector<std::string> &args)
{
    for (auto &hck: this->d->hacks())
        if (hck.name == hack)
            return hck.func(args);

    return 0;
}

AkVCam::IpcBridgePrivate::IpcBridgePrivate(IpcBridge *self):
    self(self)
{
    AkLogFunction();

    this->m_logLevel = Preferences::logLevel();
    AkVCam::Logger::setLogLevel(this->m_logLevel);
    this->m_picture = Preferences::picture();
    this->m_dataMode = Preferences::dataMode();
    this->m_pageSize = Preferences::pageSize();
    this->updateDevices();

    if (!this->launchService())
        AkLogWarning() << "There was not possible to communicate with the server consider increasing the timeout." << std::endl;

    this->m_messageClient.setPort(Preferences::servicePort());
    this->m_messagesTimer.connectTimeout(this, &IpcBridgePrivate::checkStatus);
    this->m_messagesTimer.setInterval(1000);
    this->m_messagesTimer.start();
}

AkVCam::IpcBridgePrivate::~IpcBridgePrivate()
{
    AkLogFunction();

    this->m_messagesTimer.stop();
    AkLogDebug() << "Bridge Destroyed" << std::endl;
}

void AkVCam::IpcBridgePrivate::updateDevices()
{
    AkLogFunction();

    this->m_devices.clear();
    auto nCameras = Preferences::camerasCount();
    AkLogInfo() << "Devices:" << std::endl;

    for (size_t i = 0; i < nCameras; i++) {
        auto deviceId = Preferences::cameraId(i);
        this->m_devices.push_back(deviceId);
        AkLogInfo() << "    " << deviceId << std::endl;
    }
}

bool AkVCam::IpcBridgePrivate::isServiceRunning()
{
    AkLogFunction();

    AkVCam::SharedMemory serviceLock;
    serviceLock.setName(AKVCAM_SERVICE_NAME "_Lock");
    bool result = false;

    if (serviceLock.open(1024)) {
        if (serviceLock.lock()) {
            result = true;
            serviceLock.unlock();
        }

        serviceLock.close();
    }

    AkLogDebug() << "Result: " << result << std::endl;

    return result;
}

bool AkVCam::IpcBridgePrivate::isMFServiceRunning()
{
    AkLogFunction();

    AkVCam::SharedMemory serviceLock;
    serviceLock.setName(AKVCAM_SERVICE_MF_NAME "_Lock");
    bool result = false;

    if (serviceLock.open(1024)) {
        if (serviceLock.lock()) {
            result = true;
            serviceLock.unlock();
        }

        serviceLock.close();
    }

    AkLogDebug() << "Result: " << result << std::endl;

    return result;
}

bool AkVCam::IpcBridgePrivate::launchService()
{
    AkLogFunction();

    if (!isServiceRunning()) {
        AkLogDebug() << "Launching the service" << std::endl;
        auto servicePath = locateServicePath();

        if (!servicePath.empty())
            execDetached({servicePath});
        else
            AkLogDebug() << "Service path not found" << std::endl;
    }

    if (supportsMediaFoundationVCam() && !isMFServiceRunning()) {
        AkLogDebug() << "Launching the Media Foundation service" << std::endl;
        auto mfServicePath = locateMFServicePath();

        if (!mfServicePath.empty())
            execDetached({mfServicePath});
        else
            AkLogDebug() << "Media Foundation Service path not found" << std::endl;
    }

    bool ok = false;
    auto timeout = Preferences::serviceTimeout();
    AkLogDebug() << "Service check Timeout:" << timeout << std::endl;

    for (int i = 0; i < timeout; ++i) {
        if (isServicePortUp()) {
            ok = true;

            break;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));;
    }

    return ok;
}

const std::vector<AkVCam::DeviceControl> &AkVCam::IpcBridgePrivate::controls() const
{
    static const std::vector<std::string> scalingMenu {
        "Fast",
        "Linear"
    };
    static const std::vector<std::string> aspectRatioMenu {
        "Ignore",
        "Keep",
        "Expanding"
    };
    static const auto scalingMax = int(scalingMenu.size()) - 1;
    static const auto aspectRatioMax = int(aspectRatioMenu.size()) - 1;

    static const std::vector<DeviceControl> controls {
        {"hflip"       , "Horizontal Mirror", ControlTypeBoolean, 0, 1             , 1, 0, 0, {}             },
        {"vflip"       , "Vertical Mirror"  , ControlTypeBoolean, 0, 1             , 1, 0, 0, {}             },
        {"scaling"     , "Scaling"          , ControlTypeMenu   , 0, scalingMax    , 1, 0, 0, scalingMenu    },
        {"aspect_ratio", "Aspect Ratio"     , ControlTypeMenu   , 0, aspectRatioMax, 1, 0, 0, aspectRatioMenu},
        {"swap_rgb"    , "Swap RGB"         , ControlTypeBoolean, 0, 1             , 1, 0, 0, {}             },
    };

    return controls;
}

bool AkVCam::IpcBridgePrivate::frameRequired(const std::string &deviceId,
                                             Message &message)
{
    AkLogFunction();

    this->m_broadcastsMutex.lock();

    if (this->m_broadcasts.count(deviceId) < 1) {
        this->m_broadcastsMutex.unlock();

        return false;
    }

    auto &slot = this->m_broadcasts[deviceId];

    std::unique_lock<std::mutex> lock(slot.frameMutex);

    if (!slot.available)
        slot.frameAvailable.wait_for(lock,
                                     std::chrono::seconds(1));

    auto &frame = slot.frame;
    auto run = slot.run;
    slot.available = false;
    lock.unlock();

    message = MsgBroadcast(deviceId, currentPid(), frame).toMessage();
    this->m_broadcastsMutex.unlock();

    return run;
}

bool AkVCam::IpcBridgePrivate::frameReady(const Message &message)
{
    AkLogFunction();

    MsgFrameReady msgFrameReady(message);

    this->m_broadcastsMutex.lock();

    auto deviceId = msgFrameReady.device();

    if (this->m_broadcasts.count(deviceId) < 1) {
        this->m_broadcastsMutex.unlock();

        return false;
    }

    auto &slot = this->m_broadcasts[deviceId];
    auto run = slot.run;

    if (slot.sharedMemory.isOpen()) {
        // ✅ 记录驱动读取开始时间
        auto readStartTime = std::chrono::high_resolution_clock::now();
        
        // ✅ 使用短超时（10ms）避免长时间阻塞，如果写入端正在无锁写入
        auto sharedFrame =
                reinterpret_cast<SharedFrame *>(slot.sharedMemory.lock(10));

        if (sharedFrame) {
            // ✅ 使用 frameMutex 保护 slot.frame 的更新，避免与 write() 竞争
            slot.frameMutex.lock();
            
            VideoFormat format(sharedFrame->format,
                               sharedFrame->width,
                               sharedFrame->height);

            if (!format.isSameFormat(slot.frame.format()))
                slot.frame = VideoFrame(format);

            auto dataSize =
                    std::min(slot.sharedMemory.pageSize()
                             - sizeof(SharedFrame)
                             + sizeof(void *),
                             slot.frame.size());

            if (dataSize > 0)
                memcpy(slot.frame.data(), sharedFrame->data, dataSize);
            else
                slot.frame = {};

            slot.frameMutex.unlock();
            slot.sharedMemory.unlock();
            
            // ✅ 记录驱动读取结束时间并计算耗时
            auto readEndTime = std::chrono::high_resolution_clock::now();
            auto readDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                readEndTime - readStartTime).count();
            
            // ✅ 写入自定义日志文件
            writeFrameReadLog(deviceId, readDuration, dataSize);
        } else {
            // ✅ 读取超时：可能写入端正在无锁写入，使用最新的 slot.frame（已在 write() 中更新）
            // 记录日志但不更新 slot.frame（因为 write() 已经更新了）
            auto readEndTime = std::chrono::high_resolution_clock::now();
            auto readDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                readEndTime - readStartTime).count();
            
            if (readDuration >= 10) {
                writeFrameReadLog(deviceId, readDuration, 0);
            }
        }
    }

    this->m_broadcastsMutex.unlock();

    if (slot.sharedMemory.isOpen())
        AKVCAM_EMIT(this->self,
                    FrameReady,
                    deviceId,
                    slot.frame,
                    msgFrameReady.isActive())
    else
        AKVCAM_EMIT(this->self,
                    FrameReady,
                    deviceId,
                    msgFrameReady.frame(),
                    msgFrameReady.isActive())

    return run;
}

void AkVCam::IpcBridgePrivate::checkStatus(void *userData)
{
    AkLogFunction();
    auto self = reinterpret_cast<IpcBridgePrivate *>(userData);
    self->m_statusMutex.lock();
    auto devices = self->self->devices();

    if (devices != self->m_devices) {
        self->m_devices = devices;
        AKVCAM_EMIT(self->self, DevicesChanged, devices)
    }

    auto picture = self->self->picture();

    if (picture != self->m_picture) {
        self->m_picture = picture;
        AKVCAM_EMIT(self->self, PictureChanged, picture)
    }

    std::vector<std::string> removeDevices;

    for (auto &device: self->m_controlValues)
        if (std::count(devices.begin(), devices.begin(), device.first) < 1)
            removeDevices.push_back(device.first);

    for (auto &device: removeDevices)
        self->m_controlValues.erase(device);

    for (auto &device: devices) {
        std::map<std::string, int> controlValues;

        for (auto &control: self->self->controls(device))
            controlValues[control.id] = control.value;

        if (self->m_controlValues.count(device) < 1)
            self->m_controlValues[device] = {};

        if (controlValues != self->m_controlValues[device]) {
            self->m_controlValues[device] = controlValues;
            AKVCAM_EMIT(self->self, ControlsChanged, device, controlValues)
        }
    }

    self->m_statusMutex.unlock();
}

bool AkVCam::IpcBridgePrivate::isRoot() const
{
    AkLogFunction();
    HANDLE token = nullptr;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    TOKEN_ELEVATION elevationInfo;
    memset(&elevationInfo, 0, sizeof(TOKEN_ELEVATION));
    DWORD len = 0;

    if (!GetTokenInformation(token,
                             TokenElevation,
                             &elevationInfo,
                             sizeof(TOKEN_ELEVATION),
                             &len)) {
        CloseHandle(token);

        return false;
    }

    CloseHandle(token);

    return elevationInfo.TokenIsElevated;
}

const std::vector<AkVCam::Hack> &AkVCam::IpcBridgePrivate::hacks()
{
    static const std::vector<AkVCam::Hack> hacks {
    };

    return hacks;
}

AkVCam::Hack::Hack()
{

}

AkVCam::Hack::Hack(const std::string &name,
                   const std::string &description,
                   bool isSafe,
                   bool needsRoot,
                   const Hack::HackFunc &func):
    name(name),
    description(description),
    isSafe(isSafe),
    needsRoot(needsRoot),
    func(func)
{

}

AkVCam::Hack::Hack(const Hack &other):
    name(other.name),
    description(other.description),
    isSafe(other.isSafe),
    needsRoot(other.needsRoot),
    func(other.func)
{
}

AkVCam::Hack &AkVCam::Hack::operator =(const Hack &other)
{
    if (this != &other) {
        this->name = other.name;
        this->description = other.description;
        this->isSafe = other.isSafe;
        this->needsRoot = other.needsRoot;
        this->func = other.func;
    }

    return *this;
}
