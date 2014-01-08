#include "NW.h"
#include <map>
#include <set>
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

int NW::Connection::read(char *buf, int max)
{
    assert(buf); // ### should one be able to discard data?
    assert(max > 0);
    if (mBufferLength) {
        const int s = std::min(max, mBufferLength);
        ::memcpy(buf, mBuffer, s);
        buf += s;
        if (s == mBufferLength) {
            ::free(mBuffer);
            mBufferLength = 0;
            mBuffer = 0;
        } else {
            ::memmove(mBuffer + max, mBuffer, mBufferLength - max);
            mBufferLength -= max;
        }
        max -= s;
    }
    int ret = 0;
    if (max) {
        EINTRWRAP(ret, ::read(mSocket, buf, max));
    }
    return ret;
}

NW::Connection::Connection(int socket, const NW::Interface &local, const NW::Interface &remote)
    : mSocket(socket), mLocalInterface(local), mRemoteInterface(remote),
      mBuffer(0), mBufferLength(0), mBufferCapacity(0), mState(ParseMethod), mContentLength(-1)
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

    NW::Error retVal = Success;

    std::set<Connection*> connections;
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
                        error("::socket failed %d", errno);
                        retVal = SocketInit;
                        break;
                    }

                    int reuseaddr = 1;
                    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int))) {
                        error("::setsockopt failed %d", errno);
                        ::close(fd);
                        retVal = SocketInit;
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
                        error("::bind failed %d", errno);
                        retVal = SocketInit;
                        break;
                    }
                    enum { Backlog = 128 }; // ### configurable?
                    if (::listen(fd, Backlog)) {
                        ::close(fd);
                        error("::listen failed %d", errno);
                        retVal = SocketInit;
                        break;
                    }
                    sockets[fd] = *it;
                }
                if (retVal)
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
        for (std::set<Connection*>::const_iterator it = connections.begin(); it != connections.end(); ++it) {
            const int fd = (*it)->socket();
            FD_SET(fd, &r);
            FD_SET(fd, &w);
            max = std::max(max, fd);
        }
        int count = 0;
        EINTRWRAP(count, ::select(max + 1, &r, &w, 0, 0));
        assert(count > 0);
        char pipe = 0;
        if (FD_ISSET(mPipe[0], &r)) {
            int written = 0;
            EINTRWRAP(written, ::read(mPipe[0], &pipe, sizeof(pipe)));
            --count;
        }
        printf("%d %c\n", count, pipe ? pipe : ' ');
        for (std::map<int, Interface>::const_iterator it = sockets.begin(); count && it != sockets.end(); ++it) {
            // uint8_t mode = 0;
            // if (FD_ISSET(it->first, &r)) {
            //     mode |= Read;
            // } else if (FD_ISSET(it->first, &w)) {
            //     mode |= Write;
            // }
            if (FD_ISSET(it->first, &r)) {
                --count;
                Connection *conn = acceptConnection(it->first, it->second);
                if (conn) {
                    connections.insert(conn);
                }
            }
        }
        for (std::set<Connection*>::const_iterator it = connections.begin(); count && it != connections.end(); ++it) {
            if (FD_ISSET((*it)->socket(), &r)) {
                processConnection(*it);
                --count;
            }
        }

    }
    for (std::map<int, Interface>::const_iterator it = sockets.begin(); it != sockets.end(); ++it) {
        ::close(it->first);
    }
    for (std::set<Connection*>::const_iterator it = connections.begin(); it != connections.end(); ++it) {
        ::close((*it)->socket());
        delete *it;
    }

    return retVal;
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

enum Food {
    Space,
    NonSpace
};
static inline bool eat(const char *&ch, Food food)
{
    while (*ch) {
        if (isspace(static_cast<unsigned char>(*ch)) != (food == Space))
            return true;
        ++ch;
    }
    return false;
}

static inline bool parseRequestLine(const char *str, NW::Connection::Method &method, std::string &path)
{
    if (!strncmp(str, "GET ", 4)) {
        method = NW::Connection::Get;
        str += 4;
    } else if (!strncmp(str, "HEAD ", 5)) {
        method = NW::Connection::Head;
        str += 5;
    } else if (!strncmp(str, "POST ", 5)) {
        method = NW::Connection::Post;
        str += 5;
    } else if (!strncmp(str, "PUT ", 4)) {
        method = NW::Connection::Put;
        str += 4;
    } else if (!strncmp(str, "Delete ", 7)) {
        method = NW::Connection::Delete;
        str += 7;
    } else if (!strncmp(str, "Trace ", 6)) {
        method = NW::Connection::Trace;
        str += 6;
    } else if (!strncmp(str, "Connect ", 7)) {
        method = NW::Connection::Connect;
        str += 7;
    } else {
        method = NW::Connection::Invalid;
        return false;
    }
    if (!eat(str, Space))
        return false;
    const char *pathStart = str;
    if (!eat(str, NonSpace))
        return false;
    path.assign(pathStart, str - pathStart);
    eat(str, Space);
    return !strncmp("HTTP/1.1\r\n", str, 10);

    return true;
}

static inline const char *strnchr(const char *haystack, char needle, int max)
{
    assert(max >= 0);
    while (max > 0) {
        if (*haystack == needle) {
            return haystack;
        } else if (!*haystack) {
            break;
        }
        ++haystack;
        --max;
    }
    return 0;
}

void NW::processConnection(Connection *connection)
{
    assert(connection);
    switch (connection->mState) {
    case Connection::ParseMethod:
    case Connection::ParseHeaders: {
        enum { ChunkSize = 1024 };
        if (connection->mBufferCapacity - connection->mBufferLength < ChunkSize) {
            connection->mBufferCapacity = ChunkSize - (connection->mBufferCapacity - connection->mBufferLength);
            connection->mBuffer = static_cast<char*>(::realloc(connection->mBuffer, connection->mBufferCapacity));
        }
        int read;
        EINTRWRAP(read, ::read(connection->socket(), connection->mBuffer + connection->mBufferLength, ChunkSize - 1));
        if (read > 0) {
            connection->mBufferLength += read;
            connection->mBuffer[connection->mBufferLength] = '\0';
            char *buf = connection->mBuffer;
            while (connection->mState != Connection::ParseBody) {
                char *crlf = strstr(buf, "\r\n");
                switch (connection->mState) {
                case Connection::ParseMethod:
                    if (!::parseRequestLine(connection->mBuffer, connection->mMethod, connection->mPath)) {
                        error("Parse error when parsing requestLine: [%s]\n",
                              std::string(buf, crlf - buf).c_str());
                        connection->mState = Connection::ParseError;
                        return;
                    } else {
                        connection->mState = Connection::ParseHeaders;
                        printf("Got Method %s => %d [%s]\n", std::string(buf, crlf - buf).c_str(), connection->mMethod,
                               connection->mPath.c_str());
                        buf = crlf + 2;
                    }
                    break;
                case Connection::ParseHeaders:
                    printf("Got header: [%s]\n", std::string(buf, crlf - buf).c_str());
                    if (crlf == buf + 2) {
                        connection->mState = Connection::ParseBody;
                    } else {
                        const char *colon = strnchr(buf, ':', crlf - buf);
                        connection->mHeaders.push_back(std::pair<std::string, std::string>());
                        std::pair<std::string, std::string> &h = connection->mHeaders.back();
                        if (colon) {
                            if (colon == buf) {
                                error("Parse error when parsing header: [%s]\n",
                                      std::string(buf, crlf - buf).c_str());
                                connection->mState = Connection::ParseError;
                                return;
                            }
                            // ### need to chomp spaces
                            h.first.assign(buf, colon - buf);
                            if (!strcasecmp(h.first.c_str(), "Content-Length")) {
                                char *end;
                                unsigned long contentLength = strtoull(h.second.c_str(), &end, 10);
                                if (*end) {
                                    error("Parse error when parsing Content-Length: [%s]\n",
                                          std::string(buf, crlf - buf).c_str());
                                    connection->mState = Connection::ParseError;
                                    return;
                                }

                                connection->mContentLength = std::min<unsigned long>(INT_MAX, contentLength); // ### probably lower cap
                            }
                            ++colon;
                            eat(colon, Space);
                            if (colon < crlf)
                                h.second.assign(colon, crlf - colon);
                        } else {
                            h.first.assign(buf, crlf - colon);
                        }
                        printf("[%s] [%s]\n", h.first.c_str(), h.second.c_str());
                        buf = crlf + 2;
                    }
                    break;
                case Connection::ParseBody:
                case Connection::ParseError:
                    assert(0);
                    break;
                }
            }

        }
        break; }
    case Connection::ParseBody:
        break;
    case Connection::ParseError:
        assert(0);
        break;
    }
    if (connection->mState == Connection::ParseBody)
        handleConnection(connection);
}

NW::Connection *NW::acceptConnection(int fd, const Interface &localInterface)
{
    sockaddr address;
    socklen_t len;
    int socket;
    EINTRWRAP(socket, ::accept(fd, &address, &len));
    if (socket < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "::accept failed %d", errno);
        log(Log_Error, buf);
        return 0;
    }
    sockaddr_in *sockin = reinterpret_cast<sockaddr_in*>(&address);

    Interface remoteInterface;
    remoteInterface.port = ntohs(sockin->sin_port);
    const uint64_t ip = ntohl(sockin->sin_addr.s_addr);
    remoteInterface.ip.a = ip >> 24;
    remoteInterface.ip.b = (ip >> 16) & 0xff;
    remoteInterface.ip.c = (ip >> 8) & 0xff;
    remoteInterface.ip.d = ip & 0xff;

    debug("::accepted connection on %s from %s (%d)",
          localInterface.toString().c_str(),
          remoteInterface.toString().c_str(), socket);
    return new Connection(socket, localInterface, remoteInterface);
}

void NW::log(LogType level, const char *format, va_list v)
{
    va_list v2;
    va_copy(v2, v);
    enum { Size = 16384 };
    char buf[Size];
    char *msg = buf;
    int n = vsnprintf(msg, Size, format, v);
    if (n == -1) {
        va_end(v2);
        return;
    }

    if (n >= Size) {
        msg = new char[n + 2];
        n = vsnprintf(msg, n + 1, format, v2);
    }

    log(level, msg);
    if (msg != buf)
        delete []msg;
    va_end(v2);
}

void NW::debug(const char *format, ...)
{
    va_list v;
    va_start(v, format);
    log(Log_Error, format, v);
    va_end(v);
}

void NW::error(const char *format, ...)
{
    va_list v;
    va_start(v, format);
    log(Log_Error, format, v);
    va_end(v);
}

// bool NW::parseHeaders(Connection *conn)
// {
//     assert(!mReadHeaders);

//     char buf[1024];
//     int read;
//     while (true) {
//         EINTRWRAP(read, ::read(conn->socket(), buf, sizeof(buf)));
//         // if (read > 0) {
//         //     parseHeaders(connection, buf, read);
//         // } else {
//         //     break;
//         // }
//     }

//     printf("READ:\n%s\n", std::string(buf, len).c_str());
// }
