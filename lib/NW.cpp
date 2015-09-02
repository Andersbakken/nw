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
    : mRunning(false), mInterfacesDirty(false), mMaxContentLength(INT_MAX)
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

std::vector<NW::Interface> NW::interfaces() const
{
    LockScope scope(mMutex);
    return mInterfaces;
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

int NW::maxContentLength() const
{
    LockScope scope(mMutex);
    return mMaxContentLength;
}

void NW::setMaxContentLength(int maxContentLength)
{
    LockScope scope(mMutex);
    mMaxContentLength = maxContentLength;
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

NW::Error NW::exec()
{
    {
        LockScope scope(mMutex);
        assert(!mRunning);
        mThread = pthread_self();
        mRunning = true;
    }

    NW::Error retVal = Success;

    std::set<std::shared_ptr<Request> > requests;
    std::map<int, std::shared_ptr<Connection> > connections;

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
        fd_set r;
        FD_ZERO(&r);
        FD_SET(mPipe[0], &r);
        int max = mPipe[0];
        for (std::map<int, Interface>::const_iterator it = sockets.begin(); it != sockets.end(); ++it) {
            FD_SET(it->first, &r);
            max = std::max(max, it->first);
        }
        for (std::set<std::shared_ptr<Request> >::const_iterator it = requests.begin(); it != requests.end(); ++it) {
            const int fd = (*it)->socket();
            FD_SET(fd, &r);
            max = std::max(max, fd);
        }
        int count = 0;
        EINTRWRAP(count, ::select(max + 1, &r, 0, 0, 0));
        assert(count > 0);
        char pipe = 0;
        if (FD_ISSET(mPipe[0], &r)) {
            int written = 0;
            EINTRWRAP(written, ::read(mPipe[0], &pipe, sizeof(pipe)));
            --count;
        }
        debug("Select: %d %c\n", count, pipe ? pipe : ' ');
        for (std::map<int, Interface>::const_iterator it = sockets.begin(); count && it != sockets.end(); ++it) {
            if (FD_ISSET(it->first, &r)) {
                --count;
                std::shared_ptr<Connection> conn = acceptConnection(it->first, it->second);
                if (conn) {
                    connections[conn->fd] = conn;
                }
            }
        }
        std::map<int, std::shared_ptr<Connection> >::const_iterator it = connections.begin();
        while (count && it != connections.end()) {
            if (FD_ISSET(it->first, &r)) {
                if (!processConnection(it->second)) {
                    int ret;
                    EINTRWRAP(ret, ::close(it->first));
                    connections.erase(it++);
                } else {
                    ++it;
                }
                --count;
            }

        }
    }
    for (std::map<int, Interface>::const_iterator it = sockets.begin(); it != sockets.end(); ++it) {
        ::close(it->first);
    }
    for (std::set<std::shared_ptr<Request> >::const_iterator it = requests.begin(); it != requests.end(); ++it) {
        ::close((*it)->socket());
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

static std::string trim(const char *start, int len)
{
    while (len && isspace(static_cast<unsigned char>(*start))) {
        --len;
        ++start;
    }

    while (len && isspace(static_cast<unsigned char>(*start + len - 1))) {
        --len;
    }

    return std::string(start, len);
}

bool NW::processConnection(const std::shared_ptr<Connection> &conn)
{
    assert(conn);
    int needed;
    if (conn->request && conn->request->mContentLength != -1) {
        needed = conn->request->mContentLength - conn->mBufferLength;
    } else {
        needed = 1025;
    }
    if (request->mBufferCapacity - request->mBufferLength < needed) {
        request->mBufferCapacity = needed - (request->mBufferCapacity - request->mBufferLength);
        request->mBuffer = static_cast<char*>(::realloc(request->mBuffer, request->mBufferCapacity));
    }
    int read;
    EINTRWRAP(read, ::read(request->socket(), request->mBuffer + request->mBufferLength, needed - 1)); // leave room for zero-termination
    if (!read) {
        return false; // socket was closed
    }
    request->mBufferLength += read;
    request->mBuffer[request->mBufferLength] = '\0';

    switch (request->mState) {
    case Request::ParseRequestLine:
    case Request::ParseHeaders: {
        char *buf = request->mBuffer;
        while (request->mState != Request::ParseBody) {
            char *crlf = strstr(buf, "\r\n");
            if (crlf) {
                *crlf = '\0';
            } else {
                break;
            }
            switch (request->mState) {
            case Request::ParseRequestLine:
                if (!::parseRequestLine(request->mBuffer, request->mMethod, request->mVersion, request->mPath)) {
                    error("Parse error when parsing requestLine: [%s]\n", buf);
                    request->mState = Request::RequestError;
                    return false;
                } else {
                    request->mConnectionType = request->mVersion == Request::V1_1 ? Request::KeepAlive : Request::Close;
                    request->mState = Request::ParseHeaders;
                    buf = crlf + 2;
                }
                break;
            case Request::ParseHeaders:
                if (crlf == buf) {
                    request->mState = Request::ParseBody;
                    const int rest = request->mBufferLength - (buf + 2 - request->mBuffer);
                    ::memmove(request->mBuffer, buf + 2, rest);
                    request->mBufferLength = rest;
                } else {
                    const char *colon = strnchr(buf, ':', crlf - buf);
                    request->mHeaders.push_back(std::pair<std::string, std::string>());
                    std::pair<std::string, std::string> &h = request->mHeaders.back();
                    if (colon) {
                        if (colon == buf) {
                            error("Parse error when parsing header: [%s]\n",
                                  std::string(buf, crlf - buf).c_str());
                            request->mState = Request::RequestError;
                            return false;
                        }
                        // ### need to chomp spaces
                        h.first = trim(buf, colon - buf);
                        if (!strcasecmp(h.first.c_str(), "Content-Length")) {
                            char *end;
                            unsigned long contentLength = strtoull(h.second.c_str(), &end, 10);
                            if (*end) {
                                error("Parse error when parsing Content-Length: [%s]\n",
                                      std::string(buf, crlf - buf).c_str());
                                request->mState = Request::RequestError;
                                return false;
                            }

                            request->mContentLength = std::min<unsigned long>(INT_MAX, contentLength); // ### probably lower cap
                        } else if (!strcasecmp(h.first.c_str(), "Connection")) {
                            if (!strcasecmp(h.second.c_str(), "Keep-Alive")) {
                                request->mConnectionType = Request::KeepAlive;
                            } else if (!strcasecmp(h.second.c_str(), "Close")) {
                                request->mConnectionType = Request::Close;
                            } else {
                                error("Unknown Connection value: %s", h.second.c_str());
                                request->mState = Request::RequestError;
                                return false;
                            }
                        }
                        ++colon;
                        eat(colon, Space);
                        if (colon < crlf)
                            h.second = trim(colon, crlf - colon);
                    } else {
                        h.first = trim(buf, crlf - colon);
                    }
                    debug("Header: %s: %s\n", h.first.c_str(), h.second.c_str());
                    buf = crlf + 2;
                }
                break;
            case Request::ParseBody:
            case Request::RequestError:
                assert(0);
                break;
            }
        }
        break; }
    case Request::ParseBody:
        handleRequest(request);
        break;
    case Request::RequestError:
        assert(0);
        break;
    }
}

std::shared_ptr<NW::Request> NW::acceptConnection(int fd, const Interface &localInterface)
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
        return std::shared_ptr<Request>();
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
    return std::shared_ptr<Request>(new Request(socket, localInterface, remoteInterface));
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

// ==================== Request ====================

NW::Request::Request(int socket, const NW::Interface &local, const NW::Interface &remote)
    : mSocket(socket), mLocalInterface(local), mRemoteInterface(remote),
      mMethod(NoMethod), mVersion(NoVersion), mConnectionType(NoConnection),
      mState(ParseRequestLine), mContentLength(-1), mBuffer(0), mBufferLength(0),
      mBufferCapacity(0)
{}

NW::Request::~Request()
{
    if (mBuffer)
        ::free(mBuffer);
}


bool NW::Request::hasHeader(const std::string &header) const
{
    const size_t len = header.size();
    for (std::vector<std::pair<std::string, std::string> >::const_iterator it = mHeaders.begin(); it != mHeaders.end(); ++it) {
        if (it->first.size() == len && strcasecmp(header.c_str(), it->first.c_str())) {
            return true;
        }
    }
    return false;
}

std::string NW::Request::headerValue(const std::string &header) const
{
    const size_t len = header.size();
    for (std::vector<std::pair<std::string, std::string> >::const_iterator it = mHeaders.begin(); it != mHeaders.end(); ++it) {
        if (it->first.size() == len && strcasecmp(header.c_str(), it->first.c_str())) {
            return it->second;
        }
    }
    return std::string();
}

int NW::Request::readContent(char *buf, int max)
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

