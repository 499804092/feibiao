#ifndef __SERIAL_PORT_H__
#define __SERIAL_PORT_H__

#include "global/Struct.hpp"

class SerialPort {
public:
    explicit SerialPort();
    ~SerialPort();

    bool init();
    bool receive(CarData &cd);
    void send(VisionData &vd);
    void closePort();

    // 【新增】串口有没有成功打开。main 里用它判断"飞控到底接没接"
    bool isOpen() const { return this->fd_ >= 0; }

    // 【新增】无飞控测试模式（对应 tuning.yaml 的 serialport.optional）
    //   0 = 原行为：串口打不开就报错退出（比赛用）
    //   1 = 测试模式：串口打不开也继续跑，用默认指令让【视觉链路能单独跑起来】
    bool isOptional() const { return this->optional_; }

private:
    void setBrate();
    int setBit();
    int fd_ = -1;                   // 【修正】原来没给初值，open 失败前是随机数，容易误判成"已打开"
    int baudrate_ = 460800;         // 波特率默认值（会被 tuning.yaml 覆盖）
    int databits_ = 8;              // 数据位默认值
    int stopbits_ = 1;              // 停止位默认值
    int parity_ = 'N';              // 校验位默认值：无校验
    bool optional_ = false;         // 【新增】无飞控测试模式开关
    bool warned_not_open_ = false;  // 【新增】串口没打开时"只提示一次"，避免死循环里刷屏
    unsigned char r_data_[4096];   // 接收缓冲：一次最多读 4KB(绰绰有余，电控帧只有 8 字节)
    unsigned char t_data_[64];     // 【修改】发送缓冲：原来 30 字节。
                                   // 新 VisionData 为 22 字节，30 也够用，但加大到 64 是为了
                                   // 以后再加字段(速度/预测点等)时不用回头改这里，避免溢出隐患。
};
#endif