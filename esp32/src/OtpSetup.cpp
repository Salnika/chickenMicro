#include "OtpSetup.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <stdio.h>
#include <string.h>

#include "Log.h"

namespace otp {
namespace {

WebServer server(80);
DNSServer dns;
Setup* active = nullptr;
bool routesAdded = false;

const IPAddress kAddress(192, 168, 4, 1);

/// Who comes and goes on the setup network, counted by the Wi-Fi event task
/// and read by the loop: one writer each, and a 32-bit store is atomic here.
/// With a single client allowed, "the client" is never ambiguous.
volatile uint32_t joins = 0;         // got an address: the password was right
volatile uint32_t failedJoins = 0;   // associated, left before any address
volatile uint32_t leaves = 0;        // left after getting one
volatile bool addressed = false;
bool eventsHooked = false;

void onWifiEvent(arduino_event_id_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            addressed = false;
            break;
        case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
            addressed = true;
            joins = joins + 1;
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            if (addressed) {
                leaves = leaves + 1;
            } else {
                failedJoins = failedJoins + 1;
            }
            addressed = false;
            break;
        default:
            break;
    }
}

/// No i, l, o, 0 or 1: a password read off an e-ink panel and typed on a
/// phone should not hinge on telling them apart.
constexpr char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";

/// xxxx-xxxx-xxxx: about 59 bits, drawn once the radio is on, which is when
/// esp_random() has RF noise to draw on.
void makePassword(char (&out)[16]) {
    size_t at = 0;
    for (int group = 0; group < 3; ++group) {
        if (group > 0) out[at++] = '-';
        for (int letter = 0; letter < 4; ++letter) {
            out[at++] = kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
        }
    }
    out[at] = '\0';
}

void makeToken(char (&out)[17]) {
    snprintf(out, sizeof(out), "%08lx%08lx", static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()));
}

void wipe(void* memory, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(memory);
    while (length-- > 0) *bytes++ = 0;
}

void appendEscaped(String& page, const char* text) {
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
            case '&': page += F("&amp;"); break;
            case '<': page += F("&lt;"); break;
            case '>': page += F("&gt;"); break;
            case '"': page += F("&quot;"); break;
            case '\'': page += F("&#39;"); break;
            default: page += *cursor; break;
        }
    }
}

/// A form's number field, or 0 when it is not a plain number in range.
long formNumber(const String& text, long max) {
    if (text.length() == 0 || text.length() > 4) return 0;
    for (size_t index = 0; index < text.length(); ++index) {
        if (text[index] < '0' || text[index] > '9') return 0;
    }
    const long value = text.toInt();
    return value <= max ? value : 0;
}

}  // namespace

bool Setup::start(uint32_t nowMs) {
    if (running_) return true;

    // Nothing about this access point is worth remembering across boots.
    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_AP)) {
        logMessage(proto::LogLevel::Error, "[otp] cannot turn Wi-Fi on for the setup page");
        return false;
    }
    // Counted from before the network exists, so no arrival goes unsaid.
    if (!eventsHooked) {
        WiFi.onEvent(onWifiEvent);
        eventsHooked = true;
    }
    seenJoins_ = joins;
    seenFailedJoins_ = failedJoins;
    seenLeaves_ = leaves;

    makePassword(password_);
    makeToken(token_);
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(network_, sizeof(network_), "ChickenMicro-%04X",
             static_cast<unsigned>((mac >> 32) & 0xFFFF));

    WiFi.softAPConfig(kAddress, kAddress, IPAddress(255, 255, 255, 0));
    // One client: the first phone to join is the only one that can.
    if (!WiFi.softAP(network_, password_, /*channel=*/6, /*ssid_hidden=*/0,
                     /*max_connection=*/1)) {
        logMessage(proto::LogLevel::Error, "[otp] cannot start the setup access point");
        WiFi.mode(WIFI_OFF);
        return false;
    }
    // A phone at arm's length needs little power. Low power also keeps the
    // current spikes small, which the C3 SuperMini's regulator and antenna are
    // known to need.
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    // The same network, for a phone's camera: nothing to type, so nothing to
    // mistype. The payload holds the password, so it goes once encoded.
    char payload[96];
    if (!wifiPayload(network_, password_, payload, sizeof(payload)) || !encodeQr(payload, qr_)) {
        qr_ = QrBitmap();
        logMessage(proto::LogLevel::Warn, "[otp] no QR code for the setup Wi-Fi: join it by hand");
    }
    wipe(payload, sizeof(payload));

    // Every name resolves to the box, so a phone's captive-portal check opens
    // the page by itself.
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", kAddress);

    if (!routesAdded) {
        server.on("/", HTTP_GET, [] { if (active) active->handleRoot(); });
        server.on("/add", HTTP_POST, [] { if (active) active->handleAdd(); });
        server.on("/delete", HTTP_POST, [] { if (active) active->handleDelete(); });
        server.onNotFound([] { if (active) active->handleOther(); });
        routesAdded = true;
    }
    server.begin();

    active = this;
    running_ = true;
    lastRequestMs_ = nowMs;
    notePending_ = false;
    // Never the password: it is on the e-ink, which is the point. The Mac has
    // Wi-Fi of its own, and a log is not where a key to this network belongs.
    logf(proto::LogLevel::Info, "[otp] setup page open on Wi-Fi \"%s\"", network_);
    return true;
}

void Setup::stop() {
    if (!running_) return;
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    running_ = false;
    active = nullptr;
    wipe(password_, sizeof(password_));
    wipe(token_, sizeof(token_));
    wipe(&qr_, sizeof(qr_));
    logMessage(proto::LogLevel::Info, "[otp] setup page closed, Wi-Fi off");
}

void Setup::poll() {
    if (!running_) return;
    dns.processNextRequest();
    server.handleClient();

    // Said on the panel, so whoever is holding the phone knows which half —
    // the Wi-Fi or the page — is the one not working.
    const uint32_t joined = joins;
    const uint32_t failed = failedJoins;
    const uint32_t left = leaves;
    if (joined != seenJoins_) {
        seenJoins_ = joined;
        lastRequestMs_ = millis();
        note("Joined. Open the page.");
        logMessage(proto::LogLevel::Info, "[otp] a device joined the setup Wi-Fi");
    }
    if (failed != seenFailedJoins_) {
        seenFailedJoins_ = failed;
        lastRequestMs_ = millis();
        note("Join failed. Try again.");
        logMessage(proto::LogLevel::Warn,
                   "[otp] a device dropped off the setup Wi-Fi before getting an address; "
                   "a wrong password looks like this");
    }
    if (left != seenLeaves_) {
        seenLeaves_ = left;
        note("Left the Wi-Fi.");
        logMessage(proto::LogLevel::Info, "[otp] the device left the setup Wi-Fi");
    }
}

bool Setup::takeNote(char* out, size_t capacity) {
    if (!notePending_) return false;
    notePending_ = false;
    snprintf(out, capacity, "%s", note_);
    return true;
}

void Setup::note(const char* text) {
    snprintf(note_, sizeof(note_), "%s", text);
    notePending_ = true;
}

bool Setup::tokenMatches() {
    const String given = server.arg("t");
    const size_t length = strlen(token_);
    if (length == 0 || given.length() != length) return false;
    uint8_t difference = 0;
    for (size_t index = 0; index < length; ++index) {
        difference |= static_cast<uint8_t>(given[index] ^ token_[index]);
    }
    return difference == 0;
}

void Setup::handleRoot() {
    lastRequestMs_ = millis();
    sendPage(nullptr, true);
}

void Setup::handleOther() {
    lastRequestMs_ = millis();
    server.sendHeader("Location", address(), true);
    server.send(302, "text/plain", "");
}

void Setup::handleAdd() {
    lastRequestMs_ = millis();
    if (!tokenMatches()) {
        server.send(403, "text/plain", "This form has expired. Reload the page.");
        return;
    }
    const String uri = server.arg("uri");
    const String name = server.arg("name");
    const String secret = server.arg("secret");
    // A transfer link carries several accounts, so it may be long.
    const bool transfer = isMigrationLink(uri.c_str());
    if (uri.length() > (transfer ? kMaxMigrationLinkLength : 512) || name.length() > 64 ||
        secret.length() > 160) {
        sendPage("That is longer than any real account needs.", false);
        return;
    }
    if (transfer) {
        addTransfer(uri);
        return;
    }

    Account account;
    ParseError error = ParseError::None;
    if (uri.length() > 0) {
        error = parseOtpauthUri(uri.c_str(), account);
        if (error == ParseError::None && name.length() > 0 &&
            !renameAccount(account, name.c_str())) {
            error = ParseError::MissingName;
        }
    } else {
        Algorithm algorithm = Algorithm::Sha1;
        if (!parseAlgorithm(server.arg("algorithm").c_str(), algorithm)) {
            error = ParseError::BadAlgorithm;
        } else {
            error = makeAccount(name.c_str(), secret.c_str(),
                                static_cast<uint8_t>(formNumber(server.arg("digits"), 255)),
                                static_cast<uint16_t>(formNumber(server.arg("period"), 3600)),
                                algorithm, account);
        }
    }
    if (error != ParseError::None) {
        wipe(&account, sizeof(account));
        note("Not added: see the page");
        sendPage(describe(error), false);
        return;
    }

    const Store::AddResult result = store_.add(account);
    char message[96];
    if (result == Store::AddResult::Added) {
        snprintf(message, sizeof(message), "Added \"%s\". It is in the box's list now.",
                 account.name);
        char text[49];
        snprintf(text, sizeof(text), "Added %s", account.name);
        note(text);
    } else {
        snprintf(message, sizeof(message), "%s", describe(result));
        note("Not added: see the page");
    }
    wipe(&account, sizeof(account));
    sendPage(message, result == Store::AddResult::Added);
}

/// Every account in a Google Authenticator transfer link, each added or not
/// on its own, and the page says which went in and why the others did not.
void Setup::addTransfer(const String& uri) {
    // 3 KB of decoded payload: kept off the loop task's stack.
    static MigrationReader reader;
    const ParseError opened = reader.open(uri.c_str());
    if (opened != ParseError::None) {
        note("Not added: see the page");
        sendPage(describe(opened), false);
        return;
    }
    const uint8_t batchIndex = reader.batchIndex();
    const uint8_t batchSize = reader.batchSize();

    String details;
    details.reserve(1024);
    unsigned added = 0;
    unsigned total = 0;
    Account account;
    ParseError error = ParseError::None;
    while (reader.next(account, error)) {
        ++total;
        const char* outcome = describe(error);
        if (error == ParseError::None) {
            const Store::AddResult result = store_.add(account);
            outcome = describe(result);
            if (result == Store::AddResult::Added) ++added;
        }
        details += F("<li><b>");
        appendEscaped(details, account.name[0] != '\0' ? account.name : "(no name)");
        details += F("</b> ");
        appendEscaped(details, outcome);
        details += F("</li>");
        wipe(&account, sizeof(account));
    }
    reader.wipe();

    char message[160];
    int length = snprintf(message, sizeof(message), "Added %u of the %u accounts in that link.",
                          added, total);
    if (batchSize > 1 && length > 0 && static_cast<size_t>(length) < sizeof(message)) {
        snprintf(message + length, sizeof(message) - static_cast<size_t>(length),
                 " It is QR code %u of %u: add the others the same way.",
                 static_cast<unsigned>(batchIndex) + 1, static_cast<unsigned>(batchSize));
    }
    char text[49];
    snprintf(text, sizeof(text), "Added %u of %u", added, total);
    note(text);
    logf(proto::LogLevel::Info, "[otp] transfer link: %u of %u accounts added", added, total);
    sendPage(message, added > 0, &details);
}

void Setup::handleDelete() {
    lastRequestMs_ = millis();
    if (!tokenMatches()) {
        server.send(403, "text/plain", "This form has expired. Reload the page.");
        return;
    }
    // By name, not by position: an account added from another tab since this
    // page was drawn would otherwise shift the list and take the wrong one.
    const String name = server.arg("n");
    const int index = store_.find(name.c_str());
    if (index < 0) {
        sendPage("That account is not on the box any more.", false);
        return;
    }
    char message[96];
    if (store_.remove(static_cast<uint8_t>(index))) {
        snprintf(message, sizeof(message), "Removed \"%s\".", name.c_str());
        char text[49];
        snprintf(text, sizeof(text), "Removed %s", name.c_str());
        note(text);
        sendPage(message, true);
    } else {
        sendPage("Could not remove it. Try again.", false);
    }
}

void Setup::sendPage(const char* message, bool success, const String* details) {
    String page;
    page.reserve(4096);
    page += F("<!doctype html><html lang=en><head><meta charset=utf-8>"
              "<meta name=viewport content='width=device-width,initial-scale=1'>"
              "<title>ChickenMicro - one-time codes</title><style>"
              "body{font:16px system-ui,sans-serif;margin:0 auto;max-width:30rem;padding:1rem;"
              "color:#111;background:#fff}"
              "h1{font-size:1.3rem}h2{font-size:1.05rem;margin-top:2rem}"
              "label{display:block;margin:.9rem 0 .25rem;font-weight:600}"
              "input,textarea,select{width:100%;box-sizing:border-box;font:inherit;padding:.5rem;"
              "border:1px solid #888;border-radius:6px}"
              "button{margin-top:1rem;padding:.6rem 1.2rem;font:inherit;font-weight:600;border:0;"
              "border-radius:6px;background:#111;color:#fff}"
              ".msg{padding:.7rem;border-radius:6px;margin:1rem 0}.ok{background:#e6f4ea}"
              ".err{background:#fde8e8}.or{text-align:center;color:#666;margin:1.2rem 0 .2rem}"
              "small{color:#555}details{margin-top:1rem}"
              "ul{padding:0;list-style:none}li{display:flex;justify-content:space-between;"
              "align-items:center;gap:1rem;padding:.4rem 0;border-bottom:1px solid #ddd}"
              "li form{margin:0}li button{margin:0;padding:.35rem .8rem;background:#a00}"
              ".report li{display:block}"
              "</style></head><body><h1>Add a one-time code</h1>");
    if (message != nullptr && message[0] != '\0') {
        page += success ? F("<p class='msg ok'>") : F("<p class='msg err'>");
        appendEscaped(page, message);
        page += F("</p>");
    }
    if (details != nullptr && details->length() > 0) {
        page += F("<ul class=report>");
        page += *details;
        page += F("</ul>");
    }

    page += F("<form method=post action=/add><input type=hidden name=t value=");
    page += token_;
    page += F("><label for=uri>Link</label>"
              "<textarea id=uri name=uri rows=3 autocomplete=off autocapitalize=off "
              "spellcheck=false placeholder='otpauth://totp/GitHub:alice?secret=...'></textarea>"
              "<small>An <code>otpauth://</code> link, what a service's setup QR code holds. "
              "Or an <code>otpauth-migration://</code> link, from Google Authenticator's "
              "<i>Transfer accounts</i> QR codes: it adds every account in it at once.</small>"
              "<p class=or>or type it in</p>"
              "<label for=name>Name</label>"
              "<input id=name name=name maxlength=24 placeholder=GitHub>"
              "<small>With an otpauth:// link, fill this in only to rename the account. "
              "A transfer link keeps its own names.</small>"
              "<label for=secret>Secret key</label>"
              "<input id=secret name=secret autocomplete=off autocapitalize=characters "
              "spellcheck=false placeholder='JBSW Y3DP EHPK 3PXP'>"
              "<small>What the service shows under \"can't scan the code?\".</small>"
              "<details><summary>Advanced</summary>"
              "<label for=digits>Digits</label><select id=digits name=digits>"
              "<option>6<option>7<option>8</select>"
              "<label for=period>Period (seconds)</label>"
              "<input id=period name=period type=number value=30 min=10 max=300>"
              "<label for=algorithm>Algorithm</label><select id=algorithm name=algorithm>"
              "<option>SHA1<option>SHA256<option>SHA512</select>"
              "</details><button>Add</button></form>"
              "<h2>On the box</h2>");

    if (store_.count() == 0) {
        page += F("<p><small>No account yet.</small></p>");
    } else {
        page += F("<ul>");
        for (uint8_t index = 0; index < store_.count(); ++index) {
            page += F("<li><span>");
            appendEscaped(page, store_.name(index));
            page += F("</span><form method=post action=/delete "
                      "onsubmit=\"return confirm('Remove this account from the box?')\">"
                      "<input type=hidden name=t value=");
            page += token_;
            page += F("><input type=hidden name=n value=\"");
            appendEscaped(page, store_.name(index));
            page += F("\"><button>Remove</button></form></li>");
        }
        page += F("</ul>");
    }
    page += F("<p><small>Only names are shown here. Secrets never leave the box, and a code "
              "is only made when you press a key on it.</small></p></body></html>");

    server.sendHeader("Cache-Control", "no-store");
    server.send(success ? 200 : 400, "text/html; charset=utf-8", page);
}

}  // namespace otp
