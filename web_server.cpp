#include "web_server.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

AsyncWebServer server(80);

void startWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<html><body><h1>Temperature Dashboard</h1>";
    html += "<p>Current Temp: " + String(currentTemp) + "°C</p>";
    html += "</body></html>";
    request->send(200, "text/html", html);
  });
  server.begin();
}
