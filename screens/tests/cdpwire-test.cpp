// The DevTools wire (screens/cdpwire.h): socket URLs, the handshake, frames both ways, Steam's
// target list and the messages sent.
#include "../cdpwire.h"

#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

using namespace cdpwire;

int main() {
    {
        std::string hp, path;
        check("a socket URL split", SplitWsUrl("ws://127.0.0.1:8080/devtools/page/AB12", &hp, &path) &&
                                        hp == "127.0.0.1:8080" && path == "/devtools/page/AB12");
        check("not a socket URL", !SplitWsUrl("http://127.0.0.1:8080/x", &hp, &path) && !SplitWsUrl("ws:///x", &hp, &path));
        check("switched", Upgraded("HTTP/1.1 101 WebSocket Protocol Handshake\r\nUpgrade: websocket\r\n\r\n"));
        check("refused", !Upgraded("HTTP/1.1 404 Not Found\r\n\r\n"));
        check("the request", UpgradeRequest("h:1", "/p", "k==").find("GET /p HTTP/1.1\r\nHost: h:1\r\n") == 0);
    }
    {   // frames: ours masked, read back by TakeFrame (which reads masked ones too)
        const uint8_t mask[4] = {1, 2, 3, 4};
        for (size_t n : {size_t(5), size_t(125), size_t(126), size_t(300), size_t(70000)}) {
            const std::string text(n, 'x');
            std::string buf = TextFrame(text, mask);
            const bool masked = (uint8_t(buf[1]) & 0x80) != 0;
            int op = 0;
            std::string got;
            char what[64];
            std::snprintf(what, sizeof what, "a %zu byte frame, masked, both ways", n);
            check(what, masked && TakeFrame(buf, &op, &got) && op == 1 && got == text && buf.empty());
        }
        std::string server = std::string("\x81\x05hello", 7) + std::string("\x88\x00", 2);
        int op = 0;
        std::string got;
        check("a server's text frame", TakeFrame(server, &op, &got) && op == 1 && got == "hello");
        check("then its close", TakeFrame(server, &op, &got) && op == 8 && got.empty() && server.empty());
        std::string part("\x81\x7e\x01", 3);
        check("half a frame: wait", !TakeFrame(part, &op, &got) && part.size() == 3);
    }
    {   // GET /json as Steam answers it
        const std::string json = R"J([ {
   "description": "",
   "id": "A1",
   "title": "SteamVR - valve.steam.gamepadui.frame.menu.67340005 (67340005)",
   "type": "page",
   "url": "about:blank?createflags=2&vrOverlayKey=valve.steam.gamepadui.frame.menu.67340005",
   "webSocketDebuggerUrl": "ws://127.0.0.1:8080/devtools/page/A1"
}, {
   "title": "SteamVR",
   "devtoolsFrontendUrl": "/devtools/inspector.html?ws=127.0.0.1:8080/devtools/page/B2",
   "webSocketDebuggerUrl": "ws://127.0.0.1:8080/devtools/page/B2"
}, {
   "title": "an \"odd\" one, {braces} [and] é",
   "webSocketDebuggerUrl": "ws://127.0.0.1:8080/devtools/page/C3"
}, {
   "title": "attached elsewhere: no socket"
} ])J";
        const std::vector<Target> t = Targets(json);
        check("three pages with sockets", t.size() == 3);
        check("the menu's", t.size() > 0 && t[0].title == "SteamVR - valve.steam.gamepadui.frame.menu.67340005 (67340005)" &&
                                t[0].ws == "ws://127.0.0.1:8080/devtools/page/A1");
        check("the main one's, not the inspector's", t.size() > 1 && t[1].title == "SteamVR" && t[1].ws.find("/B2") != std::string::npos);
        check("escapes in a title", t.size() > 2 && t[2].title == "an \"odd\" one, {braces} [and] \xc3\xa9");
        check("nothing from nothing", Targets("").empty() && Targets("[]").empty());
    }
    {
        double v = 0;
        check("the ratio in a reply", NumberAfter(R"({"id":1,"result":{"result":{"type":"number","value":1.5}}})", "value", &v) && v == 1.5);
        check("no number", !NumberAfter(R"({"id":1,"result":{}})", "value", &v));
        check("reply 1", IsReply(R"({"id":1,"result":{}})", 1) && !IsReply(R"({"id":12,"result":{}})", 1));
        check("a quoted expression", Evaluate(3, "a\"b") == R"({"id":3,"method":"Runtime.evaluate","params":{"expression":"a\"b","returnByValue":true}})");
        const std::string press = MouseEvent(7, Mouse::Pressed, 640, 416.7, "left", 1);
        check("a press", press == R"({"id":7,"method":"Input.dispatchMouseEvent","params":{"type":"mousePressed","x":640.0,"y":416.7,"button":"left","buttons":1,"clickCount":1}})");
        const std::string wheel = MouseEvent(8, Mouse::Wheel, 10, 20, "none", 0, -200);
        check("a wheel", wheel.find(R"("type":"mouseWheel")") != std::string::npos && wheel.find(R"("deltaY":-200)") != std::string::npos &&
                             wheel.find("clickCount") == std::string::npos);
        check("a move", MouseEvent(9, Mouse::Moved, 1, 2, "none", 0).find(R"("type":"mouseMoved","x":1.0,"y":2.0,"button":"none","buttons":0})") != std::string::npos);
    }
    return failed ? 1 : 0;
}
