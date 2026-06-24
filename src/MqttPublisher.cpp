#include "MqttPublisher.h"

#include <WiFi.h>
#include "DeviceIdentity.h"

void MqttPublisher::Begin(const Config& cfg)
{
    if (taskHandle == nullptr) {
        // depth-1 config slot (only the newest config matters); a few publish slots
        configQueue = xQueueCreate(1, sizeof(Config*));
        publishQueue = xQueueCreate(8, sizeof(Message*));

        // 6 KB stack: PubSubClient + a plain (non-TLS) WiFiClient are light. Priority
        // 1, like the loop and fetch tasks; it spends most of its life delayed.
        xTaskCreate(Trampoline, "mqtt_pub", 6144, this, 1, &taskHandle);
    }

    // hand the task a fresh config; replace any pending one so it always sees the latest
    Config* c = new Config(cfg);
    Config* stale = nullptr;
    if (xQueueReceive(configQueue, &stale, 0) == pdTRUE) delete stale;
    if (xQueueSend(configQueue, &c, 0) != pdTRUE) delete c;
}

void MqttPublisher::Publish(const String& topic, const String& payload, bool retained)
{
    if (publishQueue == nullptr)
        return;
    Message* m = new Message{ topic, payload, retained };
    if (xQueueSend(publishQueue, &m, 0) != pdTRUE)
        delete m; // queue full (broker down): drop rather than block the loop
}

bool MqttPublisher::ConsumeJustConnected()
{
    if (!justConnected)
        return false;
    justConnected = false;
    return true;
}

void MqttPublisher::Trampoline(void* arg)
{
    static_cast<MqttPublisher*>(arg)->Run();
}

void MqttPublisher::Run()
{
    const String clientId = DeviceIdentity::Name();
    Config cfg;
    bool haveCfg = false;
    unsigned long lastConnectAttempt = 0;

    client.setBufferSize(1024); // headroom for the summary + HA discovery configs

    for (;;) {
        // pick up a new config without blocking; reconnect on change
        Config* nc = nullptr;
        if (xQueueReceive(configQueue, &nc, 0) == pdTRUE && nc) {
            cfg = *nc;
            delete nc;
            haveCfg = true;
            if (client.connected()) client.disconnect();
            lastConnectAttempt = 0; // allow an immediate (re)connect with the new settings
        }

        const bool ready = haveCfg && cfg.enabled && !cfg.host.isEmpty() &&
                           WiFi.status() == WL_CONNECTED;
        if (!ready) {
            connectedFlag = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!client.connected()) {
            connectedFlag = false;
            const unsigned long now = millis();
            if (now - lastConnectAttempt < 5000) { // back off between attempts
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            lastConnectAttempt = now;

            client.setServer(cfg.host.c_str(), cfg.port);
            net.setConnectionTimeout(3000); // cap a dead-broker connect at 3s

            const char* user = cfg.user.isEmpty() ? nullptr : cfg.user.c_str();
            const char* pass = cfg.pass.isEmpty() ? nullptr : cfg.pass.c_str();
            // last-will: broker marks us "offline" (retained) if we drop
            const bool ok = client.connect(clientId.c_str(), user, pass,
                                           cfg.statusTopic.c_str(), 0, true, "offline");
            if (ok) {
                client.publish(cfg.statusTopic.c_str(), "online", true);
                connectedFlag = true;
                justConnected = true;
                Serial.printf("[mqtt] connected to %s:%u\n", cfg.host.c_str(), cfg.port);
            } else {
                Serial.printf("[mqtt] connect to %s:%u failed (state %d)\n",
                              cfg.host.c_str(), cfg.port, client.state());
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }

        client.loop();
        connectedFlag = client.connected();

        // drain queued publishes
        Message* m = nullptr;
        while (xQueueReceive(publishQueue, &m, 0) == pdTRUE && m) {
            if (client.connected())
                client.publish(m->topic.c_str(), (const uint8_t*)m->payload.c_str(),
                               m->payload.length(), m->retained);
            delete m;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
