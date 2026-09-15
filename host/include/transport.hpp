#pragma once
#include "platform.hpp"
#include <nlohmann/json.hpp>
namespace bm {
class ITransport {
  public:
    virtual ~ITransport() = default;
    virtual void poll() = 0;
    virtual bool connected() const = 0;
    virtual bool send(const Encoded &) = 0;
    virtual bool consumeKeyframeRequest() = 0;
    virtual uint32_t targetBitrate() const = 0;
    virtual nlohmann::json stats() const = 0;
};
std::unique_ptr<ITransport> webRtc(std::string server, std::string room, std::string secret);
} // namespace bm
