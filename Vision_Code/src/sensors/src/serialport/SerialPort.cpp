
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#include <cstdlib>
#include <cstring>      // strerror：把 errno 翻译成人能看懂的文字
#include <cerrno>       // errno
#include <string>       // std::string（存串口名）
#include <cstdio>       // printf

#include "serialport/CRC_Check.h"
#include "serialport/SerialPort.h"
#include "global/Config.hpp"
SerialPort::SerialPort() {
    this->init();
}

SerialPort::~SerialPort() {
    this->closePort();
}

bool SerialPort::init() {
    // 从配置读串口名（tuning.yaml 的 serialport.port）
    std::string port = (std::string)J_SENSORS.config_["serialport"]["port"];
    this->fd_ = open(port.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);
    J_SENSORS.config_["serialport"]["baudrate"] >> this->baudrate_;
    J_SENSORS.config_["serialport"]["databits"] >> this->databits_;
    J_SENSORS.config_["serialport"]["stopbits"] >> this->stopbits_;
    this->parity_ =  'N';

    // 【新增】读"无飞控测试模式"开关（tuning.yaml 的 serialport.optional）
    //   0（或不写）= 原行为：飞控没接就报错退出，比赛用
    //   1           = 测试模式：飞控没接也继续跑，方便单独测视觉识别链路
    int opt = 0;
    if (!J_SENSORS.config_["serialport"]["optional"].empty()) {
        J_SENSORS.config_["serialport"]["optional"] >> opt;
    }
    this->optional_ = (opt != 0);

    if (this->fd_ == -1) {
        // 【修正】原来这里"静默 return false"，什么都不打印。
        //   结果 fd_ 保持 -1，后面 receive() 拿去 ioctl 只会报底层的
        //   "ioctl: Bad file descriptor"，让人完全看不懂问题在哪。
        //   现在把真正的原因和排查步骤直接说出来。
        printf("\n[SerialPort] ❌ 打不开串口 %s（%s）\n", port.c_str(), strerror(errno));

        if (this->optional_) {
            // 测试模式：不退出，用默认指令继续跑，只是不下发串口
            printf("  ⚙️  配置里 serialport.optional = 1（无飞控测试模式）\n");
            printf("      → 用默认指令继续跑，视觉识别链路可以单独测；串口不下发。\n");
            printf("      → 比赛前记得把 tuning.yaml 的 serialport.optional 改回 0！\n\n");
            return false;
        }

        printf("  这个口是飞控（STM32）用的，**飞控没接就会这样**。\n");
        printf("  怎么查：\n");
        printf("    1) 飞控插了吗？  ls -l /dev/ttyUSB*  /dev/ttyACM*\n");
        printf("       看实际叫什么名字，然后改 config/tuning.yaml 的 serialport.port\n");
        printf("    2) 是 USB 转串口的话，VMware 里要把它接给虚拟机：\n");
        printf("       虚拟机 → 可移动设备 → 找到串口设备 → 连接\n");
        printf("    3) 权限不够就加组：  sudo usermod -aG dialout $USER   （重新登录生效）\n");
        printf("      或临时：           sudo chmod 666 %s\n", port.c_str());
        printf("    4) 确认设备号变了没： ls /dev/serial/by-id/\n");
        printf("    5) 只想单独测视觉？把 tuning.yaml 的 serialport.optional 改成 1\n");
        return false;
    }

    this->setBrate();

    if (this->setBit() == false) {
        exit(0);
    }

    printf("SerialPort inits successed.\n");

    return true;
}

bool SerialPort::receive(CarData &cd) {
    // 【新增】串口压根没打开（飞控没接）时，只提示一次，不要每个循环都刷屏。
    //   原来这里每次都会让 ioctl 失败并打印 "ioctl: Bad file descriptor"，
    //   而 main 里是 while(!receive()) 死循环 → 每秒刷几百行，把有效日志全冲掉。
    if (this->fd_ < 0) {
        if (!this->warned_not_open_) {
            this->warned_not_open_ = true;
            if (this->optional_) {
                printf("[SerialPort] 串口没打开，但处于【无飞控测试模式】，用默认指令继续跑。\n");
            } else {
                printf("[SerialPort] 串口没打开（飞控没接），后面不再重复提示。\n");
            }
        }
        return false;
    }

    int bytes;
    int result = ioctl(this->fd_, FIONREAD, &bytes);

    if (result == -1) {
        printf("ioctl: %s\n", strerror(errno));
        return false;
    }

    if (bytes == 0) {
        return false;
    }

    bytes = read(this->fd_, this->r_data_, bytes);
//    std::cout << "Receive Bytes: " << bytes << std::endl;
    int first_index = -1;
    for (int i = 0 ; i < bytes ; i++) {
        if (this->r_data_[i] == 0xA5 &&
            first_index == -1 &&
            Verify_CRC8_Check_Sum(&this->r_data_[i], 3) &&
            Verify_CRC16_Check_Sum(&this->r_data_[i], sizeof(CarData))) {
            first_index = i;
            break;
        }
    }

    int max_mul = (bytes - first_index) / sizeof(CarData);

    if (first_index != -1) {
        int index = first_index + (max_mul - 1) *sizeof(CarData);
        if (this->r_data_[index] == 0xA5 &&
            Verify_CRC8_Check_Sum(&this->r_data_[index], 3)) {
            memcpy(&cd, &this->r_data_[index], sizeof(CarData));
            return true;
        } else {
            std::cout << "crc check failed." << std::endl;
            return false;
        }
    } else {
        std::cout << "cannot find header," << std::endl;
        return false;
    }
}

// ---------------------------------------------------------------------------
// 【核心】下发一帧： struct → 字节流 → 补 CRC → 串口 write
//   VisionData 现在是 18 字节：header(1) + CRC8(2) + pitch(4) + yaw(4)
//                              + distance(4) + status(1) + CRC16(2)
//   （原来还有一个 4 字节的 roll_error，已经删掉）
//   所有长度一律用 sizeof(VisionData) 表达，避免写死数字导致两边不一致。
// ---------------------------------------------------------------------------
void SerialPort::send(VisionData &vd) {
    // 【新增】串口没打开时直接返回，别反复调用 init() 刷屏
    //   （原来的逻辑是 write 失败就 closePort()+init() 做自恢复，
    //    但如果一开始就没打开成功，这样会在每帧都重试并打印一次错误）
    if (this->fd_ < 0) {
        return;
    }

    memcpy(this->t_data_, &vd, sizeof(VisionData));   // 把结构体按内存布局原样拷进发送缓冲
                                                      //（packed + 小端，与飞控板字节序一致）
    Append_CRC8_Check_Sum(this->t_data_, 3);          // 帧头区校验：对前 2 字节算 CRC8，
                                                      // 结果写入第 3 字节(t_data_[2])
                                                      // → 对应结构体的 CRC8_reserved 字段
    Append_CRC16_Check_Sum(this->t_data_, sizeof(VisionData));  // 整帧校验：对前 20 字节算 CRC16，
                                                                // 结果写入最后 2 字节
                                                                // → 对应结构体的 CRC16 字段
    int result = write(this->fd_, this->t_data_, sizeof(VisionData));  // 整帧写入串口(阻塞式)
    if (result == -1) {                               // 写失败：串口多半掉线了
//        std::cout << "send failed." << std::endl;
        this->closePort();                            // 先关闭句柄
        this->init();                                 // 再重新打开/重新配置波特率，做自恢复
    }

}

void SerialPort::closePort() {
    close(this->fd_);
}

void SerialPort::setBrate() {
    int speed_arr[] = {B921600, B460800, B115200, B38400, B19200, B9600, B4800, B2400, B1200, B300,
                       B921600, B460800, B115200, B38400, B19200, B9600, B4800, B2400, B1200, B300,
    };
    int name_arr[] = {921600, 460800, 115200, 38400, 19200, 9600, 4800, 2400, 1200,  300,
                      921600, 460800, 115200, 38400, 19200, 9600, 4800, 2400, 1200,  300,
    };

    unsigned int i;
    int status;
    struct termios Opt;     //用于存储获得的终端参数信息
    tcgetattr(this->fd_, &Opt);
    for (i = 0;  i < sizeof(speed_arr) / sizeof(int);  i++) {
        if (this->baudrate_ == name_arr[i]) {
            tcflush(this->fd_, TCIOFLUSH);                 //清空缓冲区的内容
            cfsetispeed(&Opt, speed_arr[i]);        //设置接受和发送的波特率
            cfsetospeed(&Opt, speed_arr[i]);
            status = tcsetattr(this->fd_, TCSANOW, &Opt);  //使设置立即生效

            if (status != 0) {
                return;
            }

            tcflush(this->fd_, TCIOFLUSH);

        }
    }
}

int SerialPort::setBit() {
    // 获取串口相关参数
    struct termios termios_p;
    tcgetattr(this->fd_, &termios_p);
    // CLOACL：保证程序不会占用串口
    // CREAD：使得能够从串口中读取输入数据
    termios_p.c_cflag |= (CLOCAL | CREAD);

    // 设置数据位
    termios_p.c_cflag &= ~CSIZE;
    switch (this->databits_) {
        case 7:
            termios_p.c_cflag |= CS7;
            break;

        case 8:
            termios_p.c_cflag |= CS8;
            break;

        default:
            fprintf(stderr, "Unsupported data size\n");
            return false;
    }

    // 设置奇偶校验位
    switch (this->parity_) {
        case 'n':
        case 'N':
            termios_p.c_cflag &= ~PARENB;       //PARENB:启用奇偶校验码的生成和检测功能。
            termios_p.c_iflag &= ~INPCK;        //INPCK：对接收到的字符执行奇偶校检。
            break;

        case 'o':
        case 'O':
            termios_p.c_cflag |= (PARODD | PARENB); /* 设置为奇效验*/
            termios_p.c_iflag |= INPCK;             /* Disnable parity checking */
            break;

        case 'e':
        case 'E':
            termios_p.c_cflag |= PARENB;        /* Enable parity */
            termios_p.c_cflag &= ~PARODD;       /* 转换为偶效验*/
            termios_p.c_iflag |= INPCK;         /* Disnable parity checking */
            break;

        case 'S':
        case 's':  /*as no parity*/
            termios_p.c_cflag &= ~PARENB;
            termios_p.c_cflag &= ~CSTOPB;
            break;

        default:
            fprintf(stderr, "Unsupported parity\n");
            return false;

    }

    // 设置停止位
    switch (this->stopbits_) {
        case 1:
            termios_p.c_cflag &= ~CSTOPB;
            break;

        case 2:
            termios_p.c_cflag |= CSTOPB;
            break;

        default:
            fprintf(stderr, "Unsupported stop bits\n");
            return false;

    }

    if (this->parity_ != 'n') {
        termios_p.c_iflag |= INPCK;
    }

    tcflush(this->fd_, TCIFLUSH);                                  //清除输入缓存区
    termios_p.c_cc[VTIME] = 1;                              //设置超时
    termios_p.c_cc[VMIN] = 0;                 //最小接收字符
    termios_p.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);   //Input原始输入
    // 写死了，不管前面写了什么，都一定是无奇偶校验
    termios_p.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    termios_p.c_iflag &= ~(ICRNL | IGNCR);
    termios_p.c_oflag &= ~OPOST;                            //Output禁用输出处理

    if (tcsetattr(this->fd_, TCSANOW, &termios_p) != 0) {
        return false;
    }

    return true;
}