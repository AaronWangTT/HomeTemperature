#ifndef MDNS_TRANSPORT_H
#define MDNS_TRANSPORT_H

#include <Arduino.h>
#include <IPAddress.h>

class MdnsTransport {
public:
    virtual ~MdnsTransport() {}
    virtual uint8_t beginMulticast(IPAddress address, uint16_t port) = 0;
    virtual void stop() = 0;
    virtual int beginPacket(IPAddress address, uint16_t port) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size) = 0;
    virtual int endPacket() = 0;
    virtual int parsePacket() = 0;
    virtual int read(uint8_t *buffer, size_t size) = 0;
    virtual void flush() = 0;
    virtual IPAddress remoteIP() = 0;
    virtual uint16_t remotePort() = 0;
};

#endif