/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <thread>
#include <future>
#include <unistd.h>
#include <csignal>
#include <chrono>
#include <fcntl.h>

#include "cmd_executor.h"
#include "flow_control_init.h"
#include "hilog_input_socket_server.h"
#include "log_collector.h"
#include "log_kmsg.h"
#include "properties.h"
#include "service_controller.h"

#ifdef DEBUG
#include <fcntl.h>
#include <cstdio>
#include <cerrno>
#endif

namespace OHOS {
namespace HiviewDFX {
using namespace std;
using namespace std::chrono;

constexpr int HILOG_FILE_MASK = 0026;
constexpr int WAITING_DATA_MS = 5000;

#ifdef DEBUG
static int g_fd = -1;
#endif

static void SigHandler(int sig)
{
    if (sig == SIGINT) {
#ifdef DEBUG
        if (g_fd > 0) {
            close(g_fd);
        }
#endif
        std::cout<<"Exited!"<<std::endl;
    }
}

static int WaitingDataMounted(int max)
{
    chrono::steady_clock::time_point start = chrono::steady_clock::now();
    chrono::milliseconds wait(max);

    while (true) {
        struct stat st;
        if (stat(HILOG_FILE_DIR, &st) != -1) {
            cout << "waiting for " << HILOG_FILE_DIR << " successfully!" << endl;
            return 0;
        }
        std::this_thread::sleep_for(10ms);
        if ((chrono::steady_clock::now() - start) > wait) {
            cerr << "waiting for " << HILOG_FILE_DIR << " failed!" << endl;
            return -1;
        }
    }
}

static bool WriteStringToFile(int fd, const std::string& content)
{
    const char *p = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if(n == -1) {
            return false;
        }
        p += n;
        remaining -= n;
    }
    return true;
}

static bool WriteStringToFile(int max, const std::string& content, const std::string& filePath)
{
    chrono::steady_clock::time_point start = chrono::steady_clock::now();
    chrono::milliseconds wait(max);
    int fd;
    while (true) {
        if (access(filePath.c_str(), W_OK)) {
            fd = open(filePath.c_str(), O_WRONLY | O_CLOEXEC);
            if (fd >= 0) {
                break;
            }
        }
        std::this_thread::sleep_for(10ms);
        if ((chrono::steady_clock::now() - start) > wait) {
            cerr << "waiting for " << HILOG_FILE_DIR << " failed!" << endl;
            return -1;
        }
    }
    cout << "waiting for " << filePath << " successfully!" << endl;
    bool result =  WriteStringToFile(fd, content);
    close(fd);
    return result;
}

int HilogdEntry()
{
    HilogBuffer hilogBuffer;
    umask(HILOG_FILE_MASK);
#ifdef DEBUG
    if (WaitingDataMounted(WAITING_DATA_MS) == 0) {
    int fd = open(HILOG_FILE_DIR"hilogd.txt", O_WRONLY | O_APPEND);
        if (fd > 0) {
            g_fd = dup2(fd, fileno(stdout));
        } else {
            std::cout << "open file error:" <<  strerror(errno) << std::endl;
        }
    }
#endif
    std::signal(SIGINT, SigHandler);

    InitDomainFlowCtrl();

    // Start log_collector
#ifndef __RECV_MSG_WITH_UCRED_
    auto onDataReceive = [&hilogBuffer](std::vector<char>& data) {
        static LogCollector logCollector(hilogBuffer);
        logCollector.onDataRecv(data);
    };
#else
    auto onDataReceive = [&hilogBuffer](const ucred& cred, std::vector<char>& data) {
        static LogCollector logCollector(hilogBuffer);
        logCollector.onDataRecv(cred, data);
    };
#endif
    
    HilogInputSocketServer incomingLogsServer(onDataReceive);
    if (incomingLogsServer.Init() < 0) {
#ifdef DEBUG
        cout << "Failed to init input server socket ! error=" << strerror(errno) << std::endl;
#endif
    } else {
        if (chmod(INPUT_SOCKET, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) < 0) {
            cout << "chmod input socket failed !\n";
        }
#ifdef DEBUG
        cout << "Begin to listen !\n";
#endif
        incomingLogsServer.RunServingThread();
    }

    auto startupCheckTask = std::async(std::launch::async, [&hilogBuffer]() {
        prctl(PR_SET_NAME, "hilogd.pst_res");
        if (WaitingDataMounted(WAITING_DATA_MS) == 0) {
            RestorePersistJobs(hilogBuffer);
        }
    });
    auto kmsgTask = std::async(std::launch::async, [&hilogBuffer]() {
        LogKmsg logKmsg(hilogBuffer);
        logKmsg.ReadAllKmsg();
    });

    auto cgroupWriteTask = std::async(std::launch::async, [&hilogBuffer]() {
        string myPid = to_string(getpid())
        WriteStringToFile(WAITING_DATA_MS, myPid, SYSTEM_BG_STUNE);
        WriteStringToFile(WAITING_DATA_MS, myPid, SYSTEM_BG_CPUSET);
        WriteStringToFile(WAITING_DATA_MS, myPid, SYSTEM_BG_BLKIO);
    });

    CmdExecutor cmdExecutor(hilogBuffer);
    cmdExecutor.MainLoop();
    return 0;
}
} // namespace HiviewDFX
} // namespace OHOS

int main()
{
    OHOS::HiviewDFX::HilogdEntry();
    return 0;
}
