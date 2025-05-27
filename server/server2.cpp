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

// 全局数据连接映射和互斥锁
std::unordered_map<int, std::weak_ptr<class ClientHandler>> data_listen_map;
std::mutex map_mutex;

class ClientHandler : public std::enable_shared_from_this<ClientHandler> {
//private:
public:
    int ctrl_sock;
    int data_listen_sock = -1;
    int data_sock = -1;
    int epoll_fd;
    std::string current_dir;
    std::mutex data_mutex;

    void send_response(const std::string& response) {
        std::string msg = response + "\r\n";
        send(ctrl_sock, msg.c_str(), msg.size(), 0);
    }

    bool is_safe_path(const std::string& path) {
        std::string full_path = current_dir + "/" + path;
        return full_path.find(ROOT_DIR) == 0;
    }

public:
    ClientHandler(int sock, int epoll) : ctrl_sock(sock), epoll_fd(epoll) {
        current_dir = ROOT_DIR;
        mkdir(ROOT_DIR, 0777);
    }

    ~ClientHandler() {
        close(ctrl_sock);
        cleanup_data_connection();
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
        } else if (command == "PASS") {
            send_response("230 Login successful");
        } else if (command == "PASV") {
            handle_pasv();
        } else if (command == "LIST") {
            handle_list();
        } //else if (command == "RETR" && tokens.size() > 1) {
           // handle_retr(tokens[1]);
        //} else if (command == "STOR" && tokens.size() > 1) {
           // handle_stor(tokens[1]);
        //}
         else if (command == "QUIT") {
            send_response("221 Goodbye");
        } else {
            send_response("500 Unknown command");
        }
    }

//private:
    void handle_pasv() {
        std::lock_guard<std::mutex> lock(data_mutex);
        cleanup_data_connection();

        // 创建数据监听socket
        data_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        if(data_listen_sock < 0) {
            send_response("500 Internal server error");
            return;
        }

        // 设置socket选项
        int opt = 1;
        setsockopt(data_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        //set_nonblock(data_listen_sock);
        int flags = fcntl(data_listen_sock, F_GETFL, 0);
    fcntl(data_listen_sock, F_SETFL, flags | O_NONBLOCK);

        // 绑定随机端口
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

        if(listen(data_listen_sock, 5) < 0) {
            send_response("500 Listen failed");
            close(data_listen_sock);
            data_listen_sock = -1;
            return;
        }

        // 注册到epoll
        struct epoll_event data_ev;
        data_ev.events = EPOLLIN | EPOLLET;
        data_ev.data.fd = data_listen_sock;
        if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, data_listen_sock, &data_ev) == 0) {
            {
                std::lock_guard<std::mutex> lock(map_mutex);
                data_listen_map[data_listen_sock] = weak_from_this();
            }
            
            // 获取端口信息
            sockaddr_in sin;
            socklen_t len = sizeof(sin);
            getsockname(data_listen_sock, (sockaddr*)&sin, &len);
            uint16_t port = ntohs(sin.sin_port);
            
            std::string ip_str = SERVER_IP;
            std::replace(ip_str.begin(), ip_str.end(), '.', ',');
            std::ostringstream oss;
            oss << "227 Entering Passive Mode (" 
                << ip_str << "," 
                << (port >> 8) << "," 
                << (port & 0xff) << ")";
            send_response(oss.str());
        } else {
            send_response("500 Internal server error");
            close(data_listen_sock);
            data_listen_sock = -1;
        }
    }

    void handle_list() {
        std::lock_guard<std::mutex> lock(data_mutex);
        if(!setup_data_connection()) return;

        send_response("150 Here comes the directory listing");
        
        DIR* dir = opendir(current_dir.c_str());
        if(dir) {
            std::string list;
            dirent* entry;
            while((entry = readdir(dir)) != nullptr) {
                if(strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
                    list += entry->d_name;
                    list += "\r\n";
                }
            }
            closedir(dir);
            send(data_sock, list.c_str(), list.size(), 0);
        }
        
        cleanup_data_connection();
        send_response("226 Directory send OK");
    }

    bool setup_data_connection() {
        if(data_listen_sock == -1) {
            send_response("425 Use PASV first");
            return false;
        }

        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        data_sock = accept(data_listen_sock, (sockaddr*)&client_addr, &addr_len);
        if(data_sock < 0) {
            send_response("425 Data connection failed");
            return false;
        }
        return true;
    }

    void cleanup_data_connection() {
        if(data_listen_sock != -1) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, data_listen_sock, nullptr);
            {
                std::lock_guard<std::mutex> lock(map_mutex);
                data_listen_map.erase(data_listen_sock);
            }
            close(data_listen_sock);
            data_listen_sock = -1;
        }
        if(data_sock != -1) {
            close(data_sock);
            data_sock = -1;
        }
    }

    // 其他处理函数保持不变...
};

void set_nonblock(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

void handle_signal(int sig) {
    server_running = false;
}

void handle_data_connection(int data_listen_fd) {
    while(true) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int data_sock = accept(data_listen_fd, (sockaddr*)&client_addr, &addr_len);
        if(data_sock < 0) {
            if(errno == EAGAIN) break;
            perror("data accept");
            continue;
        }

        std::shared_ptr<ClientHandler> handler;
        {
            std::lock_guard<std::mutex> lock(map_mutex);
            auto it = data_listen_map.find(data_listen_fd);
            if(it != data_listen_map.end()) {
                handler = it->second.lock();
            }
        }

        if(handler) {
            handler->process_command("LIST"); // 示例处理
        } else {
            close(data_sock);
        }
    }
}


int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    ThreadPool pool(THREAD_POOL_SIZE);
    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblock(server_fd);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(CONTROL_PORT);
    bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_fd, SOMAXCONN);

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    std::cout << "FTP Server started on port " << CONTROL_PORT << std::endl;

    while(server_running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);
        for(int i=0; i<n; i++) {
            int fd = events[i].data.fd;
            
            if(fd == server_fd) {
                while(true) {
                    sockaddr_in client_addr{};
                    socklen_t addr_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (sockaddr*)&client_addr, &addr_len);
                    if(client_fd < 0) {
                        if(errno == EAGAIN) break;
                        perror("accept");
                        continue;
                    }
                    
                    set_nonblock(client_fd);
                    auto handler = std::make_shared<ClientHandler>(client_fd, epoll_fd);
                    handler->send_response("220 Welcome");
                    
                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
                }
            }
            else if(events[i].events & EPOLLRDHUP) {
                close(fd);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            }
            else {
                std::shared_ptr<ClientHandler> handler;
                {
                    std::lock_guard<std::mutex> lock(map_mutex);
                    auto it = data_listen_map.find(fd);
                    if(it != data_listen_map.end()) {
                        handler = it->second.lock();
                    }
                }
                
                if(handler) {
                    handle_data_connection(fd);
                } else {
                    pool.enqueue([fd, epoll_fd]() {
                        char buffer[BUFFER_SIZE];
                        while(true) {
                            ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
                            if(bytes <= 0) {
                                if(bytes == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                                    close(fd);
                                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                                }
                                break;
                            }
                            
                            std::string cmd(buffer, bytes);
                            cmd.erase(cmd.find_last_not_of("\r\n") + 1);
                            
                            auto handler = std::make_shared<ClientHandler>(fd, epoll_fd);
                            handler->process_command(cmd);
                        }
                    });
                }
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
    return 0;
}