#include <algorithm>

#include "ServerFrameworkImpl.h"
#include "common/DebugConfig.h"

#define MAX_POST_ACCEPT_COUNT 10

#define CONTINUE_IF(_cond_) if (_cond_) continue

namespace iocp {
    namespace _impl {

        //
        // _ServerFramework
        //

        bool _ServerFramework::initialize()
        {
            WSADATA data;
            WORD ver = MAKEWORD(2, 2);
            int ret = ::WSAStartup(ver, &data);
            if (ret != 0)
            {
                LOG_DEBUG("WSAStartup failed: last error %d", ::WSAGetLastError());
                return false;
            }
            return true;
        }

        bool _ServerFramework::uninitialize()
        {
            return ::WSACleanup() == 0;
        }

        _ServerFramework::_ServerFramework()
        {
            _ip[0] = '\0';
            ::InitializeCriticalSection(&_clientCriticalSection);
            ::InitializeCriticalSection(&_poolCriticalSection);
        }

        _ServerFramework::~_ServerFramework()
        {
            ::DeleteCriticalSection(&_clientCriticalSection);
            ::DeleteCriticalSection(&_poolCriticalSection);
        }

        bool _ServerFramework::startup(const char *ip, uint16_t port,
            const ClientRecvCallback &clientRecvCallback,
            const ClientDisconnectCallback &clientDisconnectCallback)
        {
            _clientRecvCallback = clientRecvCallback;
            _clientDisconnectCallback = clientDisconnectCallback;

            _shouldQuit = false;

            // Completion Port.
            _ioCompletionPort = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
            if (_ioCompletionPort == NULL)
            {
                return false;
            }

            SYSTEM_INFO systemInfo;
            GetSystemInfo(&systemInfo);
            DWORD workerThreadCnt = systemInfo.dwNumberOfProcessors * 2 + 2;
            LOG_DEBUG("systemInfo.dwNumberOfProcessors = %u, workerThreadCnt = %u", systemInfo.dwNumberOfProcessors, workerThreadCnt);

            // Worker threads.
            while (workerThreadCnt-- > 0)
            {
                std::thread *t = new (std::nothrow) std::thread([this]() { worketThreadProc(); });
                if (t != nullptr)
                {
                    _workerThreads.push_back(t);
                }
                else
                {
                    LOG_DEBUG("new std::thread out of memory!");
                }
            }

            _listenSocket = ::WSASocket(AF_INET, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
            if (_listenSocket == INVALID_SOCKET)
            {
                _shouldQuit = true;
                return false;
            }

            _port = port;
            struct sockaddr_in serverAddr = { 0 };
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = ::htons(_port);
            if (ip != nullptr)
            {
                strncpy(_ip, ip, 16);
                serverAddr.sin_addr.s_addr = ::inet_addr(ip);
            }

            if (::bind(_listenSocket, (const struct sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
            {
                _shouldQuit = true;
                return false;
            }

            if (::listen(_listenSocket, SOMAXCONN) == SOCKET_ERROR)
            {
                _shouldQuit = true;
                return false;
            }

            _clientCount = 0;
            return beginAccept();
        }

        void _ServerFramework::shutdown()
        {
            _shouldQuit = true;
            if (_listenSocket != INVALID_SOCKET)
            {
                ::closesocket(_listenSocket);
                _listenSocket = INVALID_SOCKET;
            }

            // Terminate all the worker threads.
            for (size_t i = _workerThreads.size(); i > 0; --i)
            {
                ::PostQueuedCompletionStatus(_ioCompletionPort, 0, (ULONG_PTR)nullptr, nullptr);
            }

            std::for_each(_workerThreads.begin(), _workerThreads.end(), [](std::thread *t) {
                t->join();
                delete t;
            });
            _workerThreads.clear();

            // Delete all the ClientContext.
            std::for_each(_clientList.begin(), _clientList.end(), [](_ClientContext *ctx) { delete ctx; });
            _clientList.clear();

            // Delete all the AcceptIOData.
            std::for_each(_allAcceptIOData.begin(), _allAcceptIOData.end(), [](_PER_IO_OPERATION_DATA *ioData) { delete ioData; });
            _allAcceptIOData.clear();

            ::CloseHandle(_ioCompletionPort);
            _ioCompletionPort = NULL;

            std::for_each(_freeSocketPool.begin(), _freeSocketPool.end(), &::closesocket);
            _freeSocketPool.clear();
        }

        void _ServerFramework::worketThreadProc()
        {
            static std::function<void (_ClientContext *)> removeExceptionalConnection = [this](_ClientContext *ctx) {
                LOG_DEBUG("%16s:%5hu disconnected", ctx->_ip, ctx->_port);
                _clientDisconnectCallback(ctx);

                SOCKET s = ctx->_socket;  // Save the socket.

                ::EnterCriticalSection(&_clientCriticalSection);
                ctx->_socket = INVALID_SOCKET;
                _clientList.erase(ctx->_iterator);  // Remove from the ClientContext list.
                delete ctx;
                --_clientCount;
                LOG_DEBUG("client count %lu", _clientCount);
                ::LeaveCriticalSection(&_clientCriticalSection);

                // Recycle the socket.
                _disconnectEx(s, nullptr, TF_REUSE_SOCKET, 0);
                ::EnterCriticalSection(&_poolCriticalSection);
                _freeSocketPool.push_back(s);
                ::LeaveCriticalSection(&_poolCriticalSection);
            };

            LPOVERLAPPED overlapped = nullptr;
            ULONG_PTR completionKey = 0;
            DWORD bytesTransfered = 0;

            while (!_shouldQuit)
            {
                BOOL ret = ::GetQueuedCompletionStatus(_ioCompletionPort, &bytesTransfered, &completionKey, &overlapped, INFINITE);

                _ClientContext *ctx = (_ClientContext *)completionKey;
                CONTINUE_IF(ctx == nullptr);

                _PER_IO_OPERATION_DATA *ioData = (_PER_IO_OPERATION_DATA *)overlapped;
                CONTINUE_IF(ioData == nullptr);

                if (!ret)
                {
                    DWORD lastError = ::GetLastError();
                    if (lastError == ERROR_NETNAME_DELETED)
                    {
                        // Since socket is the 1st field of ClientContext,
                        // we can just compare the address to determine whether the completion event is posted by listenSocket.
                        if ((void *)ctx == (void *)&_listenSocket)
                        {
                            // We should post a new AcceptEx as a supplement, if something goes wrong when accepting.
                            postAccept(ioData);
                        }
                        else
                        {
                            removeExceptionalConnection(ctx);
                        }
                        continue;
                    }

                    CONTINUE_IF(lastError == WAIT_TIMEOUT);
                    CONTINUE_IF(overlapped != nullptr);
                    break;
                }

                switch (ioData->type)
                {
                case _OPERATION_TYPE::ACCEPT_POSTED:
                    doAccept(ioData);
                    break;
                case _OPERATION_TYPE::RECV_POSTED:
                    if (bytesTransfered == 0 || !doRecv(ctx, ioData->buf, bytesTransfered))
                    {
                        removeExceptionalConnection(ctx);
                    }
                    break;
                case _OPERATION_TYPE::SEND_POSTED:
                    doSend(ctx);
                    break;
                case _OPERATION_TYPE::NULL_POSTED:
                    break;
                default:
                    break;
                }
            }
        }

        bool _ServerFramework::beginAccept()
        {
            // Associate the listenSocket with CompletionPort.
            // We just use the address of listenSocket as the CompletionKey.
            ::CreateIoCompletionPort((HANDLE)_listenSocket, _ioCompletionPort, (ULONG_PTR)&_listenSocket, 0);

            if (!getFunctionPointers())
            {
                _shouldQuit = true;
                return false;
            }

            static std::function<bool ()> postAcceptFunc = [this](){
                // Prepare the ioData for posting AcceptEx.
                _PER_IO_OPERATION_DATA *ioData = new (std::nothrow) _PER_IO_OPERATION_DATA;
                if (ioData == nullptr)
                {
                    LOG_DEBUG("new IODATA out of memory!");
                    return false;
                }

                if (postAccept(ioData))
                {
                    _allAcceptIOData.push_back(ioData);
                    return true;
                }

                delete ioData;
                return false;
            };

            // Post AcceptEx.
            for (int i = 0; i < MAX_POST_ACCEPT_COUNT; ++i)
            {
                postAcceptFunc();
            }

            return true;
        }

        bool _ServerFramework::getFunctionPointers()
        {
            GUID guidAcceptEx = WSAID_ACCEPTEX;
            DWORD bytes = 0;

            if (::WSAIoctl(_listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                &guidAcceptEx, sizeof(GUID),
                &_acceptEx, sizeof(LPFN_ACCEPTEX),
                &bytes, nullptr, nullptr) == SOCKET_ERROR)
            {
                return false;
            }

            GUID guidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;
            if (::WSAIoctl(_listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                &guidGetAcceptExSockAddrs, sizeof(GUID),
                &_getAcceptExSockAddrs, sizeof(LPFN_GETACCEPTEXSOCKADDRS),
                &bytes, nullptr, nullptr) == SOCKET_ERROR)
            {
                return false;
            }

            GUID guidDisconnectEx = WSAID_DISCONNECTEX;
            if (::WSAIoctl(_listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                &guidDisconnectEx, sizeof(GUID),
                &_disconnectEx, sizeof(LPFN_DISCONNECTEX),
                &bytes, nullptr, nullptr) == SOCKET_ERROR)
            {
                return false;
            }

            return true;
        }

        bool _ServerFramework::postAccept(_PER_IO_OPERATION_DATA *ioData)
        {
            SOCKET clientSocket = INVALID_SOCKET;

            ::EnterCriticalSection(&_poolCriticalSection);
            if (_freeSocketPool.empty())
            {
                ::LeaveCriticalSection(&_poolCriticalSection);
                clientSocket = ::socket(AF_INET, SOCK_STREAM, 0);  // Create a new one.
            }
            else
            {
                clientSocket = _freeSocketPool.back();  // Reuse.
                _freeSocketPool.pop_back();
                ::LeaveCriticalSection(&_poolCriticalSection);
            }

            if (clientSocket == INVALID_SOCKET)
            {
                return false;
            }

            memset(&ioData->overlapped, 0, sizeof(OVERLAPPED));
            char *buf = ioData->buf;
            ioData->type = _OPERATION_TYPE::ACCEPT_POSTED;

            // Store the clientSocket in the buffer.
            *(SOCKET *)&buf[(sizeof(struct sockaddr_in) + 16) * 2] = clientSocket;
            DWORD bytes = 0;

            // To make AcceptEx to complete as soon as a connection arrives, we set the 4th parameter 0.
            // On the other hand, we don't wish that the received data overwrite the clientSocket we've stored before.
            BOOL ret = _acceptEx(_listenSocket, clientSocket, buf, 0,
                sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16,
                &bytes, &ioData->overlapped);

            if (!ret && ::WSAGetLastError() != ERROR_IO_PENDING)
            {
                return false;
            }

            return true;
        }

        bool _ServerFramework::doAccept(_PER_IO_OPERATION_DATA *ioData)
        {
            // Get the clientSocket we've stored before.
            SOCKET clientSocket = *(SOCKET *)&ioData->buf[(sizeof(struct sockaddr_in) + 16) * 2];

            struct sockaddr_in *remoteAddr = nullptr;
            struct sockaddr_in *localAddr = nullptr;
            int remoteLen = sizeof(struct sockaddr_in);
            int localLen = sizeof(struct sockaddr_in);

            _getAcceptExSockAddrs(ioData->buf, 0, sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16,
                (struct sockaddr **)&localAddr, &localLen, (struct sockaddr **)&remoteAddr, &remoteLen);

            const char *ip = ::inet_ntoa(remoteAddr->sin_addr);
            uint16_t port = ::ntohs(remoteAddr->sin_port);

            LOG_DEBUG("remote address %s %hu", ::inet_ntoa(remoteAddr->sin_addr), ::ntohs(remoteAddr->sin_port));
            LOG_DEBUG("local address %s %hu", ::inet_ntoa(localAddr->sin_addr), ::ntohs(localAddr->sin_port));

            _ClientContext *ctx = new (std::nothrow) _ClientContext;
            if (ctx == nullptr)
            {
                LOG_DEBUG("new context out of memory!");

                // Recycle the socket.
                _disconnectEx(clientSocket, nullptr, TF_REUSE_SOCKET, 0);
                ::EnterCriticalSection(&_poolCriticalSection);
                _freeSocketPool.push_back(clientSocket);
                ::LeaveCriticalSection(&_poolCriticalSection);
            }
            else
            {
                 ::EnterCriticalSection(&_clientCriticalSection);
                _clientList.push_front(nullptr);  // Push a nullptr as a placeholder.
                // Then get the iterator, which won't become invalid when we erased other elements.
                mp::list<_ClientContext *>::iterator it = _clientList.begin();
                ++_clientCount;
                LOG_DEBUG("client count %lu", _clientCount);
                ::LeaveCriticalSection(&_clientCriticalSection);

                ctx->_socket = clientSocket;
                strncpy(ctx->_ip, ip, 16);
                ctx->_port = port;
                ctx->_iterator = it;
                *it = ctx;  // Replace the placeholder with the new ClientContext.

                // Associate the clientSocket with CompletionPort.
                if (::CreateIoCompletionPort((HANDLE)clientSocket, _ioCompletionPort, (ULONG_PTR)ctx, 0) != NULL)
                {
                    if (ctx->postRecv() == _ClientContext::POST_RESULT::SUCCESS)
                    {
                        LOG_DEBUG("%16s:%5hu connected", ip, port);
                    }
                    else
                    {
                        LOG_DEBUG("%16s:%5hu post recv failed", ip, port);
                    }
                }
            }
            return postAccept(ioData);
        }

        bool _ServerFramework::doRecv(_ClientContext *ctx, const char *buf, size_t len) const
        {
            bool ret = false;

            ::EnterCriticalSection(&ctx->_recvCriticalSection);
            mp::vector<char> &_recvCache = ctx->_recvCache;
            do
            {
                if (_recvCache.empty())
                {
                    size_t bytesProcessed = _clientRecvCallback(ctx, buf, len);
                    if (bytesProcessed < len)  // Cache the remainder bytes.
                    {
                        size_t remainder = len - bytesProcessed;
                        _recvCache.resize(remainder);
                        memcpy(&_recvCache[0], buf + bytesProcessed, remainder);
                    }
                }
                else
                {
                    size_t size = _recvCache.size();
                    if (size + len > RECV_CACHE_LIMIT_SIZE)  // The recvCache takes too much memory.
                    {
                        break;
                    }
                    _recvCache.resize(size + len);
                    memcpy(&_recvCache[size], buf, len);
                    size_t bytesProcessed = _clientRecvCallback(ctx, &_recvCache[0], _recvCache.size());
                    if (bytesProcessed >= _recvCache.size())  // All the cached bytes has been processed.
                    {
                        _recvCache.clear();
                    }
                    else if (bytesProcessed > 0)  // Cache the remainder bytes.
                    {
                        size_t remainder = len - bytesProcessed;
                        memmove(&_recvCache[0], &_recvCache[bytesProcessed], remainder);
                        _recvCache.resize(remainder);
                    }
                }
                ret = (ctx->postRecv() == _ClientContext::POST_RESULT::SUCCESS);  // Post a new WSARecv.
            } while (0);

            ::LeaveCriticalSection(&ctx->_recvCriticalSection);
            return ret;
        }

        void _ServerFramework::doSend(_ClientContext *ctx) const
        {
            mp::vector<char> &_sendCache = ctx->_sendCache;
            if (_sendCache.empty())
            {
                return;  // Nothing to send.
            }

            SOCKET _socket = ctx->_socket;
            _PER_IO_OPERATION_DATA &_sendIOData = ctx->_sendIOData;
            memset(&_sendIOData.overlapped, 0, sizeof(OVERLAPPED));
            _sendIOData.type = _OPERATION_TYPE::SEND_POSTED;
            DWORD sendBytes = 0;

            WSABUF wsaBuf;
            wsaBuf.buf = _sendIOData.buf;
            wsaBuf.len = OVERLAPPED_BUF_SIZE;
            if (_sendCache.size() <= OVERLAPPED_BUF_SIZE)  // Is the buffer large enough to carry cached bytes?
            {
                memcpy(_sendIOData.buf, &_sendCache[0], _sendCache.size());
                wsaBuf.len = _sendCache.size();
                ::WSASend(_socket, &wsaBuf, 1, &sendBytes, 0, (LPOVERLAPPED)&_sendIOData, nullptr);

                // Prepare next buffer.
                mp::deque<mp::vector<char> > &_sendQueue = ctx->_sendQueue;
                if (_sendQueue.empty())
                {
                    _sendCache.clear();
                }
                else
                {
                    _sendCache = std::move(_sendQueue.front());
                    _sendQueue.pop_front();
                }
            }
            else
            {
                // Send the full size bytes in buffer.
                memcpy(_sendIOData.buf, &_sendCache[0], OVERLAPPED_BUF_SIZE);
                ::WSASend(_socket, &wsaBuf, 1, &sendBytes, 0, (LPOVERLAPPED)&_sendIOData, nullptr);

                // Cache the remainder bytes.
                memmove(&_sendCache[0], &_sendCache[OVERLAPPED_BUF_SIZE], _sendCache.size() - OVERLAPPED_BUF_SIZE);
                _sendCache.resize(_sendCache.size() - OVERLAPPED_BUF_SIZE);
            }
        }

        //
        // _ClientContext
        //

        _ClientContext::_ClientContext()
        {
            ::InitializeCriticalSection(&_sendCriticalSection);
            ::InitializeCriticalSection(&_recvCriticalSection);
        }

        _ClientContext::~_ClientContext()
        {
            if (_socket != INVALID_SOCKET)
            {
                ::closesocket(_socket);
                _socket = INVALID_SOCKET;
            }
            ::DeleteCriticalSection(&_sendCriticalSection);
            ::DeleteCriticalSection(&_recvCriticalSection);
        }

        _ClientContext::POST_RESULT _ClientContext::postRecv()
        {
            memset(&_recvIOData, 0, sizeof(OVERLAPPED));
            _recvIOData.type = _OPERATION_TYPE::RECV_POSTED;
            DWORD recvBytes = 0, flags = 0;

            WSABUF wsaBuf;
            wsaBuf.buf = _recvIOData.buf;
            wsaBuf.len = OVERLAPPED_BUF_SIZE;
            int ret = ::WSARecv(_socket, &wsaBuf, 1, &recvBytes, &flags, (LPOVERLAPPED)&_recvIOData, nullptr);
            if (ret == SOCKET_ERROR && ::WSAGetLastError() != ERROR_IO_PENDING)
            {
                return POST_RESULT::FAIL;
            }
            return POST_RESULT::SUCCESS;
        }

        _ClientContext::POST_RESULT _ClientContext::postSend(const char *buf, size_t len)
        {
            if (!_sendCache.empty())  // Other bytes sending now, so we put the new buffer to the queue.
            {
                mp::vector<char> temp;
                temp.resize(len);
                memcpy(&temp[0], buf, len);
                _sendQueue.push_back(std::move(temp));
                return POST_RESULT::CACHED;
            }

            memset(&_sendIOData, 0, sizeof(OVERLAPPED));
            _sendIOData.type = _OPERATION_TYPE::SEND_POSTED;

            DWORD sendBytes = 0;
            WSABUF wsaBuf;
            wsaBuf.buf = _sendIOData.buf;
            wsaBuf.len = OVERLAPPED_BUF_SIZE;
            if (len <= OVERLAPPED_BUF_SIZE)  // Is the buffer large enough to carry the bytes to be sent?
            {
                memcpy(_sendIOData.buf, buf, len);
                wsaBuf.len = len;
                int ret = ::WSASend(_socket, &wsaBuf, 1, &sendBytes, 0, (LPOVERLAPPED)&_sendIOData, nullptr);
                if (ret == SOCKET_ERROR && ::WSAGetLastError() != ERROR_IO_PENDING)
                {
                    return POST_RESULT::FAIL;
                }
                return POST_RESULT::SUCCESS;
            }
            else
            {
                // Cache the remainder bytes.
                _sendCache.resize(len - OVERLAPPED_BUF_SIZE);
                memcpy(&_sendCache[0], buf + OVERLAPPED_BUF_SIZE, len - OVERLAPPED_BUF_SIZE);

                // Send the full size bytes in the buffer.
                memcpy(_sendIOData.buf, buf, OVERLAPPED_BUF_SIZE);
                int ret = ::WSASend(_socket, &wsaBuf, 1, &sendBytes, 0, (LPOVERLAPPED)&_sendIOData, nullptr);
                if (ret == SOCKET_ERROR && ::WSAGetLastError() != ERROR_IO_PENDING)
                {
                    return POST_RESULT::FAIL;
                }
                return POST_RESULT::SUCCESS;
            }
        }
    }  // end of namespace _impl
}  // end of namespace iocp