// What ft-together reads of tailscaled's answers (together/net/tailnet.h, json.h), on answers
// kept here: no tailscaled is asked.
#include "../net/json.h"
#include "../net/lookup.h"
#include "../net/tailnet.h"

#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static uint64_t g_seed = 11;
static uint64_t roll() { return g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull, g_seed >> 11; }

static bool parses(const std::string &text) {
    json::Value v;
    return json::Parse(text, v);
}

static void reader() {
    json::Value v;
    bool ok = json::Parse(" {\"a\": [1, -2.5e1, true, false, null, \"x\"], \"b\": {\"c\": \"d\"}, \"a\": 3} ", v);
    ok = ok && v.type == json::Value::kObject && v.members.size() == 3 && v["a"].items.size() == 6 && v["a"].items[0].number == 1 && v["a"].items[1].number == -25 &&
         v["a"].items[2].boolean && v["a"].items[3].type == json::Value::kBool && !v["a"].items[3].boolean && v["a"].items[4].type == json::Value::kNull &&
         v["a"].items[5].text() == "x" && v["b"]["c"].text() == "d";
    check("a text is read into its values, and a name used twice means the first", ok);
    check("what isn't there is null, however far one asks", v["nope"]["deeper"]["still"].type == json::Value::kNull && v["a"]["x"].text().empty() && v["b"].text().empty());

    ok = json::Parse("\"a\\n\\t\\\"\\\\\\/\\u00e9\\u20ac\\ud83d\\ude00\"", v) && v.text() == "a\n\t\"\\/\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
    check("a string's escapes are read, the pairs too", ok);
    ok = true;
    for (const char *bad : {"", " ", "{", "}", "[1,]", "{\"a\":}", "{\"a\" 1}", "{a:1}", "{\"a\":1,}", "[1 2]", "01", "1.", ".5", "-", "1e", "+1", "tru", "nul", "\"a", "\"\\x\"",
                            "\"\\u12\"", "\"\\ud83d\"", "\"\\ud83dx\"", "\"\\ude00\"", "\"a\nb\"", "1 2", "[1] x", "{\"a\":1}}", "'a'", "NaN"})
        ok = ok && !parses(bad);
    check("what isn't JSON is refused: cut short, with something after it, or nearly right", ok);

    const std::string deep(json::kMostDepth + 2, '['), deepEnd(json::kMostDepth + 2, ']');
    const std::string fine(json::kMostDepth, '['), fineEnd(json::kMostDepth, ']');
    check("nesting has a limit", !parses(deep + deepEnd) && parses(fine + fineEnd));
    std::string many = "[";
    for (size_t i = 0; i < json::kMostValues; ++i) many += "0,";
    check("...and so has the number of values, and the size", !parses(many + "0]") && !parses("\"" + std::string(json::kMostText, 'a') + "\""));

    // noise, and a good text with one byte changed: read or refused, never more
    const std::string good = "{\"Node\":{\"Name\":\"a.b.ts.net.\",\"ComputedName\":\"a\"},\"UserProfile\":{\"LoginName\":\"x@y\",\"ID\":12}}";
    for (int i = 0; i < 4000; ++i) {
        std::string text = good;
        if (i % 2) {
            text.resize(roll() % 64);
            for (char &c : text) c = "{}[]\",:\\u0123456789truefalsn. -e"[roll() % 33];
        } else {
            text[roll() % text.size()] = char(roll());
        }
        session::Who who;
        parses(text), net::ReadWho(text, who);
    }
    check("noise is read or refused", true);
}

static void addresses() {
    bool ok = net::OnTailnet("100.64.0.0") && net::OnTailnet("100.101.102.103") && net::OnTailnet("100.127.255.255") && net::OnTailnet("fd7a:115c:a1e0::ab12:3456") &&
              net::OnTailnet("fd7a:115c:a1e0:ab12:4843:cd96:6258:b240");
    check("tailscale's addresses are on the tailnet", ok);
    ok = true;
    for (const char *other : {"100.63.255.255", "100.128.0.0", "101.64.0.1", "10.0.0.1", "192.168.1.5", "127.0.0.1", "8.8.8.8", "::1", "fd7a:115c:a1e1::1", "fe80::1", "2001:db8::1",
                              "::ffff:100.64.0.1", "", "100.64.0", "100.64.0.1 ", " 100.64.0.1", "100.64.0.1/10", "100.64.0.1:47193", "steam-frame", "steam-frame.tail1234.ts.net",
                              "0x64.64.0.1", "100.064.0.1"})
        ok = ok && !net::OnTailnet(other);
    check("...and nothing else is: other networks, names, an address with more around it", ok);
    ok = net::IsLoopback("127.0.0.1") && net::IsLoopback("127.8.9.10") && net::IsLoopback("::1") && !net::IsLoopback("128.0.0.1") && !net::IsLoopback("100.64.0.1") &&
         !net::IsLoopback("::2") && !net::IsLoopback("localhost") && !net::IsLoopback("") && !net::IsLoopback("::ffff:127.0.0.1");
    check("loopback is 127.x and ::1, as numbers", ok);
}

static void answers() {
    int status = 0;
    std::string body;
    bool ok = net::HttpBody("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}", status, body) && status == 200 && body == "{}";
    ok = ok && net::HttpBody("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n\r\nno match for IP:port\n", status, body) && status == 404 && body == "no match for IP:port\n";
    ok = ok && net::HttpBody("HTTP/1.0 200 OK\r\n\r\n", status, body) && status == 200 && body.empty();
    check("an answer's status and body are found", ok);
    ok = true;
    for (const char *bad : {"", "HTTP/1.0 200", "HTTP/1.0 200 OK\r\n", "HTTP/1.0 200 OK\r\nA: b\r\n", "HTTP/2 200 OK\r\n\r\n", "HTTP/1.0 2x0 OK\r\n\r\n", "http/1.0 200 OK\r\n\r\n",
                            "HTTP/1.0  200 OK\r\n\r\n", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nA: b\r\ntransfer-encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n"})
        ok = ok && !net::HttpBody(bad, status, body);
    check("...and one cut short, or in chunks, is refused", ok);

    // tailscaled 1.102's `status?peers=false`, with what isn't read left out
    const std::string status1 =
        "{\"Version\":\"1.102.4\",\"TUN\":true,\"BackendState\":\"Running\",\"HaveNodeKey\":true,\"AuthURL\":\"\","
        "\"TailscaleIPs\":[\"100.101.102.103\",\"fd7a:115c:a1e0::ab12:3456\"],"
        "\"Self\":{\"ID\":\"nABC\",\"PublicKey\":\"nodekey:00\",\"HostName\":\"steam-frame\",\"DNSName\":\"steam-frame.tail1234.ts.net.\",\"OS\":\"linux\",\"UserID\":77,"
        "\"TailscaleIPs\":[\"fd7a:115c:a1e0::ab12:3456\",\"100.101.102.103\"],\"Online\":true},\"Health\":[],\"MagicDNSSuffix\":\"tail1234.ts.net\",\"Peer\":null,\"User\":null}";
    std::string address, name;
    ok = net::ReadSelf(status1, address, name) && address == "100.101.102.103" && name == "steam-frame";
    check("status: this node's IPv4 address on the tailnet, and its name", ok);
    ok = net::ReadSelf("{\"BackendState\":\"Running\",\"Self\":{\"DNSName\":\"frame-two.tail1234.ts.net.\",\"TailscaleIPs\":[\"100.64.0.9\"]}}", address, name) && address == "100.64.0.9" &&
         name == "frame-two";
    check("...the name from its DNS name when it has no other", ok);
    ok = true;
    for (const char *down : {"{\"BackendState\":\"Stopped\",\"Self\":{\"HostName\":\"a\",\"TailscaleIPs\":[\"100.64.0.9\"]}}",
                             "{\"BackendState\":\"NeedsLogin\",\"Self\":{\"HostName\":\"a\",\"TailscaleIPs\":null}}", "{\"BackendState\":\"Running\",\"Self\":{\"HostName\":\"a\"}}",
                             "{\"BackendState\":\"Running\",\"Self\":{\"HostName\":\"a\",\"TailscaleIPs\":[\"fd7a:115c:a1e0::1\"]}}",
                             "{\"BackendState\":\"Running\",\"Self\":{\"HostName\":\"a\",\"TailscaleIPs\":[\"192.168.1.5\", 7, null]}}",
                             "{\"BackendState\":\"Running\",\"Self\":null}", "{\"Self\":{\"TailscaleIPs\":[\"100.64.0.9\"]}}", "[]", "\"Running\"", "", "not json"})
        ok = ok && !net::ReadSelf(down, address, name);
    check("...and none while Tailscale is stopped, logged out, has no IPv4 address, or says something else", ok);

    // `whois?addr=100.101.102.103`
    const std::string whois =
        "{\"Node\":{\"ID\":123,\"StableID\":\"nABC\",\"Name\":\"steam-frame.tail1234.ts.net.\",\"User\":77,\"Addresses\":[\"100.101.102.103/32\",\"fd7a:115c:a1e0::ab12:3456/128\"],"
        "\"Hostinfo\":{\"OS\":\"linux\",\"Hostname\":\"steam-frame\"},\"ComputedName\":\"steam-frame\",\"ComputedNameWithHost\":\"steam-frame\"},"
        "\"UserProfile\":{\"ID\":77,\"LoginName\":\"someone@example.com\",\"DisplayName\":\"Some One\",\"ProfilePicURL\":\"https://example.com/p.png\"},\"CapMap\":{}}";
    session::Who who;
    ok = net::ReadWho(whois, who) && who.login == "someone@example.com" && who.name == "steam-frame";
    check("whois: the login of the node's owner, and the node's name", ok);
    ok = net::ReadWho("{\"Node\":{\"Name\":\"pc.tail1234.ts.net.\"},\"UserProfile\":{\"LoginName\":\"a@b\"}}", who) && who.login == "a@b" && who.name == "pc";
    check("...the name from its DNS name when it has no other", ok);
    ok = true;
    for (const char *nobody : {"{\"Node\":{\"Name\":\"a.b.\"},\"UserProfile\":{\"LoginName\":\"\"}}", "{\"Node\":{\"Name\":\"a.b.\"},\"UserProfile\":{\"LoginName\":7}}",
                               "{\"Node\":{\"Name\":\"a.b.\"}}", "{\"Node\":null,\"UserProfile\":{\"LoginName\":\"a@b\"}}", "{\"UserProfile\":{\"LoginName\":\"a@b\"}}",
                               "{\"Node\":\"a\",\"UserProfile\":{\"LoginName\":\"a@b\"}}", "no match for IP:port\n", "", "{}"})
        ok = ok && !net::ReadWho(nobody, who);
    check("...and nobody without a login or a node", ok);
}

static void names() {
    const auto any = [](const std::string &) { return true; };
    bool ok = net::Lookup("127.0.0.1", any) == "127.0.0.1" && net::Lookup("::1", any) == "::1" && net::Lookup("100.64.0.9", net::OnTailnet) == "100.64.0.9" &&
              net::Lookup("FD7A:115C:A1E0:0:0:0:AB12:3456", net::OnTailnet) == "fd7a:115c:a1e0::ab12:3456";
    check("an address is itself, written the one way", ok);
    std::string seen;
    const std::string local = net::Lookup("localhost", [&](const std::string &a) { return seen += a + " ", true; });
    check("a name is looked up, and its IPv4 address comes first", local == "127.0.0.1" && seen == "127.0.0.1 ");
    ok = net::Lookup("localhost", net::OnTailnet).empty() && net::Lookup("192.168.1.5", net::OnTailnet).empty() && net::Lookup("127.0.0.1", nullptr).empty() &&
         net::Lookup("", any).empty() && net::Lookup("no such host.invalid", any).empty() && net::Lookup("100.64.0.9:47193", any).empty();
    check("...and there is no address for one that isn't allowed, or for what isn't a host", ok);
}

static void pretend() {
    net::Loopback net("ada");
    session::Who who;
    std::string address;
    bool ok = net.Self(who, address) && who.login == "ada@loopback" && who.name == "ada" && address == "127.0.0.1";
    ok = ok && net.WhoIs("127.0.0.1", who) && who.login == "someone@loopback" && !net.WhoIs("100.64.0.1", who) && !net.WhoIs("192.168.1.5", who);
    ok = ok && net.MayCall("127.0.0.1") && net.MayCall("::1") && !net.MayCall("100.64.0.1") && !net.MayCall("example.com");
    check("the test's tailnet is loopback, and only that", ok);

    net::Tailscale none("/nonexistent/tailscaled.sock");
    ok = !none.Self(who, address) && !none.WhoIs("100.64.0.1", who) && none.MayCall("100.64.0.1") && !none.MayCall("127.0.0.1") && !none.MayCall("192.168.1.5");
    check("without tailscaled nobody is anybody, and only the tailnet may be called", ok);
}

int main() {
    reader();
    addresses();
    answers();
    names();
    pretend();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
