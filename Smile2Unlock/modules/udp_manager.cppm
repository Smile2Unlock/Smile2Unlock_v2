module;

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <boost/asio.hpp>

// 包含统一的 UDP 管理头文件
#include "managers/ipc/udp/udp_manager.h"

export module smile2unlock.udp_manager;

import std;

// 导出命名空间和类型
export namespace smile2unlock::udp {
    using UdpPorts = ::smile2unlock::udp::UdpPorts;
    using StatusSender = ::smile2unlock::udp::StatusSender;
    using StatusReceiver = ::smile2unlock::udp::StatusReceiver;
    using AuthRequestSender = ::smile2unlock::udp::AuthRequestSender;
    using AuthRequestReceiver = ::smile2unlock::udp::AuthRequestReceiver;
    using PasswordSender = ::smile2unlock::udp::PasswordSender;
    using PasswordReceiver = ::smile2unlock::udp::PasswordReceiver;
}

// 向后兼容性别名
export namespace smile2unlock::managers {
    // UdpReceiverFromFR: 接收来自 FR 的状态 (端口 51235)
    using UdpReceiverFromFR = ::smile2unlock::udp::StatusReceiver;
    
    // UdpSenderToCP: 发送状态到 CP (端口 51234)
    using UdpSenderToCP = ::smile2unlock::udp::StatusSender;
    
    // UdpReceiverFromCP: 接收来自 CP 的认证请求 (端口 51236)
    using UdpReceiverFromCP = ::smile2unlock::udp::AuthRequestReceiver;
    
    // UdpPasswordSenderToCP: 发送密码响应到 CP (端口 51237)
    using UdpPasswordSenderToCP = ::smile2unlock::udp::PasswordSender;
}
