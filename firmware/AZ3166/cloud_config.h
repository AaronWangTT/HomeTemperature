#ifndef CLOUD_CONFIG_H
#define CLOUD_CONFIG_H

#if defined(__has_include)
#if __has_include("cloud_deployment.h")
#include "cloud_deployment.h"
#endif
#if __has_include("cloud_secrets.h")
#include "cloud_secrets.h"
#endif
#endif

#ifndef HOME_TEMPERATURE_CLOUD_ENDPOINT
#define HOME_TEMPERATURE_CLOUD_ENDPOINT \
	"https://telemetry.example.com/api/v1/telemetry"
#endif

#ifndef CLOUD_DEVICE_API_KEY
#define CLOUD_DEVICE_API_KEY "replace-with-device-key"
#endif

#endif