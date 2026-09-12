#include "MdnsUdpTransport.h"

#include <string.h>
#include "lwip/sockets.h"

namespace {

uint32_t networkAddress(IPAddress address) {
    uint32_t value = (static_cast<uint32_t>(address[0]) << 24) |
        (static_cast<uint32_t>(address[1]) << 16) |
        (static_cast<uint32_t>(address[2]) << 8) |
        static_cast<uint32_t>(address[3]);
    return htonl(value);
}

}

MdnsUdpTransport::MdnsUdpTransport()
    : socket_(-1),
      localAddress_(0),
      destinationPort_(0),
      remotePort_(0),
      receiveLength_(0),
      receiveOffset_(0),
      sendLength_(0),
      overflow_(false),
      failed_(false) {
}

MdnsUdpTransport::~MdnsUdpTransport() {
    stop();
}

void MdnsUdpTransport::setLocalIPv4Address(uint32_t address) {
    localAddress_ = address;
}

bool MdnsUdpTransport::failed() const {
    return failed_;
}

uint8_t MdnsUdpTransport::beginMulticast(IPAddress address, uint16_t port) {
    stop();
    if (localAddress_ == 0 || port == 0) {
        return 0;
    }

    socket_ = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) {
        return 0;
    }

    sockaddr_in local = {};
    local.sin_len = sizeof(local);
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    ip_mreq membership = {};
    membership.imr_multiaddr.s_addr = networkAddress(address);
    membership.imr_interface.s_addr = htonl(localAddress_);
    int reuse = 1;
    int unicastTtl = 255;
    unsigned char multicastTtl = 255;
    unsigned long nonblocking = 1;

    if (lwip_setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                       &reuse, sizeof(reuse)) != 0 ||
        lwip_bind(socket_, reinterpret_cast<sockaddr *>(&local),
                  sizeof(local)) != 0 ||
        lwip_setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &membership, sizeof(membership)) != 0 ||
        lwip_setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF,
                       &membership.imr_interface,
                       sizeof(membership.imr_interface)) != 0 ||
        lwip_setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL,
                       &multicastTtl, sizeof(multicastTtl)) != 0 ||
        lwip_setsockopt(socket_, IPPROTO_IP, IP_TTL,
                       &unicastTtl, sizeof(unicastTtl)) != 0 ||
        lwip_ioctl(socket_, FIONBIO, &nonblocking) != 0) {
        stop();
        return 0;
    }

    return 1;
}

void MdnsUdpTransport::stop() {
    if (socket_ >= 0) {
        lwip_close(socket_);
    }
    socket_ = -1;
    receiveLength_ = receiveOffset_ = sendLength_ = 0;
    overflow_ = failed_ = false;
}

int MdnsUdpTransport::beginPacket(IPAddress address, uint16_t port) {
    destination_ = address;
    destinationPort_ = port;
    sendLength_ = 0;
    overflow_ = false;
    return socket_ >= 0 && port != 0;
}

size_t MdnsUdpTransport::write(const uint8_t *buffer, size_t size) {
    if (buffer == NULL || overflow_ || size > sizeof(sendBuffer_) - sendLength_) {
        overflow_ = true;
        return 0;
    }
    memcpy(sendBuffer_ + sendLength_, buffer, size);
    sendLength_ += size;
    return size;
}

int MdnsUdpTransport::endPacket() {
    if (socket_ < 0 || overflow_ || sendLength_ == 0) {
        failed_ = true;
        return 0;
    }

    sockaddr_in destination = {};
    destination.sin_len = sizeof(destination);
    destination.sin_family = AF_INET;
    destination.sin_port = htons(destinationPort_);
    destination.sin_addr.s_addr = networkAddress(destination_);
    int sent = lwip_sendto(
        socket_, sendBuffer_, sendLength_, 0,
        reinterpret_cast<sockaddr *>(&destination), sizeof(destination));
    if (sent != static_cast<int>(sendLength_)) {
        failed_ = true;
        return 0;
    }
    return 1;
}

int MdnsUdpTransport::parsePacket() {
    flush();
    if (socket_ < 0) {
        return 0;
    }

    unsigned long pending = 0;
    if (lwip_ioctl(socket_, FIONREAD, &pending) != 0) {
        failed_ = true;
        return 0;
    }
    if (pending == 0) {
        return 0;
    }

    sockaddr_in remote = {};
    socklen_t remoteSize = sizeof(remote);
    int received = lwip_recvfrom(
        socket_, receiveBuffer_, sizeof(receiveBuffer_), MSG_DONTWAIT,
        reinterpret_cast<sockaddr *>(&remote), &remoteSize);
    if (received <= 0 || pending > sizeof(receiveBuffer_)) {
        return 0;
    }

    uint32_t address = ntohl(remote.sin_addr.s_addr);
    remoteAddress_ = IPAddress(
        static_cast<uint8_t>(address >> 24),
        static_cast<uint8_t>(address >> 16),
        static_cast<uint8_t>(address >> 8),
        static_cast<uint8_t>(address));
    remotePort_ = ntohs(remote.sin_port);
    receiveLength_ = static_cast<size_t>(received);
    return received;
}

int MdnsUdpTransport::read(uint8_t *buffer, size_t size) {
    if (buffer == NULL) {
        return 0;
    }
    size_t remaining = receiveLength_ - receiveOffset_;
    if (size > remaining) {
        size = remaining;
    }
    memcpy(buffer, receiveBuffer_ + receiveOffset_, size);
    receiveOffset_ += size;
    return static_cast<int>(size);
}

void MdnsUdpTransport::flush() {
    receiveLength_ = receiveOffset_ = 0;
}

IPAddress MdnsUdpTransport::remoteIP() {
    return remoteAddress_;
}

uint16_t MdnsUdpTransport::remotePort() {
    return remotePort_;
}