// Victron battery monitor: the stock MeshCore sensor (examples/simple_sensor), plus
// readings from Victron devices that broadcast "Instant Readout" advertisements (a
// SmartShunt is the one this is set up for), added to the node's telemetry, and a high
// priority alert when a device's state of charge drops to VTR_LOW_ALERT_PCT or below.
//
// Unlike the Wattcycle build, nothing connects to the battery monitor: it broadcasts and
// this node only listens, so it can never get in the way of VictronConnect.

#include "../simple_sensor/SensorMesh.h"
#include "VictronMonitor.h"
#include <math.h>

#ifdef DISPLAY_CLASS
  #include "../simple_sensor/UITask.h"
  static UITask ui_task(display);
#endif

/* ---------------------------- Battery monitor config ---------------------------- */

#ifndef VTR_DEVICES
  #define VTR_DEVICES                ""     // "name:KEY" entries, comma separated, set in platformio.ini
#endif
#ifndef VTR_SCAN_SECS
  #define VTR_SCAN_SECS              10     // how long each listening cycle scans for
#endif
#ifndef VTR_SCAN_INTERVAL_SECS
  #define VTR_SCAN_INTERVAL_SECS     60     // gap between listening cycles
#endif
#ifndef VTR_LOW_ALERT_PCT
  #define VTR_LOW_ALERT_PCT          20     // alert at or below this charge %
#endif
#ifndef VTR_STALE_SECS
  #define VTR_STALE_SECS             (5 * 60)   // readings older than this are left out of telemetry
#endif
#ifndef VTR_TELEM_CHANNEL_BASE
  #define VTR_TELEM_CHANNEL_BASE     10     // first device on channel 10, second on 11, ...
#endif

static VictronMonitor victron;

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
  Trigger victron_low[VTR_MAX_DEVICES];

  bool getFreshReading(int idx, VictronReading& d) {
    uint32_t age_secs;
    return victron.getReading(idx, d, age_secs) && age_secs <= VTR_STALE_SECS;
  }

  void onSensorDataRead() override {
    float batt_voltage = getVoltage(TELEM_CHANNEL_SELF);

    battery_data.recordData(getRTCClock(), batt_voltage);   // record node battery
    alertIf(batt_voltage < 3.4f, critical_batt, HIGH_PRI_ALERT, "Battery is critical!");
    alertIf(batt_voltage < 3.6f, low_batt, LOW_PRI_ALERT, "Battery is low");

    // Victron devices
    for (int i = 0; i < victron.getCount(); i++) {
      VictronReading d;
      if (!getFreshReading(i, d)) continue;   // no fresh reading: leave this device's alert state as it is
      if (!d.have_soc) continue;              // nothing to compare against the threshold

      char text[64];
      snprintf(text, sizeof(text), "%s charge low: %d%%", victron.getName(i), (int)(d.soc_pct + 0.5f));
      alertIf(d.soc_pct <= (float)VTR_LOW_ALERT_PCT, victron_low[i], HIGH_PRI_ALERT, text);
    }
  }

  void queryExtraTelemetry(uint8_t requester_permissions, CayenneLPP& telemetry) override {
    VTR_LOG("VTR: telemetry built, permissions 0x%02X, environment %s\n", requester_permissions,
            (requester_permissions & TELEM_PERM_ENVIRONMENT) ? "allowed" : "NOT allowed");
    if ((requester_permissions & TELEM_PERM_ENVIRONMENT) == 0) return;

    for (int i = 0; i < victron.getCount(); i++) {
      VictronReading d;
      uint32_t age_secs = 0;
      bool have = victron.getReading(i, d, age_secs);
      if (!have || age_secs > VTR_STALE_SECS) {
        if (!have) {
          VTR_LOG("VTR:   '%s' left out: no reading yet\n", victron.getName(i));
        } else {
          VTR_LOG("VTR:   '%s' left out: reading %lu s old\n", victron.getName(i), (unsigned long)age_secs);
        }
        continue;
      }
      VTR_LOG("VTR:   '%s' added on channel %d (reading %lu s old)\n", victron.getName(i),
              VTR_TELEM_CHANNEL_BASE + i, (unsigned long)age_secs);

      uint8_t ch = VTR_TELEM_CHANNEL_BASE + i;
      if (d.have_soc) {
        telemetry.addPercentage(ch, (uint32_t)(d.soc_pct + 0.5f));
      }
      if (d.have_voltage) {
        telemetry.addVoltage(ch, lppRound(d.voltage_v, 0.01f));
      }
      if (d.have_current) {
        float amps = lppRound(fabsf(d.current_a), 0.001f);   // the LPP current field has no sign
        if (amps > 65.535f) amps = 65.535f;                  // and a 2 byte range (0.001 A steps)
        telemetry.addCurrent(ch, amps);
      }
      if (d.have_aux_temp_c) {
        telemetry.addTemperature(ch, lppRound(d.aux_temp_c, 0.1f));
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
  victron.begin(VTR_DEVICES, VTR_SCAN_SECS, VTR_SCAN_INTERVAL_SECS);   // starts the Bluetooth listening task

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
