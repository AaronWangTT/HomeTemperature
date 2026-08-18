#ifndef TELEMETRY_SERVICE_H
#define TELEMETRY_SERVICE_H

#include <stddef.h>

#include <HTS221Sensor.h>
#include <LPS22HBSensor.h>
#include <Sensor.h>

struct TelemetryReading {
    float temperature;
    float humidity;
    float pressure;
};

class TelemetryService {
public:
    enum PayloadError {
        PAYLOAD_SENSOR_ERROR = -1,
        PAYLOAD_FORMAT_ERROR = -2
    };

    TelemetryService();

    void begin(const char *deviceId);
    bool read(TelemetryReading &reading);

    int buildPayload(
        char *payload,
        size_t payloadSize);

    static int formatPayload(
        const char *deviceId,
        const TelemetryReading &reading,
        char *payload,
        size_t payloadSize);

private:
    const char *deviceId_;
    DevI2C devI2c_;
    HTS221Sensor hts221_;
    LPS22HBSensor lps22hb_;
};

#endif