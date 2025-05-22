#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <sys/socket.h>  // 套接字编程头文件
#include <netinet/in.h>   // 网络地址结构定义
#include <arpa/inet.h>    // 地址转换函数
#include <unistd.h>       // 系统调用（close等）
#include <regex>          // 正则表达式库
#include <fstream>        // 文件流操作

#define CONTROL_PORT 2100 // 服务器控制端口
#define BUFFER_SIZE 1024  // 数据缓冲区大小
class FTPClient
{
    private:
    int ctrl_sock = -1;   // 控制连接socket描述符
    int data_sock = -1;   // 数据连接socket描述符
    std::string server_ip;// 服务器IP地址
    public:
    FTPClient(const std::string& ip) : server_ip(ip) {};
    bool connect()
    {
        ctrl_sock = socket(AF_INET, SOCK_STREAM, 0);
        if(ctrl_sock < 0) 
        {
            return false;
        }
         // 配置服务器地址结构
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;         // IPv4
        server_addr.sin_port = htons(CONTROL_PORT); // 端口转换
        inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr); // IP转换

        // 发起连接请求
        if(::connect(ctrl_sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(ctrl_sock);
            return false;
        }
        std::cout << "connect win" << std::endl;
        char buffer[BUFFER_SIZE];
        recv(ctrl_sock, buffer, BUFFER_SIZE, 0);
        std::cout << buffer << std::endl;
        return true;
    }

};
int main()
{
    FTPClient client("127.0.0.1");
    while(1)
    {
        if(client.connect())
        {
            std::cout<<"连接成功"<<std::endl;
        };
        std::cout<<"ftp>>";
        
    }
}
    