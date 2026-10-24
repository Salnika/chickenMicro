// ---------------------------------------------------------------------------
// Renders every screen to a PNG-able bitmap, on a Mac, with no panel attached.
//
//     tools/preview/render.sh          -> tools/preview/out/screens.png
//
// The point is to be able to *look* at a layout before flashing it: text that
// overruns its band, a label that collides with the one beside it, or a key
// legend that does not fit are all invisible in a compiler's output and obvious
// in an image.
//
// It compiles the real Display.cpp, State.cpp and TextLayout.cpp against a
// stub driver, so what it draws is what the firmware draws.
// ---------------------------------------------------------------------------

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "Display.h"
#include "DisplayDriver.h"
#include "DisplayModel.h"
#include "Protocol.h"
#include "State.h"
#include "WifiQr.h"

// --- Logging stubs ----------------------------------------------------------

void setLogLevel(proto::LogLevel) {}
void logMessage(proto::LogLevel, const char* message) { fprintf(stderr, "  %s\n", message); }
void logf(proto::LogLevel, const char* format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "  ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

namespace {

constexpr int16_t kWidth = EINK_WIDTH;
constexpr int16_t kHeight = EINK_HEIGHT;

/// A DisplayDriver that renders into RAM, so the layout code runs unmodified.
class PreviewDriver final : public ui::DisplayDriver {
public:
    bool begin() override { return true; }
    int16_t width() const override { return kWidth; }
    int16_t height() const override { return kHeight; }
    bool supportsPartial() const override { return true; }

    void render(ui::RefreshMode, const ui::DrawFunction& draw) override {
        canvas_.fillScreen(paper());
        canvas_.setTextColor(ink());
        draw(canvas_);
    }

    void hibernate() override {}
    const char* modelName() const override { return "preview"; }
    Adafruit_GFX& metrics() override { return canvas_; }
    uint16_t ink() const override { return 1; }
    uint16_t paper() const override { return 0; }

    GFXcanvas1& canvas() { return canvas_; }

private:
    GFXcanvas1 canvas_{kWidth, kHeight};
};

// --- Scene construction -----------------------------------------------------

proto::InboundFrame sessionsFrame(
    const std::vector<std::pair<const char*, proto::ClaudeState>>& sessions,
    const char* focus,
    const char* activePermission = "") {
    proto::InboundFrame frame;
    frame.type = proto::InboundType::Sessions;
    for (const auto& session : sessions) {
        if (frame.sessionCount >= proto::kMaxSessions) break;
        proto::SessionEntry& entry = frame.sessions[frame.sessionCount];
        snprintf(entry.id, sizeof(entry.id), "s%u", frame.sessionCount + 1);
        snprintf(entry.name, sizeof(entry.name), "%s", session.first);
        entry.state = session.second;
        ++frame.sessionCount;
    }
    frame.sessionTotal = frame.sessionCount;
    snprintf(frame.focus, sizeof(frame.focus), "%s", focus);
    snprintf(frame.activePermission, sizeof(frame.activePermission), "%s", activePermission);
    return frame;
}

proto::InboundFrame permissionFrame(
    const char* id, const char* kind, const char* title, const char* content,
    const char* session, bool approve = true, bool deny = true) {
    proto::InboundFrame frame;
    frame.type = proto::InboundType::Permission;
    snprintf(frame.id, sizeof(frame.id), "%s", id);
    snprintf(frame.kind, sizeof(frame.kind), "%s", kind);
    snprintf(frame.title, sizeof(frame.title), "%s", title);
    snprintf(frame.content, sizeof(frame.content), "%s", content);
    snprintf(frame.session, sizeof(frame.session), "%s", session);
    frame.offersApprove = approve;
    frame.offersDeny = deny;
    return frame;
}

struct Scene {
    const char* name;
    std::vector<uint8_t> pixels;   // one byte per pixel, 1 = ink
};

std::vector<Scene> scenes;

void capture(const char* name, ui::Display& display, PreviewDriver& driver, app::State& state) {
    state.setPageCount(display.pageCountFor(state));
    display.draw(state, 0, /*force=*/true);

    Scene scene;
    scene.name = name;
    scene.pixels.resize(static_cast<size_t>(kWidth) * kHeight);
    for (int16_t y = 0; y < kHeight; ++y) {
        for (int16_t x = 0; x < kWidth; ++x) {
            scene.pixels[static_cast<size_t>(y) * kWidth + x] =
                driver.canvas().getPixel(x, y) ? 1 : 0;
        }
    }
    scenes.push_back(scene);
    fprintf(stderr, "rendered %s (%u page(s))\n", name, state.pageCount());
}

// --- Contact sheet ----------------------------------------------------------

constexpr int kScale = 2;
constexpr int kColumns = 2;
constexpr int kLabelHeight = 12;
constexpr int kGap = 10;

/// Writes a 24-bit BMP, which `sips` converts to PNG without another library.
void writeBmp(const char* path, const std::vector<uint8_t>& mono, int width, int height) {
    const int rowBytes = width * 3;
    const int padding = (4 - (rowBytes % 4)) % 4;
    const int imageBytes = (rowBytes + padding) * height;
    const int fileBytes = 54 + imageBytes;

    FILE* file = fopen(path, "wb");
    if (file == nullptr) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    uint8_t header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    const uint32_t fileSize = static_cast<uint32_t>(fileBytes);
    memcpy(header + 2, &fileSize, 4);
    const uint32_t offset = 54;
    memcpy(header + 10, &offset, 4);
    const uint32_t headerSize = 40;
    memcpy(header + 14, &headerSize, 4);
    const int32_t w = width;
    const int32_t h = height;
    memcpy(header + 18, &w, 4);
    memcpy(header + 22, &h, 4);
    const uint16_t planes = 1;
    const uint16_t bits = 24;
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &bits, 2);
    fwrite(header, 1, sizeof(header), file);

    std::vector<uint8_t> row(rowBytes + padding, 0);
    for (int y = height - 1; y >= 0; --y) {       // BMP rows run bottom-up
        for (int x = 0; x < width; ++x) {
            const uint8_t value = mono[static_cast<size_t>(y) * width + x] ? 0x00 : 0xFF;
            row[x * 3 + 0] = value;
            row[x * 3 + 1] = value;
            row[x * 3 + 2] = value;
        }
        fwrite(row.data(), 1, row.size(), file);
    }
    fclose(file);
}

}  // namespace

int main() {
    PreviewDriver driver;
    ui::Display display(driver);
    display.begin();

    const char* kReply =
        "I'll split the sorting logic out of Table.tsx into a hook so it can be tested on its "
        "own, then update the snapshot tests and check the empty-state rendering.";

    // 1. Waiting for the Mac.
    {
        app::State state;
        state.setLoaderFrame(2);
        capture("connecting", display, driver, state);
    }

    // 2. One session, mid-turn.
    {
        app::State state;
        state.setLinkUp(true);
        auto frame = sessionsFrame({{"chickenMicro", proto::ClaudeState::Writing}}, "s1");
        state.setSessions(frame);
        state.setClaudeState(proto::ClaudeState::Writing, "Improve the device interface");
        state.setMessage(kReply);
        state.reconcileScreen();
        capture("home - one session", display, driver, state);

        // The chicken on its beat: out to the right with its legs together,
        // then on the way back with them apart.
        for (int step = 0; step < 9; ++step) state.advanceWalk();
        capture("home - chicken walking right", display, driver, state);
        for (int step = 0; step < 25; ++step) state.advanceWalk();
        capture("home - chicken walking back", display, driver, state);
    }

    // 3. No Claude window open.
    {
        app::State state;
        state.setLinkUp(true);
        auto frame = sessionsFrame({}, "");
        state.setSessions(frame);
        state.setClaudeState(proto::ClaudeState::Disconnected, "");
        state.reconcileScreen();
        capture("home - nothing running", display, driver, state);
    }

    // 4. Three sessions, following the second.
    {
        app::State state;
        state.setLinkUp(true);
        auto frame = sessionsFrame({{"chickenMicro", proto::ClaudeState::Idle},
                                    {"table-header refactor", proto::ClaudeState::Thinking},
                                    {"docs", proto::ClaudeState::WaitingPermission}},
                                   "s2");
        state.setSessions(frame);
        state.setClaudeState(proto::ClaudeState::Thinking, "Split the sorting logic out");
        state.setMessage(kReply);
        state.reconcileScreen();
        capture("home - three sessions", display, driver, state);
        state.setScreen(app::Screen::Sessions);
        state.setCursor(2);
        capture("session list", display, driver, state);
        state.setScreen(app::Screen::Message);
        capture("message", display, driver, state);
    }

    // 5. A full list, scrolled.
    {
        app::State state;
        state.setLinkUp(true);
        auto frame = sessionsFrame({{"chickenMicro", proto::ClaudeState::Writing},
                                    {"table-header refactor", proto::ClaudeState::Idle},
                                    {"docs", proto::ClaudeState::WaitingPermission},
                                    {"protocol notes", proto::ClaudeState::Done},
                                    {"eink bring-up", proto::ClaudeState::Idle},
                                    {"joystick calibration", proto::ClaudeState::Error},
                                    {"wire protocol", proto::ClaudeState::Thinking},
                                    {"spare", proto::ClaudeState::Idle}},
                                   "s1");
        frame.sessionTotal = 9;
        state.setSessions(frame);
        state.setScreen(app::Screen::Sessions);
        state.setCursor(6);
        capture("session list - scrolled", display, driver, state);
    }

    // 6. The permission Claude Desktop actually raises, in French.
    {
        app::State state;
        state.setLinkUp(true);
        auto sessions = sessionsFrame({{"chickenMicro", proto::ClaudeState::WaitingPermission},
                                       {"docs", proto::ClaudeState::WaitingPermission}},
                                      "s1", "req-1");
        state.setSessions(sessions);
        auto permission = permissionFrame(
            "req-1", "write", "Autoriser Claude a modifier selectors.js ?",
            "Permission demandee : modifier. Le fichier "
            "mac/src/accessibility/selectors.js sera reecrit.",
            "s1");
        state.setPermission(permission, 0);
        state.reconcileScreen();
        capture("permission - write", display, driver, state);
    }

    // 7. A long command, paginated.
    {
        app::State state;
        state.setLinkUp(true);
        auto sessions = sessionsFrame({{"chickenMicro", proto::ClaudeState::WaitingPermission}},
                                      "s1", "req-2");
        state.setSessions(sessions);
        auto permission = permissionFrame(
            "req-2", "shell", "Run command",
            "git push --force-with-lease origin refactor/table-header && gh pr create "
            "--base main --title \"Refactor table header\" --body \"Splits the sorting "
            "logic out of Table.tsx\"",
            "s1");
        state.setPermission(permission, 0);
        state.reconcileScreen();
        capture("permission - shell, page 1", display, driver, state);
        state.setPageCount(display.pageCountFor(state));
        state.setPage(1);
        capture("permission - shell, page 2", display, driver, state);
    }

    // 8. Deny only: Claude offered a standing grant, which is never bound.
    {
        app::State state;
        state.setLinkUp(true);
        auto sessions = sessionsFrame({{"chickenMicro", proto::ClaudeState::WaitingPermission}},
                                      "s1", "req-3");
        state.setSessions(sessions);
        auto permission = permissionFrame("req-3", "tool", "", "Fetch https://example.com/data.json",
                                          "s1", /*approve=*/false, /*deny=*/true);
        state.setPermission(permission, 0);
        state.reconcileScreen();
        capture("permission - deny only", display, driver, state);
    }

    // 9. A queue: two windows waiting at once, showing the approve-all hint.
    {
        app::State state;
        state.setLinkUp(true);
        auto sessions = sessionsFrame({{"chickenMicro", proto::ClaudeState::WaitingPermission},
                                       {"docs", proto::ClaudeState::WaitingPermission},
                                       {"protocol notes", proto::ClaudeState::WaitingPermission}},
                                      "s1", "req-4");
        state.setSessions(sessions);
        auto permission = permissionFrame("req-4", "shell", "Run command",
                                          "git push origin master", "s1");
        state.setPermission(permission, 0);
        state.reconcileScreen();
        capture("permission - queue of 3", display, driver, state);
    }

    // 9b. Sessions named after their conversations: the longest the wire allows,
    // beside the state tag they must not run into.
    {
        app::State state;
        state.setLinkUp(true);
        auto sessions = sessionsFrame({{"Claude Desktop ESP32 controlle", proto::ClaudeState::Thinking},
                                       {"Migration React 19", proto::ClaudeState::WaitingPermission},
                                       {"Wiring the buzzer and LEDs", proto::ClaudeState::Idle}},
                                      "s1");
        state.setSessions(sessions);
        state.setScreen(app::Screen::Sessions);
        capture("session list - long names", display, driver, state);
    }

    // 10. The long-press menu.
    {
        app::State state;
        state.setLinkUp(true);
        auto sessions = sessionsFrame({{"chickenMicro", proto::ClaudeState::Idle}}, "s1");
        state.setSessions(sessions);
        state.setScreen(app::Screen::Menu);
        capture("menu", display, driver, state);
    }

    // 11. One-time codes: the list, the code just copied, and adding one.
    {
        app::State state;
        state.setLinkUp(true);
        auto sessions = sessionsFrame({{"chickenMicro", proto::ClaudeState::Idle}}, "s1");
        state.setSessions(sessions);
        state.setClock(1726000000000ull, 0);
        const char* names[] = {"AWS:root", "GitHub:alice", "Google", "Proton"};
        state.setOtpAccounts(4, names);
        state.moveOtpCursor(1);
        state.setScreen(app::Screen::OtpList);
        capture("otp - list", display, driver, state);

        state.showOtpCode("GitHub:alice", "042917", 18, 0);
        state.setScreen(app::Screen::OtpCode);
        capture("otp - code copied", display, driver, state);
        state.expireOtpCode();
        capture("otp - code expired", display, driver, state);

        // The real QR code, so the sheet can be held up to a phone.
        char payload[96];
        otp::QrBitmap qr;
        otp::wifiPayload("ChickenMicro-4F2A", "k7mp-q2xa-9rtz", payload, sizeof(payload));
        otp::encodeQr(payload, qr);
        state.setOtpSetup("ChickenMicro-4F2A", "k7mp-q2xa-9rtz", "http://192.168.4.1", qr);
        state.setScreen(app::Screen::OtpSetup);
        capture("otp - add, waiting", display, driver, state);
        state.setSetupNote("Joined. Open the page.");
        capture("otp - add, phone joined", display, driver, state);
        state.setSetupNote("Added Microsoft:alice.smith");
        capture("otp - add, long name added", display, driver, state);
        state.setOtpSetup("ChickenMicro-4F2A", "k7mp-q2xa-9rtz", "http://192.168.4.1",
                          otp::QrBitmap());
        capture("otp - add, no QR code", display, driver, state);
    }

    // 12. No account yet, and the Mac's clock not in: nothing to copy.
    {
        app::State state;
        state.setLinkUp(true);
        auto sessions = sessionsFrame({{"chickenMicro", proto::ClaudeState::Idle}}, "s1");
        state.setSessions(sessions);
        state.setScreen(app::Screen::OtpList);
        capture("otp - list, empty, no clock", display, driver, state);
    }

    // --- Compose the contact sheet ------------------------------------------

    const int cellWidth = kWidth * kScale;
    const int cellHeight = kHeight * kScale;
    const int rows = static_cast<int>((scenes.size() + kColumns - 1) / kColumns);
    const int sheetWidth = kColumns * cellWidth + (kColumns + 1) * kGap;
    const int sheetHeight = rows * (cellHeight + kLabelHeight + kGap) + kGap;

    GFXcanvas1 sheet(sheetWidth, sheetHeight);
    sheet.fillScreen(0);
    sheet.setTextColor(1);
    sheet.setFont(nullptr);
    sheet.setTextSize(1);

    for (size_t index = 0; index < scenes.size(); ++index) {
        const int column = static_cast<int>(index % kColumns);
        const int row = static_cast<int>(index / kColumns);
        const int x0 = kGap + column * (cellWidth + kGap);
        const int y0 = kGap + row * (cellHeight + kLabelHeight + kGap);

        sheet.setCursor(x0, y0);
        sheet.print(scenes[index].name);

        const int top = y0 + kLabelHeight;
        sheet.drawRect(x0 - 1, top - 1, cellWidth + 2, cellHeight + 2, 1);
        for (int y = 0; y < cellHeight; ++y) {
            for (int x = 0; x < cellWidth; ++x) {
                const size_t source =
                    static_cast<size_t>(y / kScale) * kWidth + static_cast<size_t>(x / kScale);
                if (scenes[index].pixels[source]) sheet.drawPixel(x0 + x, top + y, 1);
            }
        }
    }

    std::vector<uint8_t> flat(static_cast<size_t>(sheetWidth) * sheetHeight);
    for (int y = 0; y < sheetHeight; ++y) {
        for (int x = 0; x < sheetWidth; ++x) {
            flat[static_cast<size_t>(y) * sheetWidth + x] = sheet.getPixel(x, y) ? 1 : 0;
        }
    }
    writeBmp("tools/preview/out/screens.bmp", flat, sheetWidth, sheetHeight);
    fprintf(stderr, "wrote tools/preview/out/screens.bmp (%dx%d)\n", sheetWidth, sheetHeight);
    return 0;
}
