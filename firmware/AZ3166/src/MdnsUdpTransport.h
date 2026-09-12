#ifndef MDNS_UDP_TRANSPORT_H
#define MDNS_UDP_TRANSPORT_H

#include "MdnsTransport.h"

class MdnsUdpTransport : public MdnsTransport {
public:
    MdnsUdpTransport();
    ~MdnsUdpTransport() override;
    void setLocalIPv4Address(uint32_t address);
    bool failed() const;

    uint8_t beginMulticast(IPAddress address, uint16_t port) override;
    void stop() override;
    int beginPacket(IPAddress address, uint16_t port) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    int endPacket() override;
    int parsePacket() override;
    int read(uint8_t *buffer, size_t size) override;
    void flush() override;
    IPAddress remoteIP() override;
    uint16_t remotePort() override;

private:
    MdnsUdpTransport(const MdnsUdpTransport &) = delete;
    MdnsUdpTransport &operator=(const MdnsUdpTransport &) = delete;

    int socket_;
    uint32_t localAddress_;
    IPAddress destination_;
    IPAddress remoteAddress_;
    uint16_t destinationPort_;
    uint16_t remotePort_;
    size_t receiveLength_;
    size_t receiveOffset_;
    size_t sendLength_;
    bool overflow_;
    bool failed_;
    uint8_t receiveBuffer_[1536];
    uint8_t sendBuffer_[512];
};

#endif