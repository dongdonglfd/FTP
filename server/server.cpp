#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#include <fstream>
#include <cstring>
#include <sys/stat.h>
#include <atomic>
#include<signal.h>
#include<algorithm>

#define CONTROL_PORT 2100
#define BUFFER_SIZE 1024
#define SERVER_IP "127.0.0.1"  // 服务器IP地址
#define ROOT_DIR "/tmp/ftproot" // 服务器根目录

std::atomic<bool> server_running(true); // 服务器运行状态标志

// 客户端会话处理类
class ClientHandler {
private:
    int ctrl_sock;      // 控制连接socket
    int data_listen_sock = -1; // 数据监听socket
    int data_sock = -1; // 数据连接socket
    std::string current_dir; // 当前工作目录
    std::mutex data_mutex;  // 数据连接互斥锁

    // 发送响应到客户端（自动添加CRLF）
    void send_response(const std::string& response) {
        std::string msg = response + "\n";
        send(ctrl_sock, msg.c_str(), msg.size(), 0);
    }

    // 路径安全检查（防止目录遍历）
    bool is_safe_path(const std::string& path) {
        std::string full_path = current_dir + "/" + path;
        return full_path.find(ROOT_DIR) == 0; // 必须包含根目录
    }

public:
    explicit ClientHandler(int sock) : ctrl_sock(sock) {
        current_dir = ROOT_DIR;
        mkdir(ROOT_DIR, 0777); // 确保根目录存在
    }

    ~ClientHandler() {
        close(ctrl_sock);
        if(data_listen_sock != -1) close(data_listen_sock);
        if(data_sock != -1) close(data_sock);
    }

    // 主处理循环
    void handle() {
        send_response("220 Welcome to MyFTP Server");

        char buffer[BUFFER_SIZE];
        while (server_running) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = recv(ctrl_sock, buffer, BUFFER_SIZE, 0);
            if (bytes <= 0) break;

            std::string cmd(buffer);
            cmd.erase(cmd.find_last_not_of("\r\n") + 1); // 清理命令结尾

            // 命令解析
            std::istringstream iss(cmd);//创建一个字符串流 iss，用于从字符串 cmd 中读取数据。
            std::vector<std::string> tokens;
            std::string token;
            while(iss >> token) tokens.push_back(token);//从字符串流 iss 中逐个读取令牌，并将其添加到 tokens 向量中。
            if(tokens.empty()) continue;

            std::string command = tokens[0];
            std::transform(command.begin(), command.end(), command.begin(), ::toupper);//用于对容器中的元素进行转换操作

            if (command == "USER") {
                send_response("331 Please specify the password");
            } 
            else if (command == "PASS") {
                send_response("230 Login successful");
            }
            else if (command == "PASV") {
                handle_pasv();
            }
            else if (command == "LIST") {
                handle_list();
            }
            else if (command == "RETR" && tokens.size() > 1) {
                handle_retr(tokens[1]);
            }
            else if (command == "STOR" && tokens.size() > 1) {
                handle_stor(tokens[1]);
            }
            else if (command == "QUIT") {
                send_response("221 Goodbye");
                break;
            }
            else {
                send_response("500 Unknown command");
            }
        }
    }

private:
    // 处理PASV命令（被动模式）
    void handle_pasv() {
        std::lock_guard<std::mutex> lock(data_mutex);
        
        if(data_listen_sock != -1) {
            close(data_listen_sock);
            data_listen_sock = -1;
        }

        // 创建数据监听socket
        data_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        if(data_listen_sock < 0) {
            send_response("500 Internal server error");
            return;
        }

        // 绑定随机端口
        sockaddr_in data_addr{};
        data_addr.sin_family = AF_INET;
        data_addr.sin_addr.s_addr = INADDR_ANY;
        data_addr.sin_port = 0; // 自动分配端口

        if(bind(data_listen_sock, (sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
            send_response("500 Port allocation failed");
            close(data_listen_sock);
            data_listen_sock = -1;
            return;
        }

        // 开始监听
        if(listen(data_listen_sock, 1) < 0) {
            send_response("500 Listen failed");
            close(data_listen_sock);
            data_listen_sock = -1;
            return;
        }

        // 获取绑定的端口号
        socklen_t addr_len = sizeof(data_addr);
        getsockname(data_listen_sock, (sockaddr*)&data_addr, &addr_len);//getsockname 可以用于获取绑定到套接字的实际地址和端口。
        uint16_t port = ntohs(data_addr.sin_port);// 获取端口号（网络字节序转主机字节序）

        // 构造响应字符串
        std::string ip_str = SERVER_IP;
        std::replace(ip_str.begin(), ip_str.end(), '.', ',');
        std::ostringstream oss;
        //在FTP的PASV模式响应中，端口号需要被分解为高8位和低8位两个部分
        oss << "227 Entering Passive Mode (" 
            << ip_str << "," 
            << (port >> 8) << "," //高8位
            << (port & 0xff) << ")";// 低8位
        
        send_response(oss.str());
    }

    // 处理LIST命令（目录列表）
    void handle_list() {
        std::lock_guard<std::mutex> lock(data_mutex);
        
        if(data_listen_sock == -1) {
            send_response("425 Use PASV first");
            return;
        }

        // 接受数据连接
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        data_sock = accept(data_listen_sock, 
                         (sockaddr*)&client_addr, &addr_len);
        if(data_sock < 0) {
            send_response("425 Data connection failed");
            return;
        }

        send_response("150 Here comes the directory listing");
        
        // 获取目录内容
        DIR* dir = opendir(current_dir.c_str());
        if(dir) {
            std::string list;
            dirent* entry;
            while((entry = readdir(dir)) != nullptr) {
                // 过滤特殊目录
                if(strcmp(entry->d_name, ".") == 0 || 
                   strcmp(entry->d_name, "..") == 0) continue;
                
                list += entry->d_name;
                list += "\r\n";
            }
            closedir(dir);
            send(data_sock, list.c_str(), list.size(), 0);
        }

        // 清理资源
        close(data_sock);
        close(data_listen_sock);
        data_sock = -1;
        data_listen_sock = -1;
        send_response("226 Directory send OK");
    }

    // 处理RETR命令（文件下载）
    void handle_retr(const std::string& filename) {
        if(!is_safe_path(filename)) {
            send_response("550 Invalid filename");
            return;
        }

        std::lock_guard<std::mutex> lock(data_mutex);
        
        if(data_listen_sock == -1) {
            send_response("425 Use PASV first");
            return;
        }

        // 建立数据连接
        data_sock = accept(data_listen_sock, nullptr, nullptr);
        if(data_sock < 0) {
            send_response("425 Data connection failed");
            return;
        }

        // 打开文件
        std::string fullpath = current_dir + "/" + filename;
        std::ifstream file(fullpath, std::ios::binary);//以二进制模式（std::ios::binary）打开文件。
        if(!file) {
            send_response("550 File not found");
            close(data_sock);
            return;
        }

        send_response("150 Opening binary mode data connection");
        
        // 分块传输文件
        char buffer[BUFFER_SIZE];
        while(file.read(buffer, BUFFER_SIZE)) {
            if(send(data_sock, buffer, file.gcount(), 0) < 0) {
                break;
            }
        }

        // 清理资源
        close(data_sock);
        close(data_listen_sock);
        data_sock = -1;
        data_listen_sock = -1;
        send_response("226 Transfer complete");
    }

    // 处理STOR命令（文件上传）
    void handle_stor(const std::string& filename) {
        if(!is_safe_path(filename)) {
            send_response("550 Invalid filename");
            return;
        }

        std::lock_guard<std::mutex> lock(data_mutex);
        
        if(data_listen_sock == -1) {
            send_response("425 Use PASV first");
            return;
        }

        // 建立数据连接
        data_sock = accept(data_listen_sock, nullptr, nullptr);
        if(data_sock < 0) {
            send_response("425 Data connection failed");
            return;
        }

        // 创建文件
        std::string fullpath = current_dir + "/" + filename;
        std::ofstream file(fullpath, std::ios::binary);
        if(!file) {
            send_response("550 Can't create file");
            close(data_sock);
            return;
        }

        send_response("150 Ready to receive data");
        
        // 接收数据
        char buffer[BUFFER_SIZE];
        ssize_t bytes_received;
        while((bytes_received = recv(data_sock, buffer, BUFFER_SIZE, 0)) > 0) {
            file.write(buffer, bytes_received);
        }

        // 清理资源
        close(data_sock);
        close(data_listen_sock);
        data_sock = -1;
        data_listen_sock = -1;
        send_response("226 Transfer complete");
    }
};

// 信号处理函数
void handle_signal(int sig) {
    server_running = false;
}

int main() {
    // 设置信号处理
    signal(SIGINT, handle_signal);// 捕获Ctrl+C
    signal(SIGTERM, handle_signal);// 捕获kill命令

    // 创建控制socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return 1;
    }

    // 设置socket选项
    int opt = 1;
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Setsockopt failed" << std::endl;
        close(server_fd);
        return 1;
    }

    // 绑定地址
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(CONTROL_PORT);

    if(bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        close(server_fd);
        return 1;
    }

    // 开始监听
    if(listen(server_fd, 5) < 0) {
        std::cerr << "Listen failed" << std::endl;
        close(server_fd);
        return 1;
    }

    std::cout << "FTP Server started on port " << CONTROL_PORT << std::endl;

    // 主循环接受连接
    while(server_running) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &addr_len);
        
        if(client_fd < 0) {
            if(server_running) std::cerr << "Accept failed" << std::endl;
            continue;
        }

        // 创建新线程处理客户端
        std::thread([client_fd]() {
            ClientHandler handler(client_fd);
            handler.handle();
        }).detach();
    }

    // 清理资源
    close(server_fd);
    std::cout << "\nServer stopped" << std::endl;
    return 0;
}# FTP
