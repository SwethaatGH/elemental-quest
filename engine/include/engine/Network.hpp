#pragma once

#include <memory>
#include <string>

namespace engine {
namespace net {
enum class SocketType {
    Request,  // REQ - the asking end of a synchronous pair
    Reply,  // REP - the answering end of a synchronous pair
    Publish,  // PUB - one-to-many broadcast
    Subscribe,  // SUB - the receiving end of a broadcast
};

// A deliberately thin wrapper over cppzmq
class Socket {
public:
    explicit Socket(SocketType type);
    ~Socket();

    Socket(Socket &&) noexcept;
    Socket &operator=(Socket &&) noexcept;
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    // "tcp://*:5555" to listen; "tcp://localhost:5555" to reach out
    bool Bind(const std::string &endpoint);
    bool Connect(const std::string &endpoint);
    const std::string &LastError() const;

    // SUB sockets only, and required for them: a SUB that subscribes to nothing receives nothing
    void Subscribe(const std::string &topic = "");

    // Without a timeout, a blocking Receive() hangs forever if the peer dies mid-exchange
    void SetReceiveTimeoutMs(int milliseconds);

    // Messages queued per peer before 0MQ starts dropping
    void SetSendQueue(int messages);
    void SetReceiveQueue(int messages);

    // Neither throws
    bool Send(const std::string &payload);

    // False is usually a timeout; an error, if any, is in LastError()
    bool Receive(std::string &out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}  // namespace net
}  // namespace engine
