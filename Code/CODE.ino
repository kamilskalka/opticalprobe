#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

class MeterModel {
public:
    String voltage = "--";
    String current = "--";
    String energy  = "--";
    String status  = "No read yet";
    String raw     = "";

    void update(const String& v, const String& i, const String& e, const String& rawTelegram) {
        voltage = v;
        current = i;
        energy  = e;
        raw     = rawTelegram;
        status  = "OK @ " + String(millis() / 1000) + "s";
    }

    void setError(const String& msg) {
        status = "Error: " + msg;
    }
};

class IIECReader {
public:
    virtual bool read(String& outTelegram) = 0;
    virtual ~IIECReader() {}
};

class IEC6205621Reader : public IIECReader {
private:
    const int TX_PIN;
    const int RX_PIN;

    String obisValue(const String& telegram, const String& code) {
        int idx = telegram.indexOf(code);
        if (idx == -1) return "";
        int open  = telegram.indexOf('(', idx);
        int close = telegram.indexOf(')', open);
        if (open == -1 || close == -1) return "";
        String val = telegram.substring(open + 1, close);
        int star = val.indexOf('*');
        if (star != -1) val = val.substring(0, star);
        return val;
    }

public:
    IEC6205621Reader(int txPin, int rxPin) : TX_PIN(txPin), RX_PIN(rxPin) {}

    bool read(String& outTelegram) override {
        Serial.println("[IECReader] Starting read sequence");

        Serial1.begin(300, SERIAL_7E1, RX_PIN, TX_PIN);
        delay(500);
        while (Serial1.available()) Serial1.read();

        Serial1.print("/?!\r\n");
        Serial1.flush();

        String ident = "";
        unsigned long t = millis();
        while (millis() - t < 3000) {
            while (Serial1.available()) {
                char c = (char)Serial1.read();
                ident += c;
                t = millis();
                if (c == '\n' && ident.length() > 3) goto ident_done;
            }
        }
        ident_done:

        if (ident.indexOf('/') == -1) {
            Serial1.end();
            pinMode(TX_PIN, OUTPUT);
            digitalWrite(TX_PIN, LOW);
            return false;
        }

        char baudChar = '0';
        int slashPos = ident.indexOf('/');
        if (slashPos != -1 && (int)ident.length() >= slashPos + 5)
            baudChar = ident.charAt(slashPos + 4);

        int newBaud = 300;
        switch (baudChar) {
            case '1': newBaud =   600; break;
            case '2': newBaud =  1200; break;
            case '3': newBaud =  2400; break;
            case '4': newBaud =  4800; break;
            case '5': newBaud =  9600; break;
            case '6': newBaud = 19200; break;
            default:  newBaud =   300; break;
        }

        String ack = "\x06""0"; ack += baudChar; ack += "0\r\n";
        Serial1.print(ack);
        Serial1.flush();

        delay(220);
        Serial1.end();
        delay(50);
        Serial1.begin(newBaud, SERIAL_7E1, RX_PIN, TX_PIN);

        String payload = "";
        t = millis();
        bool ended = false;
        while (millis() - t < 8000) {
            while (Serial1.available()) {
                char c = (char)Serial1.read();
                payload += c;
                t = millis();
                if (c == '!') ended = true;
                if (ended && payload.endsWith("!\r\n")) goto read_done;
            }
        }
        read_done:

        Serial1.end();
        pinMode(TX_PIN, OUTPUT);
        digitalWrite(TX_PIN, LOW);

        if (payload.length() < 5) return false;

        outTelegram = payload;
        return true;
    }

    String parseObis(const String& telegram, const String& code) {
        return obisValue(telegram, code);
    }
};

class MeterController {
private:
    MeterModel&      model;
    IEC6205621Reader reader;
    WebServer        server;

    String renderView() {
        String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                   "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                   "<title>IEC Meter</title>";
        h += "<style>"
             "body{font-family:sans-serif;text-align:center;background:#f4f4f4;margin:0;padding:0;}"
             ".card{background:#fff;padding:30px;border-radius:12px;display:inline-block;"
             "margin-top:50px;box-shadow:0 4px 12px rgba(0,0,0,0.1);min-width:260px;}"
             ".val{font-size:1.4em;font-weight:bold;color:#007bff;}"
             ".lbl{color:#555;margin-top:12px;margin-bottom:2px;}"
             ".status{color:#888;font-size:0.85em;margin-top:16px;}"
             "button{margin-top:24px;padding:10px 24px;cursor:pointer;background:#28a745;"
             "color:#fff;border:none;border-radius:6px;font-size:1em;}"
             "button:disabled{background:#aaa;}"
             "details{margin-top:16px;text-align:left;}"
             "summary{cursor:pointer;color:#555;font-size:0.85em;}"
             "pre{background:#eee;padding:10px;font-size:0.72em;"
             "white-space:pre-wrap;word-break:break-all;border-radius:6px;}"
             "</style></head><body>";
        h += "<div class='card'>"
             "<h1>⚡ Energy Meter</h1>";
        h += "<p class='lbl'>Voltage</p><p class='val'>" + model.voltage + "</p>";
        h += "<p class='lbl'>Current</p><p class='val'>" + model.current + "</p>";
        h += "<p class='lbl'>Energy</p><p class='val'>"  + model.energy  + "</p>";
        h += "<p class='status'>" + model.status + "</p>";
        h += "<button id='btn' onclick='go()'>FETCH DATA</button>";
        if (model.raw.length() > 0)
            h += "<details><summary>Raw telegram</summary><pre>" + model.raw + "</pre></details>";
        h += "</div>";
        h += "<script>"
             "function go(){"
             "var b=document.getElementById('btn');"
             "b.disabled=true;b.innerText='Reading...';"
             "fetch('/refresh').then(()=>setTimeout(()=>location.reload(),3500));}"
             "</script>";
        h += "</body></html>";
        return h;
    }

    void handleRoot() {
        server.send(200, "text/html", renderView());
    }

    void handleRefresh() {
        String telegram;
        if (reader.read(telegram)) {
            String v = reader.parseObis(telegram, "1-0:32.7.0");
            if (v == "") v = reader.parseObis(telegram, "1-0:12.7.0");
            String i = reader.parseObis(telegram, "1-0:31.7.0");
            if (i == "") i = reader.parseObis(telegram, "1-0:11.7.0");
            String e = reader.parseObis(telegram, "1-0:1.8.0");
            if (e == "") e = reader.parseObis(telegram, "1-0:1.8.1");

            model.update(
                (v != "") ? v + " V"   : "N/A",
                (i != "") ? i + " A"   : "N/A",
                (e != "") ? e + " kWh" : "N/A",
                telegram
            );
        } else {
            model.setError("Read failed");
        }
        server.send(200, "text/plain", "OK");
    }

public:
    MeterController(MeterModel& m, int txPin, int rxPin)
        : model(m), reader(txPin, rxPin), server(80) {}

    void begin() {
        server.on("/",        [this]() { handleRoot(); });
        server.on("/refresh", [this]() { handleRefresh(); });
        server.begin();
    }

    void loop() {
        server.handleClient();
    }
};

const char* SSID     = "network";
const char* PASSWORD = "password";

const int TX_PIN = 21; 
const int RX_PIN = 20;

MeterModel      model;
MeterController controller(model, TX_PIN, RX_PIN);

void setup() {
    Serial.begin(115200);
    pinMode(TX_PIN, OUTPUT);
    digitalWrite(TX_PIN, LOW);
    setCpuFrequencyMhz(80);
    WiFi.begin(SSID, PASSWORD);
    while (WiFi.status() != WL_CONNECTED) { delay(500); }
    MDNS.begin("meter");
    controller.begin();
}

void loop() {
    controller.loop();
}