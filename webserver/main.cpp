#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <error.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include <signal.h>
#include "http_conn.h"

#define MAX_FD 65535  // 最大的文件描述数个数
#define MAX_EVENT_NUMBER 10000   // 监听的最大的事件数

// 添加信号捕捉
void addsig(int sig, void(*handler)(int)){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa)); // 清空sa
    sa.sa_handler = handler;  // 信号处理函数
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);  // 设置一个信号处理器
}

// 添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);

// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);

// 修改文件描述符
extern void modfd(int epollfd, int fd, int ev);

int main(int argc, char* argv[]) {

    if (argc <= 1) {
        printf("按照如下格式运行：%s port_number\n", basename(argv[0]));
        exit(-1);
    }
    
    // 获取端口号
    int port = atoi(argv[1]); // 字符串数字转换成整数

    // 对SIGPIPE信号进行处理
    addsig(SIGPIPE, SIG_IGN); // SIGPIPE信号，默认情况下，会终止进程，这里我们是设为ignore，忽略它，什么都不做，程序正常进行，要不然，开启的这个服务器程序会闪退

    // 创建线程池，初始化线程池
    threadpool<http_conn> * pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    }
    catch (...){
        exit(-1);
    }

    // 创建一个数组用于保存所有的客户端信息, users中每一个元素就是一个客户端的连接
    http_conn* users = new http_conn[MAX_FD];

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    // 设置端口复用
    // 1.防止服务器重启时之前绑定的端口还没释放 2.程序突然退出而系统没有释放端口
    // 针对的是服务器端socket的time_wait状态
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定 
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port); // 主机序转变为网络序
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    // 监听
    listen(listenfd, 5);  // 5是未连接的请求和已经连接的请求的和的最大值,可以 cat /proc/sys/net/core/somaxconn查看，本机是4096
                          // 但是一般设为5就够了，因为连接上的请求，accept会立即将它取走

    // 创建epoll对象，事件数组， 添加监听的文件描述符
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5); 

    // 将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn :: m_epollfd = epollfd;   // 哈哈哈, 还是将上面的listenfd加进来了，我以为不加呢，那这样的话，整个服务器端就一个epollfd

    while(true) {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1); // 检测到的事件的个数，这里是-1表示阻塞的
        if ((num < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        // 循环遍历事件数组
        for (int i = 0; i < num; i++) {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd) {
                // 有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);

                if (http_conn:: m_user_count >= MAX_FD) {
                    // 服务器目前很忙，连接数满了
                    close(connfd); // 所以将这个连接关闭
                    continue;
                }

                // 要将新的客户的数据初始化，放到数组中
                users[connfd].init(connfd, client_address);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 对方异常断开或者错误等事件
                users[sockfd].close_conn();

            }
            else if (events[i].events & EPOLLIN) {
                // 有读事件发生
                if (users[sockfd].read()) {
                    // 一次性把所有数据读完
                    pool -> append(users + sockfd);
                }
                else{
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT) {
                // 有写事件发生
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;
    return 0;
}