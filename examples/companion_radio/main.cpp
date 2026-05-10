#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
    DataStore store(InternalFS, QSPIFlash, rtc_clock);
  #else
  #if defined(EXTRAFS)
    #include <CustomLFS.h>
    CustomLFS ExtraFS(0xD4000, 0x19000, 128);
    DataStore store(InternalFS, ExtraFS, rtc_clock);
  #else
    DataStore store(InternalFS, rtc_clock);
  #endif
  #endif
#endif

#if defined(NRF52_PLATFORM)
  // Newlib heap diagnostic helpers. The Adafruit nRF52 BSP runs FreeRTOS
  // with task stacks allocated from inside newlib's heap arena, so a naive
  // sbrk/stack-pointer comparison is meaningless. Instead we report
  // mallinfo() arena stats, and probe the largest single malloc that
  // succeeds to measure real headroom.
  #include <malloc.h>

  static void dbg_heap_report(const char* label) {
    struct mallinfo mi = mallinfo();
    Serial.printf("%s arena=%u used=%u free_chunks=%u\n",
                  label,
                  (unsigned)mi.arena,
                  (unsigned)mi.uordblks,
                  (unsigned)mi.fordblks);
  }

  // Probe the largest contiguous malloc that currently succeeds. Frees
  // immediately, but note that newlib's sbrk-based allocator does not
  // return memory to the system, so the arena will have grown after this
  // call. Use sparingly (boot-time only is fine).
  static uint32_t dbg_probe_max_alloc() {
    const uint32_t step = 256;
    uint32_t size = 96 * 1024;  // start optimistic
    while (size >= step) {
      void* p = malloc(size);
      if (p) {
        free(p);
        return size;
      }
      size -= step;
    }
    return 0;
  }
#endif

#if defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  DataStore store(LittleFS, rtc_clock);
#elif defined(ESP32)
  #include <SPIFFS.h>
  DataStore store(SPIFFS, rtc_clock);
#endif

#ifdef ESP32
  #ifdef WIFI_SSID
    #include <helpers/esp32/SerialWifiInterface.h>
    SerialWifiInterface serial_interface;
    #ifndef TCP_PORT
      #define TCP_PORT 5000
    #endif
  #elif defined(BLE_PIN_CODE)
    #include <helpers/esp32/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #elif defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(RP2040_PLATFORM)
  //#ifdef WIFI_SSID
  //  #include <helpers/rp2040/SerialWifiInterface.h>
  //  SerialWifiInterface serial_interface;
  //  #ifndef TCP_PORT
  //    #define TCP_PORT 5000
  //  #endif
  // #elif defined(BLE_PIN_CODE)
  //   #include <helpers/rp2040/SerialBLEInterface.h>
  //   SerialBLEInterface serial_interface;
  #if defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(NRF52_PLATFORM)
  #ifdef BLE_PIN_CODE
    #include <helpers/nrf52/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(STM32_PLATFORM)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface serial_interface;
#else
  #error "need to define a serial interface"
#endif

/* GLOBAL OBJECTS */
#ifdef DISPLAY_CLASS
  #include "UITask.h"
  UITask ui_task(&board, &serial_interface);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);

/* END GLOBAL OBJECTS */

void halt() {
  while (1) ;
}

void setup() {
  Serial.begin(115200);

  board.begin();

#ifdef DISPLAY_CLASS
  DisplayDriver* disp = NULL;
  if (display.begin()) {
    disp = &display;
    disp->startFrame();
  #ifdef ST7789
    disp->setTextSize(2);
  #endif
    disp->drawTextCentered(disp->width() / 2, 28, "Loading...");
    disp->endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_driver.getRngSeed());

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  #if defined(QSPIFLASH)
    if (!QSPIFlash.begin()) {
      // debug output might not be available at this point, might be too early. maybe should fall back to InternalFS here?
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: failed to initialize");
    } else {
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: initialized successfully");
    }
  #else
  #if defined(EXTRAFS)
      ExtraFS.begin();
  #endif
  #endif
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

#ifdef BLE_PIN_CODE
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

  //#ifdef WIFI_SSID
  //  WiFi.begin(WIFI_SSID, WIFI_PWD);
  //  serial_interface.begin(TCP_PORT);
  // #elif defined(BLE_PIN_CODE)
  //   char dev_name[32+16];
  //   sprintf(dev_name, "%s%s", BLE_NAME_PREFIX, the_mesh.getNodeName());
  //   serial_interface.begin(dev_name, the_mesh.getBLEPin());
  #if defined(SERIAL_RX)
    companion_serial.setPins(SERIAL_RX, SERIAL_TX);
    companion_serial.begin(115200);
    serial_interface.begin(companion_serial);
  #else
    serial_interface.begin(Serial);
  #endif
    the_mesh.startInterface(serial_interface);
#elif defined(ESP32)
  SPIFFS.begin(true);
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

#ifdef WIFI_SSID
  board.setInhibitSleep(true);   // prevent sleep when WiFi is active
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  serial_interface.begin(TCP_PORT);
#elif defined(BLE_PIN_CODE)
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#elif defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  serial_interface.begin(companion_serial);
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#else
  #error "need to define filesystem"
#endif

  sensors.begin();

#if ENV_INCLUDE_GPS == 1
  the_mesh.applyGpsPrefs();
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
#endif

#if defined(NRF52_PLATFORM)
  // ---- Heap diagnostic (boot snapshot) ----
  // Reports the malloc arena state and then probes for the largest single
  // allocation that succeeds. The probe number is the most useful headroom
  // indicator: it tells you how much contiguous RAM is available to grow
  // into right now.
  //   arena       = bytes newlib has claimed for its heap pool so far
  //   used        = bytes currently allocated inside the arena
  //   free_chunks = unallocated bytes inside the arena (immediately reusable)
  //   max_alloc   = largest single malloc that succeeds right now
  delay(200);  // let serial settle
  Serial.println();
  Serial.println("=== Heap report (boot) ===");
  dbg_heap_report("  pre-probe: ");
  uint32_t max_alloc = dbg_probe_max_alloc();
  Serial.printf("  max single alloc: %u bytes\n", (unsigned)max_alloc);
  dbg_heap_report("  post-probe:");
  Serial.printf("  MAX_CONTACTS: %d\n", MAX_CONTACTS);
  Serial.printf("  MAX_GROUP_CHANNELS: %d\n", MAX_GROUP_CHANNELS);
  Serial.printf("  OFFLINE_QUEUE_SIZE: %d\n", OFFLINE_QUEUE_SIZE);
  Serial.println("==========================");
#endif
}

void loop() {
  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

#if defined(NRF52_PLATFORM)
  // ---- Heap diagnostic (runtime watchdog) ----
  // Tracks arena growth. The arena grows when malloc needs more RAM than
  // the existing free chunks can satisfy, so each growth event marks a
  // new high-water mark for total heap commitment. When growth stops, the
  // node has reached steady state. Quiet most of the time; only prints
  // when the arena has actually grown.
  static uint32_t last_arena = 0;
  static uint32_t next_heap_check = 0;
  if (millis() >= next_heap_check) {
    struct mallinfo mi = mallinfo();
    if ((uint32_t)mi.arena > last_arena) {
      Serial.printf("Heap: arena grew to %u (used=%u, free_chunks=%u)\n",
                    (unsigned)mi.arena,
                    (unsigned)mi.uordblks,
                    (unsigned)mi.fordblks);
      last_arena = mi.arena;
    }
    next_heap_check = millis() + 5000;  // check every 5s
  }
#endif
}