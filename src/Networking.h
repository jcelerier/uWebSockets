#ifndef NETWORKING_UWS_H
#define NETWORKING_UWS_H

#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define SSL_CTX_up_ref(x) x->references++
#define SSL_up_ref(x) x->references++
#endif

#ifndef __linux
#define MSG_NOSIGNAL 0
#else
#include <endian.h>
#endif

#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htobe64(x) OSSwapHostToBigInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#endif

#ifdef _WIN32
#define NOMINMAX
#include <WinSock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define SHUT_WR SD_SEND
#ifdef __MINGW32__
// Windows has always been tied to LE
#define htobe64(x) __builtin_bswap64(x)
#define be64toh(x) __builtin_bswap64(x)
#else
#define __thread __declspec(thread)
#define htobe64(x) htonll(x)
#define be64toh(x) ntohll(x)
#define pthread_t DWORD
#define pthread_self GetCurrentThreadId
#endif
#define WIN32_EXPORT __declspec(dllexport)

inline void close(SOCKET fd) {closesocket(fd);}
inline int setsockopt(SOCKET fd, int level, int optname, const void *optval, socklen_t optlen) {
    return setsockopt(fd, level, optname, (const char *) optval, optlen);
}

inline SOCKET dup(SOCKET socket) {
    WSAPROTOCOL_INFOW pi;
    if (WSADuplicateSocketW(socket, GetCurrentProcessId(), &pi) == SOCKET_ERROR) {
        return INVALID_SOCKET;
    }
    return WSASocketW(pi.iAddressFamily, pi.iSocketType, pi.iProtocol, &pi, 0, WSA_FLAG_OVERLAPPED);
}
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
#define WIN32_EXPORT
#endif

#include "Backend.h"
#include <openssl/ssl.h>
#include <csignal>
#include <vector>
#include <string>
#include <mutex>
#include <algorithm>
#include <memory>

namespace uS {

// todo: mark sockets nonblocking in these functions
struct Context {

#ifdef USE_MTCP
    mtcp_context *mctx;
#endif

    Context() {
        // mtcp_create_context
#ifdef USE_MTCP
        mctx = mtcp_create_context(0); // cpu index?
#endif
    }

    ~Context() {
#ifdef USE_MTCP
        mtcp_destroy_context(mctx);
#endif
    }

    // returns INVALID_SOCKET on error
    uv_os_sock_t acceptSocket(uv_os_sock_t fd) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
        // Linux, FreeBSD
        return accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
        // Windows, OS X
        return accept(fd, nullptr, nullptr);
#endif
    }

    // returns INVALID_SOCKET on error
    uv_os_sock_t createSocket(int domain, int type, int protocol) {
        int flags = 0;
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
        flags = SOCK_CLOEXEC | SOCK_NONBLOCK;
#endif

        return socket(domain, type | flags, protocol);
    }

    void closeSocket(uv_os_sock_t fd) {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
    }
};

namespace TLS {

class WIN32_EXPORT Context {
private:
    SSL_CTX *context = nullptr;
    std::shared_ptr<std::string> password;

    static int passwordCallback(char *buf, int size, int rwflag, void *u)
    {
        std::string *password = (std::string *) u;
        int length = std::min<int>(size, password->length());
        memcpy(buf, password->data(), length);
        buf[length] = '\0';
        return length;
    }

public:
    friend Context createContext(std::string certChainFileName, std::string keyFileName, std::string keyFilePassword);
    Context(SSL_CTX *context) : context(context) {

    }

    Context() = default;
    Context(const Context &other);
    Context &operator=(const Context &other);
    ~Context();
    operator bool() {
        return context;
    }

    SSL_CTX *getNativeContext() {
        return context;
    }
};

Context createContext(std::string certChainFileName, std::string keyFileName, std::string keyFilePassword = std::string());

}

struct Socket;

struct WIN32_EXPORT NodeData {
    char *recvBufferMemoryBlock;
    char *recvBuffer;
    int recvLength;
    Loop *loop;
    uS::Context *netContext;
    void *user = nullptr;
    static const int preAllocMaxSize = 1024;
    char **preAlloc;
    SSL_CTX *clientContext;

    Async *async = nullptr;
    pthread_t tid;

    struct TransferData {
        Poll *p;
        uv_os_sock_t fd;
        Socket *socketData;
        void (*pollCb)(Poll *, int, int);
        void (*cb)(Poll *);
    };

    void addAsync() {
        async = new Async(loop);
        async->setData(this);
        async->start(NodeData::asyncCallback);
    }

    std::mutex *asyncMutex;
    std::vector<TransferData> transferQueue;
    std::vector<Poll *> changePollQueue;
    static void asyncCallback(Async *async);

    static int getMemoryBlockIndex(size_t length) {
        return (length >> 4) + bool(length & 15);
    }

    char *getSmallMemoryBlock(int index) {
        if (preAlloc[index]) {
            char *memory = preAlloc[index];
            preAlloc[index] = nullptr;
            return memory;
        } else {
            return new char[index << 4];
        }
    }

    void freeSmallMemoryBlock(char *memory, int index) {
        if (!preAlloc[index]) {
            preAlloc[index] = memory;
        } else {
            delete [] memory;
        }
    }
};

}

#endif // NETWORKING_UWS_H
