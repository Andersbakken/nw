#include "NW.h"
#include <map>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/un.h>
#include <netinet/in.h>

#ifdef NDEBUG
#define CHECK(op) op
#else
#define CHECK(op)                               \
    do {                                        \
        const int retVal = op;                  \
        assert(!retVal);                        \
    } while (0)
#endif

#define EINTRWRAP(var, op)                      \
    do {                                        \
        var = op;                               \
    } while (var == -1 && errno == EINTR);

struct LockScope
{
    LockScope(pthread_mutex_t &mutex)
        : mMutex(mutex)
    {
        CHECK(pthread_mutex_lock(&mMutex));
    }
    ~LockScope()
    {
        CHECK(pthread_mutex_unlock(&mMutex));
    }

    pthread_mutex_t mMutex;
};

NW::NW()
    : mRunning(false), mInterfacesDirty(false)
{
    signal(SIGPIPE, SIG_IGN);
    CHECK(pthread_mutex_init(&mMutex, 0));
    CHECK(pipe(mPipe));
}

NW::~NW()
{
    stop();
    close(mPipe[0]);
    close(mPipe[1]);
    CHECK(pthread_mutex_destroy(&mMutex));
}

void NW::setInterfaces(const std::vector<Interface> &interfaces)
{
    {
        LockScope scope(mMutex);
        mInterfaces = interfaces;
        mInterfacesDirty = true;
        if (mRunning)
            wakeup('i');
    }
}

void NW::stop()
{
    bool join = false;
    {
        LockScope scope(mMutex);
        if (mRunning) {
            mRunning = false;
            if (!pthread_equal(pthread_self(), mThread)) {
                wakeup('q');
                join = true;
            }
        }
    }
    if (join) {
        void *retval;
        pthread_join(mThread, &retval);
    }
}

int NW::Connection::contentLength() const
{

}

int NW::Connection::read(char *buf, int max)
{

}

NW::Connection::Connection(int socket, const NW::Interface &local, const NW::Interface &remote)
    : mSocket(socket), mLocalInterface(local), mRemoteInterface(remote), mBuffer(0), mBufferLength(0)
{}

NW::Connection::~Connection()
{
    if (mBuffer)
        ::free(mBuffer);
}

NW::Error NW::exec()
{
    {
        LockScope scope(mMutex);
        assert(!mRunning);
        mThread = pthread_self();
        mRunning = true;
    }

    NW::Error error = Success;

    std::map<int, Connection*> connections;
    std::map<int, Interface> sockets;
    while (true) {
        {
            LockScope scope(mMutex);
            if (!mRunning) {
                mRunning = false;
                break;
            }
            if (mInterfacesDirty) {
                mInterfacesDirty = false;
                // do I need to close all current connections on interfaces that
                // have disappeared?

                std::map<int, Interface>::iterator it = sockets.begin();
                while (it != sockets.end()) {
                    ::close(it->first);
                    sockets.erase(it++);
                }
                int idx = 0;
                for (std::vector<Interface>::const_iterator it = mInterfaces.begin(); it != mInterfaces.end(); ++it) {
                    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                    if (fd < 0) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "::socket failed %d", errno);
                        log(Error, buf);
                        error = SocketInit;
                        break;
                    }

                    int reuseaddr = 1;
                    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int))) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "::setsockopt failed %d", errno);
                        log(Error, buf);
                        ::close(fd);
                        error = SocketInit;
                        break;
                    }

                    sockaddr_in address;
                    ::memset(&address, 0, sizeof(address));
                    address.sin_family = AF_INET;
                    address.sin_port = htons(it->port);
                    if (it->ip == IP::any()) {
                        address.sin_addr.s_addr = INADDR_ANY;
                    } else {
                        address.sin_addr.s_addr = htonl((it->ip.a << 24) | (it->ip.b << 16) | (it->ip.c << 8) | it->ip.d);
                    }

                    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
                        ::close(fd);
                        char buf[128];
                        snprintf(buf, sizeof(buf), "::bind failed %d", errno);
                        log(Error, buf);
                        error = SocketInit;
                        break;
                    }
                    if (::listen(fd, it->backlog)) {
                        ::close(fd);
                        char buf[128];
                        snprintf(buf, sizeof(buf), "::listen failed %d", errno);
                        log(Error, buf);
                        error = SocketInit;
                        break;
                    }
                }
                if (error)
                    break;
            }
        }
        fd_set r, w;
        FD_ZERO(&r);
        FD_ZERO(&w);
        FD_SET(mPipe[0], &r);
        int max = mPipe[0];
        for (std::map<int, Interface>::const_iterator it = sockets.begin(); it != sockets.end(); ++it) {
            FD_SET(it->first, &r);
            FD_SET(it->first, &w);
            max = std::max(max, it->first);
        }
        int count = 0;
        EINTRWRAP(count, ::select(max, &r, &w, 0, 0));
        assert(count > 0);
        char pipe = 0;
        if (FD_ISSET(mPipe[0], &r)) {
            int written = 0;
            EINTRWRAP(written, ::read(mPipe[0], &pipe, sizeof(pipe)));
            --count;
        }
        for (std::map<int, Interface>::const_iterator it = sockets.begin(); count && it != sockets.end(); ++it) {
            // uint8_t mode = 0;
            // if (FD_ISSET(it->first, &r)) {
            //     mode |= Read;
            // } else if (FD_ISSET(it->first, &w)) {
            //     mode |= Write;
            // }
            if (FD_ISSET(it->first, &r)) {
                --count;
                acceptConnection(it->first, it->second);
            }
        }
    }
    {
        for (std::map<int, Interface>::const_iterator it = sockets.begin(); it != sockets.end(); ++it) {
            ::close(it->first);
        }
    }

    {
        for (std::map<int, Connection*>::const_iterator it = connections.begin(); it != connections.end(); ++it) {
            ::close(it->first);
            delete it->second;
        }
    }


    return error;
}

void NW::wakeup(char byte)
{
    int ret;
    EINTRWRAP(ret, write(mPipe[1], &byte, sizeof(byte)));
    assert(ret == 1);
    (void)ret;
}

std::vector<NW::Interface> NW::interfaces() const
{
    LockScope scope(mMutex);
    return mInterfaces;
}

void NW::processConnection(Connection *connection)
{

}

void NW::acceptConnection(int fd, const Interface &localInterface)
{
    sockaddr address;
    socklen_t len;
    int fd;
    EINTRWRAP(fd, ::accept(listener->sock, &address, &len));
    if (fd < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "::accept failed %d", errno);
        log(Error, buf);
        return;
    }

    conn->request_info.remote_port = ntohs(conn->client.rsa.u.sin.sin_port);
    memcpy(&conn->request_info.remote_ip,
           &conn->client.rsa.u.sin.sin_addr.s_addr, 4);
    conn->request_info.remote_ip = ntohl(conn->request_info.remote_ip);

    Connection (

}
