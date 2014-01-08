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
#include <stdarg.h>
#include <limits.h>

#ifdef NDEBUG
#define REQUIRE(op) op
#else
#define REQUIRE(op)                             \
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
        REQUIRE(pthread_mutex_lock(&mMutex));
    }
    ~LockScope()
    {
        REQUIRE(pthread_mutex_unlock(&mMutex));
    }

    pthread_mutex_t mMutex;
};

NW::NW()
    : mRunning(false), mInterfacesDirty(false)
{
    signal(SIGPIPE, SIG_IGN);
    REQUIRE(pthread_mutex_init(&mMutex, 0));
    REQUIRE(pipe(mPipe));
}

NW::~NW()
{
    stop();
    close(mPipe[0]);
    close(mPipe[1]);
    REQUIRE(pthread_mutex_destroy(&mMutex));
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

int NW::Request::read(char *buf, int max)
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

NW::Request::Request(int socket, const NW::Interface &local, const NW::Interface &remote)
    : mSocket(socket), mLocalInterface(local), mRemoteInterface(remote),
      mBuffer(0), mBufferLength(0), mBufferCapacity(0), mState(ParseRequest),
      mMethod(NoMethod), mVersion(NoVersion), mConnection(NoConnection), mContentLength(-1)
{}

NW::Request::~Request()
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

    std::set<Request*> requests;
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
                    info("Listening on %s\n", it->toString().c_str());
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
        for (std::set<Request*>::const_iterator it = requests.begin(); it != requests.end(); ++it) {
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
        debug("Select: %d %c\n", count, pipe ? pipe : ' ');
        for (std::map<int, Interface>::const_iterator it = sockets.begin(); count && it != sockets.end(); ++it) {
            // uint8_t mode = 0;
            // if (FD_ISSET(it->first, &r)) {
            //     mode |= Read;
            // } else if (FD_ISSET(it->first, &w)) {
            //     mode |= Write;
            // }
            if (FD_ISSET(it->first, &r)) {
                --count;
                Request *conn = acceptConnection(it->first, it->second);
                if (conn) {
                    requests.insert(conn);
                }
            }
        }
        std::set<Request*>::iterator it = requests.begin();
        while (count && it != requests.end()) {
            if (FD_ISSET((*it)->socket(), &r)) {
                if (!processConnection(*it)) {
                    int ret;
                    EINTRWRAP(ret, ::close((*it)->socket()));
                    requests.erase(it++);
                } else {
                    ++it;
                }
                --count;
            } else {
                ++it;
            }
        }

    }
    for (std::map<int, Interface>::const_iterator it = sockets.begin(); it != sockets.end(); ++it) {
        ::close(it->first);
    }
    for (std::set<Request*>::const_iterator it = requests.begin(); it != requests.end(); ++it) {
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
        const bool space = isspace(static_cast<unsigned char>(*ch));
        if (space != (food == Space))
            return true;
        ++ch;
    }
    return false;
}

static inline bool parseRequestLine(const char *str,
                                    NW::Request::Method &method,
                                    NW::Request::Version &version,
                                    std::string &path)
{
    version = NW::Request::NoVersion;
    method = NW::Request::NoMethod;
    if (!strncmp(str, "GET ", 4)) {
        method = NW::Request::Get;
        str += 4;
    } else if (!strncmp(str, "HEAD ", 5)) {
        method = NW::Request::Head;
        str += 5;
    } else if (!strncmp(str, "POST ", 5)) {
        method = NW::Request::Post;
        str += 5;
    } else if (!strncmp(str, "PUT ", 4)) {
        method = NW::Request::Put;
        str += 4;
    } else if (!strncmp(str, "Delete ", 7)) {
        method = NW::Request::Delete;
        str += 7;
    } else if (!strncmp(str, "Trace ", 6)) {
        method = NW::Request::Trace;
        str += 6;
    } else if (!strncmp(str, "Connect ", 7)) {
        method = NW::Request::Connect;
        str += 7;
    } else {
        return false;
    }

    if (!eat(str, Space)) {
        return false;
    }
    const char *pathStart = str;
    if (!eat(str, NonSpace)) {
        return false;
    }
    path.assign(pathStart, str - pathStart);
    eat(str, Space);
    if (!strncmp("HTTP/1.", str, 7)) {
        switch (str[7]) {
        case '0':
            version = NW::Request::V1_0;
            break;
        case '1':
            version = NW::Request::V1_1;
            break;
        default:
            return false;
        }
        return true;
    }
    return false;
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

bool NW::processConnection(Request *connection)
{
    assert(connection);
    switch (connection->mState) {
    case Request::ParseRequest:
    case Request::ParseHeaders: {
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
            while (connection->mState != Request::ParseBody) {
                char *crlf = strstr(buf, "\r\n");
                if (crlf) {
                    *crlf = '\0';
                } else {
                    break;
                }
                switch (connection->mState) {
                case Request::ParseRequest:
                    if (!::parseRequestLine(connection->mBuffer, connection->mMethod, connection->mVersion, connection->mPath)) {
                        error("Parse error when parsing requestLine: [%s]\n",
                              std::string(buf, crlf - buf).c_str());
                        connection->mState = Request::ParseError;
                        return false;
                    } else {
                        connection->mState = Request::ParseHeaders;
                        printf("Got Method %s => %d [%s]\n", std::string(buf, crlf - buf).c_str(), connection->mMethod,
                               connection->mPath.c_str());
                        buf = crlf + 2;
                    }
                    break;
                case Request::ParseHeaders:
                    printf("Got header: [%s]\n", std::string(buf, crlf - buf).c_str());
                    if (crlf == buf) {
                        connection->mState = Request::ParseBody;
                    } else {
                        const char *colon = strnchr(buf, ':', crlf - buf);
                        connection->mHeaders.push_back(std::pair<std::string, std::string>());
                        std::pair<std::string, std::string> &h = connection->mHeaders.back();
                        if (colon) {
                            if (colon == buf) {
                                error("Parse error when parsing header: [%s]\n",
                                      std::string(buf, crlf - buf).c_str());
                                connection->mState = Request::ParseError;
                                return false;
                            }
                            // ### need to chomp spaces
                            h.first.assign(buf, colon - buf);
                            if (!strcasecmp(h.first.c_str(), "Content-Length")) {
                                char *end;
                                unsigned long contentLength = strtoull(h.second.c_str(), &end, 10);
                                if (*end) {
                                    error("Parse error when parsing Content-Length: [%s]\n",
                                          std::string(buf, crlf - buf).c_str());
                                    connection->mState = Request::ParseError;
                                    return false;
                                }

                                connection->mContentLength = std::min<unsigned long>(INT_MAX, contentLength); // ### probably lower cap
                            }
                            ++colon;
                            eat(colon, Space);
                            if (colon < crlf)
                                h.second.assign(colon, crlf - colon);
                        } else {
                            printf("%p %p %p %d\n", buf, crlf, colon, crlf - colon);
                            h.first.assign(buf, crlf - colon);
                        }
                        printf("[%s] [%s]\n", h.first.c_str(), h.second.c_str());
                        buf = crlf + 2;
                    }
                    break;
                case Request::ParseBody:
                case Request::ParseError:
                    assert(0);
                    break;
                }
            }

        }
        break; }
    case Request::ParseBody:
        break;
    case Request::ParseError:
        assert(0);
        break;
    }
    if (connection->mState == Request::ParseBody)
        handleConnection(connection);
}

NW::Request *NW::acceptConnection(int fd, const Interface &localInterface)
{
    sockaddr address;
    memset(&address, 0, sizeof(address));
    socklen_t len = sizeof(sockaddr);
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
    return new Request(socket, localInterface, remoteInterface);
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
    log(Log_Debug, format, v);
    va_end(v);
}

void NW::info(const char *format, ...)
{
    va_list v;
    va_start(v, format);
    log(Log_Info, format, v);
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
