// Wattcycle battery monitor: the stock MeshCore sensor (examples/simple_sensor), plus
// Bluetooth readings from Wattcycle (TDT BMS) batteries, added to the node's telemetry,
// and a high priority alert when a battery's charge drops to BMS_LOW_ALERT_PCT or below.

#include "../simple_sensor/SensorMesh.h"
#include "TdtBmsPoller.h"
#include <math.h>

#ifdef DISPLAY_CLASS
  #include "../simple_sensor/UITask.h"
  static UITask ui_task(display);
#endif

/* ------------------------------ Battery config -------------------------------- */

#ifndef BMS_NAMES
  #define BMS_NAMES                ""     // comma separated Bluetooth names, set in platformio.ini
#endif
#ifndef BMS_POLL_INTERVAL_SECS
  #define BMS_POLL_INTERVAL_SECS   60
#endif
#ifndef BMS_LOW_ALERT_PCT
  #define BMS_LOW_ALERT_PCT        20     // alert at or below this charge %
#endif
#ifndef BMS_STALE_SECS
  #define BMS_STALE_SECS           (5 * 60)   // readings older than this are left out of telemetry
#endif
#ifndef BMS_TELEM_CHANNEL_BASE
  #define BMS_TELEM_CHANNEL_BASE   10     // first battery on channel 10, second on 11, ...
#endif

static TdtBmsPoller bms;

// CayenneLPP truncates (value x multiplier) to an integer, so add half a step to round instead
static float lppRound(float value, float step) {
  return value >= 0 ? value + step / 2 : value - step / 2;
}

/* ------------------------------------------------------------------------------ */

class MyMesh : public SensorMesh {
public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
     : SensorMesh(board, radio, ms, rng, rtc, tables),
       battery_data(12*24, 5*60)    // 24 hours worth of battery data, every 5 minutes
  {
  }

protected:
  /* ========================== custom logic here ========================== */
  Trigger low_batt, critical_batt;
  TimeSeriesData  battery_data;
  Trigger bms_low[BMS_MAX_BATTERIES];

  bool getFreshReading(int idx, TdtBmsData& d) {
    uint32_t age_secs;
    return bms.getReading(idx, d, age_secs) && age_secs <= BMS_STALE_SECS;
  }

  void onSensorDataRead() override {
    float batt_voltage = getVoltage(TELEM_CHANNEL_SELF);

    battery_data.recordData(getRTCClock(), batt_voltage);   // record battery
    alertIf(batt_voltage < 3.4f, critical_batt, HIGH_PRI_ALERT, "Battery is critical!");
    alertIf(batt_voltage < 3.6f, low_batt, LOW_PRI_ALERT, "Battery is low");

    // Wattcycle batteries
    for (int i = 0; i < bms.getCount(); i++) {
      TdtBmsData d;
      if (!getFreshReading(i, d)) continue;   // no fresh reading: leave this battery's alert state as it is

      char text[64];
      snprintf(text, sizeof(text), "%s charge low: %u%%", bms.getName(i), (unsigned)d.soc);
      alertIf(d.soc <= BMS_LOW_ALERT_PCT, bms_low[i], HIGH_PRI_ALERT, text);
    }
  }

  void queryExtraTelemetry(uint8_t requester_permissions, CayenneLPP& telemetry) override {
    if ((requester_permissions & TELEM_PERM_ENVIRONMENT) == 0) return;

    for (int i = 0; i < bms.getCount(); i++) {
      TdtBmsData d;
      if (!getFreshReading(i, d)) continue;

      uint8_t ch = BMS_TELEM_CHANNEL_BASE + i;
      telemetry.addPercentage(ch, d.soc);
      telemetry.addVoltage(ch, lppRound(d.voltage, 0.01f));
      float amps = lppRound(fabsf(d.current), 0.001f);   // the LPP current field has no sign
      if (amps > 65.535f) amps = 65.535f;                  // and a 2 byte range (0.001 A steps)
      telemetry.addCurrent(ch, amps);
      if (d.temp_count > 0) {
        telemetry.addTemperature(ch, lppRound(d.avg_temp_c, 0.1f));
      }
    }
  }

  int querySeriesData(uint32_t start_secs_ago, uint32_t end_secs_ago, MinMaxAvg dest[], int max_num) override {
    battery_data.calcMinMaxAvg(getRTCClock(), start_secs_ago, end_secs_ago, &dest[0], TELEM_CHANNEL_SELF, LPP_VOLTAGE);
    return 1;
  }

  bool handleCustomCommand(uint32_t sender_timestamp, char* command, char* reply) override {
    if (strcmp(command, "magic") == 0) {    // example 'custom' command handling
      strcpy(reply, "**Magic now done**");
      return true;   // handled
    }
    return false;  // not handled
  }
  /* ======================================================================= */
};

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Sensor ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();
  bms.begin(BMS_NAMES, BMS_POLL_INTERVAL_SECS);   // starts the Bluetooth polling task

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif
}

void loop() {
  int len = strlen(command);
  // `command` must stay NUL-terminated within its bounds. If it ever isn't,
  // strlen() above can return >= sizeof(command) and the loop below would then
  // index past the buffer, so clamp defensively.
  if (len >= (int)sizeof(command)) {
    command[0] = 0;
    len = 0;
  }
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
    }
    Serial.print(c);
  }
  if (len == sizeof(command)-1) {  // buffer full: treat as a completed line
    command[sizeof(command)-2] = '\r';  // place end-of-line marker inside the buffer
    command[sizeof(command)-1] = 0;     // keep the buffer NUL-terminated
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    the_mesh.handleCommand(NULL, 0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

  board.loop();   // let the board feed its watchdog, run periodic housekeeping

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
}
