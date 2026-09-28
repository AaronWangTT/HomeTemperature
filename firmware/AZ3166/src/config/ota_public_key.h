#ifndef HOME_TEMPERATURE_OTA_PUBLIC_KEY_H
#define HOME_TEMPERATURE_OTA_PUBLIC_KEY_H

#include <stddef.h>
#include <stdint.h>

// Provision the production RFC 5480 P-256 public key in this file through the
// reviewed wired-bootstrap release process. Empty configuration disables OTA.
static const uint8_t HOME_TEMPERATURE_OTA_PUBLIC_KEY_DER[] = {0};
static const size_t HOME_TEMPERATURE_OTA_PUBLIC_KEY_DER_SIZE = 0;

#endif
