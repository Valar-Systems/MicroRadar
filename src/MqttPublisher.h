#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <PubSubClient.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Minimal MQTT transport for the Home Assistant integration. All the socket work
// -- connecting (which can block when the broker is unreachable), keepalive, and
// publishing -- runs on a dedicated FreeRTOS task so it never stalls the render
// loop, mirroring the background OpenSky fetch. The loop task only ever hands it
// fully-formed (topic, payload) messages through a queue; this class knows
// nothing about aircraft, so AircraftManager owns all the payload/topic shaping.
class MqttPublisher {
public:
    struct Config {
        bool enabled = false;
        String host;
        uint16_t port = 1883;
        String user;
        String pass;
        String statusTopic;       // LWT / availability topic ("<base>/status")
    };

    // (Re)configure. Spawns the worker task on the first call; later calls hand
    // the task a fresh config (it reconnects with the new settings). Safe to call
    // from the loop task on every config reload.
    void Begin(const Config& cfg);

    // Enqueue a publish (called from the loop task). The strings are copied into
    // the queued message, so the caller keeps ownership. Dropped if the queue is
    // full (broker down / slow) so the loop never blocks.
    void Publish(const String& topic, const String& payload, bool retained);

    bool Connected() const { return connectedFlag; }

    // True once after each successful (re)connect, so the loop can (re)publish the
    // retained discovery configs and a fresh state snapshot.
    bool ConsumeJustConnected();

private:
    WiFiClient net;
    PubSubClient client{net};

    TaskHandle_t taskHandle = nullptr;
    QueueHandle_t configQueue = nullptr;   // loop -> task: Config* (depth 1, latest wins)
    QueueHandle_t publishQueue = nullptr;  // loop -> task: Message* (depth N)

    volatile bool connectedFlag = false;
    volatile bool justConnected = false;

    struct Message { String topic; String payload; bool retained; };

    static void Trampoline(void* arg);
    void Run();
};
