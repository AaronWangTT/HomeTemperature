// AZ3166 Core 2.0.0 links these vendor SDK telemetry hooks from its archive.
// Matching no-op C definitions prevent those callbacks from sending unrelated
// system telemetry, so network egress remains under application control. Keep
// these symbol names and signatures aligned with the SDK hooks.
extern "C" {

void telemetry_init() {}

void send_telemetry_data(const char *, const char *, const char *) {}

void send_telemetry_data_async(const char *, const char *, const char *) {}

void send_telemetry_data_sync(const char *, const char *, const char *) {}

}