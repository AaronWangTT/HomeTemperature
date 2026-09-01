#include "TelemetryService.h"

#include <Arduino.h>
#include <HTS221Sensor.h>
#include <LPS22HBSensor.h>
#include <Sensor.h>

#include "AppConfig.h"

namespace {

bool isValidTelemetryReading(const TelemetryReading &reading) {
    return isfinite(reading.temperature) &&
           reading.temperature >= AppConfig::TELEMETRY_MIN_TEMPERATURE_C &&
           reading.temperature <= AppConfig::TELEMETRY_MAX_TEMPERATURE_C &&
           isfinite(reading.humidity) &&
           reading.humidity >= AppConfig::TELEMETRY_MIN_HUMIDITY_PERCENT &&
           reading.humidity <= AppConfig::TELEMETRY_MAX_HUMIDITY_PERCENT &&
           isfinite(reading.pressure) &&
           reading.pressure >= AppConfig::TELEMETRY_MIN_PRESSURE_HPA &&
           reading.pressure <= AppConfig::TELEMETRY_MAX_PRESSURE_HPA;
}

}  // namespace

TelemetryService::TelemetryService()
    : deviceId_(NULL),
      devI2c_(D14, D15),
      hts221_(devI2c_),
      lps22hb_(devI2c_) {
}

void TelemetryService::begin(const char *deviceId) {
    deviceId_ = deviceId;
    hts221_.init(NULL);
    hts221_.enable();
    lps22hb_.init(NULL);
}

bool TelemetryService::read(TelemetryReading &reading) {
    return hts221_.getTemperature(&reading.temperature) == 0 &&
           hts221_.getHumidity(&reading.humidity) == 0 &&
           lps22hb_.getPressure(&reading.pressure) == 0;
}

int TelemetryService::formatPayload(
    const char *deviceId,
    const TelemetryReading &reading,
    char *payload,
    size_t payloadSize) {
    if (deviceId == NULL || payload == NULL || payloadSize == 0) {
        return PAYLOAD_FORMAT_ERROR;
    }

    payload[0] = '\0';
    if (!isValidTelemetryReading(reading)) {
        return PAYLOAD_SENSOR_ERROR;
    }

    char temperatureText[16];
    char humidityText[16];
    char pressureText[16];
    dtostrf(reading.temperature, 0, 1, temperatureText);
    dtostrf(reading.humidity, 0, 1, humidityText);
    dtostrf(reading.pressure, 0, 1, pressureText);
    if (strcmp(temperatureText, "ovf") == 0 ||
        strcmp(humidityText, "ovf") == 0 ||
        strcmp(pressureText, "ovf") == 0) {
        return PAYLOAD_FORMAT_ERROR;
    }

    int payloadLength = snprintf(
        payload,
        payloadSize,
        "{\"deviceId\":\"%s\",\"temperature\":%s,\"humidity\":%s,\"pressure\":%s}",
        deviceId,
        temperatureText,
        humidityText,
        pressureText);
    if (payloadLength < 0 ||
        static_cast<size_t>(payloadLength) >= payloadSize) {
        payload[0] = '\0';
        return PAYLOAD_FORMAT_ERROR;
    }

    return payloadLength;
}

int TelemetryService::buildPayload(
    char *payload,
    size_t payloadSize) {
    if (deviceId_ == NULL) {
        return PAYLOAD_FORMAT_ERROR;
    }

    TelemetryReading reading;
    if (!read(reading)) {
        return PAYLOAD_SENSOR_ERROR;
    }

    return formatPayload(
        deviceId_,
        reading,
        payload,
        payloadSize);
}
