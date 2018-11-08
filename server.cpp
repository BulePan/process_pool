#include "process_pool.h"

class cgi_conn {
public:
    cgi_conn()
        : _sockfd{ -1 }, _read_idx{ -1 }
    {
        memset(_buf, 0, sizeof(_buf));
    }

    void init(int epollfd, int sockfd, const sockaddr_in & client_address)
    {
        _epollfd = epollfd;
        _sockfd = sockfd;
        _address = client_address;
        _read_idx = 0;
    }

    void process()
    {
        int idx = 0;
        int ret = -1;

        while(true)
        {
            idx = _read_idx;
            ret = recv(_sockfd, _buf+idx, BUFFER_SIZE-1-idx, 0);
            if(ret < 0)
            {
                if(errno != EAGAIN)
                    removefd(_epollfd, _sockfd);
                break;
            }
            else if(ret == 0)
            {
                removefd(_epollfd, _sockfd);
                break;
            }
            else 
            {
                _read_idx += ret;
                printf("user content is: %s", _buf);
                for(; idx < _read_idx; ++idx)
                {
                    if((idx >= 1) && (_buf[idx-1] == '\r') && (_buf[idx] == '\n'))
                        break;
                }
            }

            if(idx == _read_idx)
                continue;

            _buf[idx-1] = '\0';

            char *file_name = _buf;
            if(access(file_name, F_OK) == -1)
            {
                removefd(_epollfd, _sockfd);
                break;
            }

            ret = fork();
            if(ret == -1)
            {
                removefd(_epollfd, _sockfd);
                break;
            }
            else if(ret > 0)
            {
                removefd(_epollfd, _sockfd);
                break;
            }
            else 
            {
                close(STDOUT_FILENO);
                dup(_sockfd);
                execl(_buf, _buf, NULL);
                exit(0);
            }
        }
    }

private:
    static const int BUFFER_SIZE = 1024;
    static int _epollfd;
    int _sockfd;
    sockaddr_in _address;
    char _buf[BUFFER_SIZE];
    int _read_idx;
};

int cgi_conn::_epollfd = -1;

int main(int argc, char **argv)
{
    if(argc < 2)
    {
        printf("usage: %s port_number\n", basename(argv[0]));
        return -1;
    }

    int port = atoi(argv[1]);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int ret = 0;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    if(ret == -1)
    {
        printf("what: %m\n");
        return -1;
    }

    ret = listen(listenfd, 5);
    assert(ret != -1);

    process_pool<cgi_conn> * pool = process_pool<cgi_conn>::creat(listenfd);
    if(pool)
    {
        pool->run();
        delete pool;
    }

    close(listenfd);

    return 0;
}

