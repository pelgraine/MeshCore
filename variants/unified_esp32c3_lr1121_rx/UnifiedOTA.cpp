#if defined(ENABLE_COMPANION_OTA) && !defined(DISABLE_WIFI_OTA)
#include "UnifiedBoard.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>
#include <SPIFFS.h>

bool UnifiedBoard::startUnifiedOTA(const char* id, char reply[]) {
  static AsyncWebServer* server = nullptr;
  if (server != nullptr && server->state() == LISTEN) {
    sprintf(reply, "Started: http://%s/update", WiFi.softAPIP().toString().c_str());
    return true;
  }
#ifndef UNIFIED_KISS_OTA
  const uint32_t free_heap = ESP.getFreeHeap();
  const uint32_t largest_block = ESP.getMaxAllocHeap();
#endif
  if (!WiFi.softAP("MeshCore-OTA", NULL)) {
#ifndef UNIFIED_KISS_OTA
    Serial.printf("OTA: Wi-Fi startup failed (before: heap=%u, largest=%u; after: heap=%u, largest=%u)\n",
      free_heap, largest_block, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
#endif
    strcpy(reply, "Error: could not start OTA Wi-Fi");
    return false;
  }
  inhibit_sleep = true;   // prevent sleep during OTA

  if (server == nullptr) {
    static char id_buf[60];
    snprintf(id_buf, sizeof(id_buf), "%s (%s)", id, getManufacturerName());
    static char home_buf[128];
    snprintf(home_buf, sizeof(home_buf), "<H2>MeshCore OTA. ID: %s</H2><a href='/update'>Update firmware</a>", id);

    server = new AsyncWebServer(80);

    server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(200, "text/html", home_buf);
    });
    server->on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(SPIFFS, "/packet_log", "text/plain");
    });

    AsyncElegantOTA.setID(id_buf);
    AsyncElegantOTA.begin(server);
  }
  server->begin();
  if (server->state() != LISTEN) {
#ifndef UNIFIED_KISS_OTA
    Serial.printf("OTA: HTTP startup failed (heap=%u, largest=%u)\n",
      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
#endif
    strcpy(reply, "Error: could not start OTA HTTP server");
    return false;
  }

  sprintf(reply, "Started: http://%s/update", WiFi.softAPIP().toString().c_str());
#ifndef UNIFIED_KISS_OTA
  Serial.printf("OTA: HTTP listening on port 80 (heap=%u, largest=%u)\n",
    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
#endif
#ifndef UNIFIED_KISS_OTA
  MESH_DEBUG_PRINTLN("startOTAUpdate: %s", reply);
#endif

  return true;
}

#endif
