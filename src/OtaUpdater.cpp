#include "OtaUpdater.h"
#include "LGFX.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

namespace {

// "releases/latest/download/<asset>" always resolves to the newest published
// (non-draft, non-prerelease) release's asset, redirecting to the CDN.
const char* VERSION_URL  = "https://github.com/Valar-Systems/Blipscope/releases/latest/download/version.txt";
const char* FIRMWARE_URL = "https://github.com/Valar-Systems/Blipscope/releases/latest/download/firmware.bin";

void drawStatus(LGFX& tft, const String& msg)
{
    tft.fillScreen(lgfx::color888(0, 0, 0));
    tft.setTextColor(lgfx::color888(0, 255, 0));
    tft.drawCentreString(msg, 120, 108);
}

} // namespace

void MaybeUpdateFirmware(LGFX& tft)
{
    // 1. read the latest published version. setInsecure() skips certificate
    // validation -- acceptable here, matching the rest of the device's HTTPS.
    WiFiClientSecure verClient;
    verClient.setInsecure();

    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // GitHub redirects to its CDN
    http.setConnectTimeout(8000);
    http.setTimeout(8000);

    if (!http.begin(verClient, VERSION_URL)) {
        Serial.println("[ota] version check: begin failed");
        return;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[ota] version check: HTTP %d\n", code);
        http.end();
        return;
    }

    const int latest = http.getString().toInt();
    http.end();

    Serial.printf("[ota] current=%d latest=%d\n", FW_VERSION, latest);
    if (latest <= FW_VERSION)
        return; // already up to date

    // 2. download + flash the new image, showing a progress bar on the screen
    Serial.println("[ota] newer firmware available -- updating");
    drawStatus(tft, "Updating firmware...");

    WiFiClientSecure updClient;
    updClient.setInsecure();

    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress([&tft](int current, int total) {
        const int pct = total > 0 ? (int)((current * 100L) / total) : 0;
        tft.fillRect(50, 140, 140, 14, lgfx::color888(0, 40, 0));
        tft.fillRect(52, 142, (136 * pct) / 100, 10, lgfx::color888(0, 255, 0));
    });

    const t_httpUpdate_return ret = httpUpdate.update(updClient, FIRMWARE_URL);
    if (ret == HTTP_UPDATE_FAILED) {
        Serial.printf("[ota] update failed (%d): %s\n",
                      httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
        drawStatus(tft, "Update failed");
        delay(2000);
    }
    // HTTP_UPDATE_OK reboots automatically (rebootOnUpdate(true))
}
