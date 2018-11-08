#pragma once

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

class process { // 描述一个子进程的类
public:
    pid_t _pid;    // 目标子进程的pid
    int _pipefd[2]; //父进程与子进程通信的管道
};

template<class T>
class process_pool
{
private:
    process_pool(int listenfd, int process_number);
    static process_pool<T> * _instance;
public:
    static process_pool<T> * creat(int listenfd, int process_number = 8)
    {
        if(_instance == nullptr)
            _instance = new process_pool<T>(listenfd, process_number);
        return _instance;
    }

    void run();

    ~process_pool() 
    {
        delete  [] _sub_process;
    }

private:
    //进程池的最大子进程数量
    static const int MAX_PROCESS_NUMBER = 16;
    
    //每个子进程最多能处理的客户数量
    static const int USER_PER_PROCESS = 65536;

    //外婆来了最多能处理的事件数
    static const  int MAX_EVENT_NUMBER = 10000;

    //进程池中的进程总数
    int _process_number;

    //子进程在池中的序号
    int _index;

    //每个进程都有一个epollfd
    int _epollfd;

    //监听套接字
    int _listenfd;

    //子进程通过_stop来决定是否停止运行
    bool _stop;

    //保存所有子进程的描述信息
    process * _sub_process;

    void setup_sig_pipe();
    void run_parent();
    void run_child();
};

template<class T>
process_pool<T> * process_pool<T>::_instance = nullptr;

static int sig_pipefd[2]; //用于处理信号的管道，以统一事件源，后面称之为信号管道

static int setnonbacking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
} 

static void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
}

static void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

static void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(sig_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

static void addsig(int sig, void (handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    if(restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

template<class T>
process_pool<T>::process_pool(int listenfd, int process_number)
    : _listenfd{ listenfd }, _process_number{ process_number }, _index{ -1 }, _stop{ false }
{
    assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));
    _sub_process = new process[_process_number];
    for(int i = 0; i < _process_number; ++i)
    {
        int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, _sub_process[i]._pipefd);
        assert(ret != -1);
        
        _sub_process[i]._pid = fork();
        assert(_sub_process[i]._pid >= 0);

        if(_sub_process[i]._pid > 0)
        {
            close(_sub_process[i]._pipefd[1]);
            continue;
        }
        else 
        {
            close(_sub_process[i]._pipefd[0]);
            _index = i; //子进程赋予自己的index
            break; //子进程需要退出循环，不然子进程也会fork()
        }
    }
}

template<class T>
void process_pool<T>::setup_sig_pipe()
{
    _epollfd = epoll_create(1);
    assert(_epollfd != -1);
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
    assert(ret != -1);
    setnonbacking(sig_pipefd[1]);
    addfd(_epollfd, sig_pipefd[0]);
    addsig(SIGCHLD, sig_handler);
    addsig(SIGTERM, sig_handler);
    addsig(SIGINT, sig_handler);
    addsig(SIGPIPE, SIG_IGN);
}

template<class T>
void process_pool<T>::run()
{
    _index == -1 ? run_parent() : run_child();
}

template<class T>
void process_pool<T>::run_child()
{
    setup_sig_pipe();
    
    //每个子进程通过进程池中的序号指找到与父进程通信的管道
    int pipefd = _sub_process[_index]._pipefd[1];
    //子进程需要监听管道描述符，因为父进程通过它来通知子进程新连接
    addfd(_epollfd, pipefd);

    epoll_event events[MAX_EVENT_NUMBER];
    //每个子进程负责的客户数组
    T* users = new T[USER_PER_PROCESS];

    int number = 0;
    int ret = -1;

    while(!_stop)
    {
        number = epoll_wait(_epollfd, events, MAX_EVENT_NUMBER, -1);
        if((number < 0) && (errno != EINTR))
        {
            perror("epoll error");
            break;
        }

        for(int i = 0; i < number; ++i)
        {
            int sockfd = events [i].data.fd;
            if((sockfd == pipefd) && (events[i].events & EPOLLIN))
            {
                int client = 0;
                //从父子进程的管道中读取数据，如果读取成功，则表示有新客户连接到服务器
                ret = recv(sockfd, (char*)&client, sizeof(client), 0);
                if(((ret < 0) && (errno != EAGAIN)) || ret == 0)
                    continue;
                else
                {
                    struct sockaddr_in client_address; 
                    socklen_t len = sizeof(sockaddr_in);
                    int connfd = accept(_listenfd, (struct sockaddr *)&client_address, &len);
                    if(connfd < 0)
                    {
                        perror("accept error");
                        continue; 
                    }
                    addfd(_epollfd, connfd);
                    //模板类T必须要实现init方法，以初始化一个客户连接，
                    //我们直接使用connfd来索引逻辑处理对象（T类型的对象），
                    //以提高程序效率
                    users[connfd].init(_epollfd, connfd, client_address);
                } 
            }
            else if((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                //子进程收到的信号
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if(ret <= 0)
                    continue;
                else 
                {
                    for(int i = 0; i < ret; ++i)
                    {
                        switch(signals[i])
                        {
                        case SIGCHLD:
                            pid_t pid;
                            int stat;
                            while((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                                continue;
                            break;
                        case SIGTERM:
                        case SIGINT:
                            _stop = true;
                            break;
                        default:
                            break;

                        }
                    }
                }
            }
            else if(events[i].events & EPOLLIN)
            {
                //其他数据，那么必然是客户请求到来，调用逻辑处理对象的方法处理之
                users[sockfd].process();
            }
            else
                continue;
        }
    }

    delete  []users;

    users = nullptr;
    close(pipefd);
    close(_epollfd);
}

template<class T>
void process_pool<T>::run_parent()
{
    setup_sig_pipe(); 

    addfd(_epollfd, _listenfd);

    epoll_event events[MAX_EVENT_NUMBER];

    int sub_process_counter = 0;
    int number = 0;
    int ret = -1;

    while(!_stop)
    {
        number = epoll_wait(_epollfd, events, MAX_EVENT_NUMBER, -1);
        if(number < 0 && errno != EINTR)
        {
            perror("epoll_wait error");
            break;
        }

        for(int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            if(sockfd == _listenfd)
            {
                //如果有新连接到来，采用Round Robin(轮询调度)的方式为其分配一个子进程
                int i = sub_process_counter;
                do{
                    if(_sub_process[i]._pid != -1)
                        break;//选一个仍然存活的进程
                    i = (i+1) % _process_number;
                }while(i != sub_process_counter);
                //
                if(_sub_process[i]._pid == -1) //没有一个存活的进程
                {
                    _stop == true;
                    break;
                }

                sub_process_counter = (i+1) % _process_number;
                int new_conn = 1;
                //随便发送一个数据(信号)，让子进程识别
                send(_sub_process[i]._pipefd[0], (char*)&new_conn, sizeof(new_conn), 0);
                printf("send request to child : %d\n", i);
            }
            else if((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
            {//产生了一个信号
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if(ret <= 0)
                    continue;
                else 
                {
                    for(int i = 0; i < ret; ++i)
                    {
                        switch(signals[i])
                        {
                        case SIGCHLD:
                        {
                            pid_t pid;
                            int stat;
                            while((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                            {
                                for(int i = 0; i < _process_number; ++i)
                                {
                                    if(_sub_process[i]._pid == pid)
                                    {
                                        //如果进程池中第i个子进程退出了，则主进程关闭相应通信管道，
                                        //并设置pid为-1，标记该子进程已退出
                                        printf("child %d join\n", i);
                                        close(_sub_process[i]._pipefd[0]);
                                         _sub_process[i]._pid = -1;
                                     }
                                 }
                            }
                            _stop = true;
                            for(int i = 0; i < _process_number; ++i)
                            {
                                //如果所有子进程都退出了，父进程也退出
                                 if(_sub_process[i]._pid != -1)
                                    _stop = false;
                            }
                            break;
                        }
                        case SIGTERM:
                        case SIGINT:
                            printf("kill all the child now\n");
                            for(int i = 0; i < _process_number; ++i)
                            {
                                //如果父进程收到终止信号，那么就杀死所有子进程，并等待他们全部结束，
                                //当然通知子进程结束的更好，方法是向父子进程的管道中发送特殊数据
                                int pid = _sub_process[i]._pid;
                                if(pid != -1)
                                     kill(pid, SIGTERM);
                            }
                            break;
                        default:
                            break;
                          }
                     }
                } 
            }
            else
                continue;
        }
    }
    close(_epollfd);
}
