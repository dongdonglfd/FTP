#include <iostream>
#include <string>
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
#include <signal.h>
#include <fcntl.h>
#include <algorithm>
#include <sys/epoll.h>
#include <unordered_map>
#include <memory>
#include "threadpool.h"

#define CONTROL_PORT 2100
#define BUFFER_SIZE 1024
#define MAX_EVENTS 1024
#define THREAD_POOL_SIZE 4
#define SERVER_IP "127.0.0.1"
#define ROOT_DIR "/home/lfd/FTP/server"

std::atomic<bool> server_running(true);

class ClientHandler {
private:
    int ctrl_sock;              // 控制连接socket
    int data_listen_sock = -1;  // 数据监听socket
    int data_sock = -1;         // 数据传输socket
    std::string current_dir;    // 当前工作目录
    std::mutex data_mutex;      // 数据连接互斥锁
    int epoll_fd_;              // epoll实例文件描述符

    // 设置非阻塞socket
    void set_nonblock(int sock) {
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    // void send_response(const std::string& response) {
    //     std::string msg = response + "\r\n";
    //     send(ctrl_sock, msg.c_str(), msg.size(), 0);
    // }

    bool is_safe_path(const std::string& path) {
        std::string full_path = current_dir + "/" + path;
        return full_path.find(ROOT_DIR) == 0;
    }

public:
    explicit ClientHandler(int sock, int epoll_fd) 
        : ctrl_sock(sock), epoll_fd_(epoll_fd) {
        current_dir = ROOT_DIR;
        mkdir(ROOT_DIR, 0777);
    }

    ~ClientHandler() {
        close(ctrl_sock);
        if(data_listen_sock != -1) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, data_listen_sock, nullptr);
            close(data_listen_sock);
        }
        if(data_sock != -1) close(data_sock);
    }
    void send_response(const std::string& response) {
        std::string msg = response + "\r\n";
        send(ctrl_sock, msg.c_str(), msg.size(), 0);
    }
    void process_command(const std::string& cmd) {
        std::istringstream iss(cmd);
        std::vector<std::string> tokens;
        std::string token;
        while(iss >> token) tokens.push_back(token);
        if(tokens.empty()) return;

        std::string command = tokens[0];
        std::transform(command.begin(), command.end(), command.begin(), ::toupper);

        if (command == "USER") {
            send_response("331 Please specify the password");
        } 
        else if (command == "PASS") {
            send_response("230 Login successful");
        }
        else if (command == "PASV") {
            std::cout<<"222"<<std::endl;
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
        }
        else {
            send_response("500 Unknown command");
        }
    }

private:
    void handle_pasv() {
        std::lock_guard<std::mutex> lock(data_mutex);
        
        // 清理旧连接
        if(data_listen_sock != -1) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, data_listen_sock, nullptr);
            close(data_listen_sock);
            data_listen_sock = -1;
        }

        // 创建并配置数据监听socket
        data_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        if(data_listen_sock < 0) {
            send_response("500 Internal server error");
            return;
        }

        set_nonblock(data_listen_sock);  // 设置非阻塞模式

        sockaddr_in data_addr{};
        data_addr.sin_family = AF_INET;
        data_addr.sin_addr.s_addr = INADDR_ANY;
        data_addr.sin_port = 0;

        if(bind(data_listen_sock, (sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
            send_response("500 Port allocation failed");
            close(data_listen_sock);
            data_listen_sock = -1;
            return;
        }

        if(listen(data_listen_sock, 1) < 0) {
            send_response("500 Listen failed");
            close(data_listen_sock);
            data_listen_sock = -1;
            return;
        }

        // 注册到epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = data_listen_sock;
        if(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, data_listen_sock, &ev) < 0) {
            send_response("500 Internal server error");
            return;
        }

        // 获取端口信息
        sockaddr_in sin;
        socklen_t len = sizeof(sin);
        getsockname(data_listen_sock, (sockaddr*)&sin, &len);
        uint16_t port = ntohs(sin.sin_port);
        std::cout<<port<<"*"<<std::endl;
        std::ostringstream oss;
        oss << "227 Entering Passive Mode (" 
            << replace_ip(SERVER_IP) << "," 
            << (port >> 8) << "," << (port & 0xff) << ")";
        send_response(oss.str());
    }

    void handle_list() {
        std::lock_guard<std::mutex> lock(data_mutex);
        
        if(data_listen_sock == -1) {
            send_response("425 Use PASV first");
            return;
        }

        // 等待数据连接（由epoll事件触发）
        struct epoll_event events[1];
        int n = epoll_wait(epoll_fd_, events, 1, 5000); // 5秒超时
        
        if(n <= 0) {
            send_response("425 Data connection timeout");
            return;
        }

        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        data_sock = accept(data_listen_sock, (sockaddr*)&client_addr, &addr_len);
        if(data_sock < 0) {
            send_response("425 Data connection failed");
            return;
        }

        send_response("150 Here comes the directory listing");
        
        // 生成目录列表
        std::string list;
        DIR* dir = opendir(current_dir.c_str());
        if(dir) {
            dirent* entry;
            while((entry = readdir(dir)) != nullptr) {
                if(strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
                    list += entry->d_name;
                    list += "\r\n";
                }
            }
            closedir(dir);
        }

        // 发送数据
        send_data(list);
        send_response("226 Directory send OK");
    }

    void send_data(const std::string& data) {
        size_t total_sent = 0;
        const char* buf = data.c_str();
        size_t remaining = data.size();
        
        while(remaining > 0) {
            ssize_t sent = send(data_sock, buf + total_sent, remaining, MSG_NOSIGNAL);
            if(sent < 0) {
                if(errno == EAGAIN) continue;
                break;
            }
            total_sent += sent;
            remaining -= sent;
        }
        
        close(data_sock);
        data_sock = -1;
    }

    void handle_retr(const std::string& filename) {
        if(!is_safe_path(filename)) {
            send_response("550 Invalid filename");
            return;
        }

        std::lock_guard<std::mutex> lock(data_mutex);
        
        // [数据连接处理类似handle_list...]
        // ...
    }

    void handle_stor(const std::string& filename) {
        if(!is_safe_path(filename)) {
            send_response("550 Invalid filename");
            return;
        }

        std::lock_guard<std::mutex> lock(data_mutex);
        
        // [数据连接处理类似handle_list...]
        // ...
    }

    // 辅助函数
    std::string replace_ip(const std::string& ip) {
        std::string s(ip);
        std::replace(s.begin(), s.end(), '.', ',');
        return s;
    }
};
ThreadPool* g_thread_pool = nullptr;
void handle_signal(int sig) {
    server_running = false;
}
void set_nonblock(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}
int main() {
    // [保留原有信号处理设置]
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    // 创建线程池
    ThreadPool pool(THREAD_POOL_SIZE);
    g_thread_pool = &pool;

    // 创建epoll实例
    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    // [保留服务器socket创建和绑定代码]
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
    if(listen(server_fd, SOMAXCONN) < 0) {
        std::cerr << "Listen failed" << std::endl;
        close(server_fd);
        return 1;
    }
    set_nonblock(server_fd);  // 设置非阻塞

    // 注册服务器socket到epoll
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    std::cout << "FTP Server started on port " << CONTROL_PORT << std::endl;

    while(server_running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);
        
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                // 处理新连接
                while(true) {
                    sockaddr_in client_addr{};
                    socklen_t addr_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, 
                                        (sockaddr*)&client_addr, &addr_len);
                    
                    if (client_fd < 0) {
                        if (errno == EAGAIN) break;  // 处理非阻塞返回
                        perror("accept");
                        continue;
                    }

                    set_nonblock(client_fd);
                    
                    // 注册客户端socket到epoll
                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);

                    // 创建处理器（传入epoll_fd）
                    auto handler = new ClientHandler(client_fd, epoll_fd);
                    handler->send_response("220 Welcome to MyFTP Server");
                }
            } else {
                int client_fd = events[i].data.fd;
                
                // 处理连接关闭
                if (events[i].events & EPOLLRDHUP) {
                    close(client_fd);
                    continue;
                }

                // 提交任务到线程池
                pool.enqueue([client_fd, epoll_fd]() {
                    char buffer[BUFFER_SIZE];
                    ssize_t bytes;
                    
                    // ET模式需要循环读取
                    while ((bytes = recv(client_fd, buffer, BUFFER_SIZE, 0)) > 0) {
                        std::string cmd(buffer, bytes);
                        cmd.erase(cmd.find_last_not_of("\r\n") + 1);

                        ClientHandler handler(client_fd, epoll_fd);
                        handler.process_command(cmd);
                    }

                    // 处理连接关闭
                    if (bytes == 0 || (bytes < 0 && errno != EAGAIN)) {
                        close(client_fd);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    }
                });
            }
        }
    }

    // [保留清理代码]
    close(epoll_fd);
    return 0;
}