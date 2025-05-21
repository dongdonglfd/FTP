#include <iostream>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <regex>
#include <fstream>
#include <cstring>
#include <sys/time.h>

#define CONTROL_PORT 2100
#define DATA_BUFFER_SIZE 4096
#define CONNECT_TIMEOUT 5
#define RESPONSE_TIMEOUT 10

class FTPClient {
private:
    int ctrl_sock = -1;
    int data_sock = -1;
    bool pasv_mode = false;
    std::string last_error;

    // 设置socket非阻塞模式
    bool set_nonblock(int sock, bool nonblock) {
        int flags = fcntl(sock, F_GETFL, 0);
        return (flags == -1) ? false : fcntl(sock, F_SETFL, nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) != -1;
    }

    bool send_command(const std::string& cmd, std::string& response) {
        std::string full_cmd = cmd + "\r\n";
        if (send(ctrl_sock, full_cmd.c_str(), full_cmd.size(), 0) < 0) {
            last_error = "发送失败: " + std::string(strerror(errno));
            return false;
        }

        struct timeval start;
        gettimeofday(&start, nullptr);
        response.clear();

        while (true) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ctrl_sock, &fds);

            timeval now;
            gettimeofday(&now, nullptr);
            double elapsed = (now.tv_sec - start.tv_sec) + 
                           (now.tv_usec - start.tv_usec) / 1000000.0;
            if (elapsed >= RESPONSE_TIMEOUT) {
                last_error = "响应超时";
                return false;
            }

            timeval timeout = { 
                RESPONSE_TIMEOUT - static_cast<int>(elapsed),
                static_cast<int>((RESPONSE_TIMEOUT - elapsed - 
                               static_cast<int>(RESPONSE_TIMEOUT - elapsed)) * 1000000)
            };

            int ready = select(ctrl_sock + 1, &fds, nullptr, nullptr, &timeout);
            if (ready < 0) {
                last_error = "select错误: " + std::string(strerror(errno));
                return false;
            }
            if (ready == 0) continue;

            char buffer[DATA_BUFFER_SIZE];
            ssize_t bytes = recv(ctrl_sock, buffer, sizeof(buffer), 0);
            if (bytes < 0) {
                last_error = "接收失败: " + std::string(strerror(errno));
                return false;
            }
            if (bytes == 0) break;

            response.append(buffer, bytes);
            if (response.find("\r\n") != std::string::npos) break;
        }

        size_t end = response.find_last_not_of("\r\n");
        if (end != std::string::npos) {
            response = response.substr(0, end + 1);
        }
        return true;
    }

    bool parse_pasv(const std::string& response) {
        std::regex pattern(R"((\d+),(\d+),(\d+),(\d+),(\d+),(\d+))");
        std::smatch matches;

        if (!std::regex_search(response, matches, pattern)) {
            last_error = "无效的PASV响应格式";
            return false;
        }

        std::string ip = matches[1].str() + "." + matches[2].str() + "." 
                       + matches[3].str() + "." + matches[4].str();
        int port = (std::stoi(matches[5]) << 8) + std::stoi(matches[6]);

        if ((data_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            last_error = "创建数据socket失败: " + std::string(strerror(errno));
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            close(data_sock);
            last_error = "无效的IP地址格式";
            return false;
        }

        // 修正connect调用
        int ret=::connect(data_sock, reinterpret_cast<struct sockaddr*>(&addr),static_cast<socklen_t>(sizeof(addr)));
        if(ret==-1)
        {
            perror("connect");
            exit(0);
        }
        pasv_mode = true;
        return true;
    }

    void close_data_conn() {
        if (data_sock != -1) {
            close(data_sock);
            data_sock = -1;
        }
        pasv_mode = false;
    }

public:
    const std::string& get_last_error() const { return last_error; }
    bool connect(const std::string& server_ip = "127.0.0.1")
    {
        ctrl_sock = socket(AF_INET, SOCK_STREAM, 0);
        if(ctrl_sock==-1)
        {
            perror("socket");
            exit(0);
        }
        struct sockaddr_in addr;
        addr.sin_family=AF_INET;//IPV4
        addr.sin_port=htons(CONTROL_PORT);
        inet_pton(AF_INET,server_ip.c_str(),&addr.sin_addr.s_addr);
        int ret=::connect(ctrl_sock, 
                  reinterpret_cast<struct sockaddr*>(&addr),
                  static_cast<socklen_t>(sizeof(addr)));
        
        if(ret==-1)
        {
            perror("connect");
            exit(0);
        }
        std::cout<<"服务器响应: "<<std::endl;
        char buf[1024];
        memset(buf,0,sizeof(buf));
        ssize_t len=recv(ctrl_sock,buf,sizeof(buf),0);
        printf("recv buf:%s\n",buf);
        return true;
    }
    bool execute_command(const std::string& raw_cmd) {
        last_error.clear();
        std::istringstream iss(raw_cmd);
        std::string cmd;
        iss >> cmd;
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

        try {
            if (cmd == "PASV") {
                std::string response;
                // if (send_command("PASV", response) &&parse_pasv(response)) {
                //     std::cout << "进入被动模式: " << response << std::endl;
                //     return true;

                // }
                std::string full_cmd = cmd + "\r\n";
                send(ctrl_sock, full_cmd.c_str(), full_cmd.size(), 0);
                char buffer[DATA_BUFFER_SIZE];
                ssize_t bytes = recv(ctrl_sock, buffer, sizeof(buffer), 0);
                response=buffer;
                std::cout<<response<<std::endl;
                if (parse_pasv(response)) {
                    std::cout << "进入被动模式: " << response << std::endl;
                    return true;

                }
                else
                {
                    throw std::runtime_error("PASV命令失败");
                }
            }

            if (cmd == "LIST") {
                if (!pasv_mode) throw std::runtime_error("请先使用PASV模式");

                std::string response;
                //if (!send_command("LIST", response)) return false;
                std::string full_cmd = cmd + "\r\n";
                ssize_t sent = send(ctrl_sock, full_cmd.c_str(), full_cmd.size(), 0);
                if(sent>0)
                {
                    std::cout<<"list发送成功"<<std::endl;
                }
                if (sent < 0) throw std::runtime_error("发送命令失败");
                char buffer[DATA_BUFFER_SIZE];
                ssize_t total = 0;
                while (true) {
                    ssize_t bytes = recv(data_sock, buffer, sizeof(buffer), 0);
                    if (bytes < 0) throw std::runtime_error("接收数据失败");
                    if (bytes == 0) break;
                    
                    //std::cout.write(buffer, bytes);
                    std::cout<<buffer<<std::endl;
                    total += bytes;
                }

                std::string confirm;
                //if (!send_command("", confirm)) return false;
                std::cout << "传输完成: " << confirm << std::endl;

                close_data_conn();
                return true;
            }

            if (cmd == "RETR") {
                if (!pasv_mode) throw std::runtime_error("请先使用PASV模式");
                
                std::string filename;
                iss >> filename;
                if (filename.empty()) throw std::runtime_error("需要文件名参数");

                std::string response;
                //if (!send_command("RETR " + filename, response)) return false;

                std::ofstream file(filename, std::ios::binary);
                //if (!file) throw std::runtime_error("无法创建文件");
                std::string full_cmd = cmd + " "+filename+"\r\n";
                ssize_t sent = send(ctrl_sock, full_cmd.c_str(), full_cmd.size(), 0);

                char buffer[DATA_BUFFER_SIZE];
                ssize_t total = 0;
                while (true) {
                    ssize_t bytes = recv(data_sock, buffer, sizeof(buffer), 0);
                    if (bytes < 0) throw std::runtime_error("接收失败");
                    if (bytes == 0) break;
                    
                    file.write(buffer, bytes);
                    total += bytes;
                }
        //         char buffer[DATA_BUFFER_SIZE];
        //         ssize_t total = 0;
        //         while (file.read(buffer, sizeof(buffer))) {
        //             std::streamsize bytes_read = file.gcount();
        //             if (bytes_read <= 0) break;
        //             ssize_t total_sent = 0;
        //             // ssize_t sent = send(data_sock, buffer, bytes_read, MSG_NOSIGNAL);
        //             // if (sent < 0) throw std::runtime_error("发送失败");
        //             // total += sent;
        //             while (total_sent < bytes_read) {
        //     ssize_t sent = send(data_sock, 
        //                       buffer + total_sent, 
        //                       bytes_read - total_sent, 
        //                       MSG_NOSIGNAL);
        //     if (sent <= 0) {
        //         std::cerr << "发送失败: " << strerror(errno) << std::endl;
        //         break;
        //     }
        //     total_sent += sent;
        // }
                //}
                std::string confirm;
                if (!send_command("", confirm)) return false;
                std::cout << "下载完成: " << confirm << " (" << total << " bytes)" << std::endl;

                close_data_conn();
                return true;
            }

            if (cmd == "STOR") {
                if (!pasv_mode) throw std::runtime_error("请先使用PASV模式");
                
                std::string filename;
                iss >> filename;
                if (filename.empty()) throw std::runtime_error("需要文件名参数");

                std::ifstream file(filename, std::ios::binary);
                if (!file) throw std::runtime_error("文件不存在");

                std::string response;
                //if (!send_command("STOR " + filename, response)) return false;
                std::string full_cmd = cmd + " "+filename+"\r\n";
                ssize_t sent = send(ctrl_sock, full_cmd.c_str(), full_cmd.size(), 0);

        //         char buffer[DATA_BUFFER_SIZE];
        //         ssize_t total = 0;
                
        //         while (file.read(buffer, sizeof(buffer))) {
        //             std::streamsize bytes_read = file.gcount();
        //             total=bytes_read;
        //             if (bytes_read <= 0) break;
        //             ssize_t total_sent = 0;

        //             while (total_sent < bytes_read) {
        //     ssize_t sent = send(data_sock, 
        //                       buffer + total_sent, 
        //                       bytes_read - total_sent, 
        //                       MSG_NOSIGNAL);
        //     if (sent <= 0) {
        //         std::cerr << "发送失败: " << strerror(errno) << std::endl;
        //         break;
        //     }
        //     total_sent += sent;
        // }
    //      char buffer[DATA_BUFFER_SIZE];
    // ssize_t total = 0;
    
    // while (file.read(buffer, sizeof(buffer))) {
    //     std::streamsize bytes_read = file.gcount(); // 正确位置获取
    //     total=bytes_read;
    //     ssize_t total_sent = 0;
    //     while (total_sent < bytes_read) {
    //         ssize_t sent = send(data_sock, 
    //                           buffer + total_sent, 
    //                           bytes_read - total_sent, 
    //                           MSG_NOSIGNAL);
    //         if (sent <= 0) {
    //             throw std::runtime_error("发送失败");
    //         }
    //         total_sent += sent;
    //     }
    //     total += total_sent;
    // }
    char buffer[DATA_BUFFER_SIZE];
    ssize_t total = 0;
    
    while (file) {
        file.read(buffer, sizeof(buffer));
        std::streamsize bytes_read = file.gcount();
        
        if (bytes_read > 0) {
            ssize_t total_sent = 0;
            while (total_sent < bytes_read) {
                ssize_t sent = send(data_sock, 
                                  buffer + total_sent, 
                                  bytes_read - total_sent, 
                                  MSG_NOSIGNAL);
                if (sent <= 0) {
                    throw std::runtime_error("发送失败");
                }
                total_sent += sent;
            }
            total += total_sent;
        }
    }
                    // ssize_t sent = send(data_sock, buffer, file.gcount(), 0);
                    // if (sent < 0) throw std::runtime_error("发送失败");
                    // total += sent;
                

                std::string confirm;
                if (!send_command("", confirm)) return false;
                std::cout << "上传完成: " << confirm << " (" << total << " bytes)" << std::endl;

                close_data_conn();
                return true;
            }

           std::string response;
            if (!send_command(raw_cmd, response)) return false;
            std::cout << "服务器响应: " << response << std::endl;
            return true;

        } catch (const std::exception& e) {
            close_data_conn();
            last_error = e.what();
            return false;
        }
    }

    // void disconnect() {
    //     if (ctrl_sock != -1) {
    //         std::string response;
    //         send_command("QUIT", response);
    //         close(ctrl_sock);
    //         ctrl_sock = -1;
    //     }
    //     close_data_conn();
    // }
};

int main() {
    FTPClient client;
    if (!client.connect()) {
        std::cerr << "连接失败: " << client.get_last_error() << std::endl;
        return 1;
    }

    std::cout << "已连接到FTP服务器，输入命令开始操作" << std::endl;
    std::cout << "支持命令: PASV, LIST, RETR <file>, STOR <file>, QUIT" << std::endl;

    std::string command;
    while (true) {
        std::cout << "ftp> ";
        std::getline(std::cin, command);
        if (command.empty()) continue;
        if (command == "QUIT") break;

        if (!client.execute_command(command)) {
            std::cerr << "错误: " << client.get_last_error() << std::endl;
        }
    }
    //client.disconnect();
    std::cout << "连接已关闭" << std::endl;
    return 0;
}