// ============================================================================
// fake_serial.h —— 测试用的"假串口"实现（独立编译单元）
// ----------------------------------------------------------------------------
// 【为什么要它】
//   工程的取图流程第一步是：
//       while (!this->sp_.receive(car_data)) { sleep(5ms); }   // 等电控指令
//   没有接飞控板、没有串口设备时，这里会**永久阻塞**，根本走不到取图那一步。
//
//   所以做集成测试时，要用一个"永远能收到假指令"的串口实现替换掉真的
//   SerialPort.cpp（真实现在 src/sensors/src/serialport/SerialPort.cpp，
//   会去 open("/dev/ttyUSB")）。
//
//   用法：编译时**不要**把 SerialPort.cpp 加进去，只加本文件：
//       g++ ... uvc_project_integration_test.cpp fake_serial.cpp UvcCamera.cpp ...
//
//   本文件只属于测试，**不要**放进正式工程。
// ============================================================================

#include "serialport/SerialPort.h"
#include "global/Struct.hpp"

#include <cstdio>
#include <thread>
#include <chrono>

// ---- 构造函数 / 析构 ----
SerialPort::SerialPort() {
    this->fd_ = -1;
    printf("[假串口] 已创建（不打开真实设备，仅用于集成测试）\n");
}
SerialPort::~SerialPort() {}

bool SerialPort::init() { return true; }

// ---- 收"电控指令"：永远返回成功，并按 120fps 的节拍节流 ----
bool SerialPort::receive(CarData &cd) {
    static int n = 0;
    cd.header = 0xA5;
    cd.mode   = 1;                                   // 1 = 开自瞄（让跟踪线程正常工作）
    cd.status = 0;
    cd.number = (uint8_t)((n++ / 120) % 5);          // 每 120 帧换一发飞镖，0~4
    cd.dune   = 0;                                   // 龙门架完全展开
    std::this_thread::sleep_for(std::chrono::milliseconds(8));   // ≈120fps
    return true;
}

// ---- 发"视觉帧"：只统计，不真写串口 ----
void SerialPort::send(VisionData &vd) {
    static int cnt = 0;
    if (++cnt % 120 == 0) {
        printf("[假串口] 已（假）下发 %d 帧  yaw=%+.4f pitch=%+.4f roll=%+.4f dist=%.3f status=%d\n",
               cnt, vd.yaw_error, vd.pitch_error, vd.roll_error, vd.distance, vd.target_status);
    }
}

void SerialPort::closePort() {}
void SerialPort::setBrate() {}
int  SerialPort::setBit() { return 1; }
