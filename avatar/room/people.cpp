// Who is in the room with you (people.h).
#include "people.h"

#include "../../together/local/datagrams.h"
#include "../../together/net/json.h"

#include <cmath>

namespace roompeople {

namespace {

// A whole number from `lo` to `hi` out of a JSON number. False for anything else.
bool Whole(const json::Value &v, int lo, int hi, int &out) {
    if (v.type != json::Value::kNumber || !(v.number >= lo && v.number <= hi) || v.number != std::floor(v.number)) return false;
    return out = int(v.number), true;
}

}  // namespace

void People::Gone() {
    inRoom_ = false, me_ = -1, myAway_ = 0;
    for (Person &p : people_) {
        const unsigned since = p.since + (p.here ? 1 : 0);
        p = Person();
        p.since = since;
    }
}

bool People::Room(const std::string &text) {
    json::Value state;
    if (!json::Parse(text, state) || state.type != json::Value::kObject || state["people"].type != json::Value::kArray) return Gone(), false;
    const std::string &mode = state["mode"].text();
    int me = -1;
    if ((mode != "hosting" && mode != "joined") || !Whole(state["me"], 0, proto::kPeople - 1, me)) return Gone(), mode == "idle" || mode == "calling";
    bool here[proto::kPeople] = {};
    for (const json::Value &v : state["people"].items) {
        int id = -1, away = 0;
        const json::Value &spot = v["spot"];
        if (!Whole(v["id"], 0, proto::kPeople - 1, id) || here[id] || !Whole(v["away"], 0, 255, away) || spot.type != json::Value::kArray || spot.items.size() != 4)
            return Gone(), false;
        const double at[3] = {spot.items[0].number, spot.items[2].number, spot.items[3].number};
        for (const json::Value &part : spot.items)
            if (part.type != json::Value::kNumber || !std::isfinite(part.number) || std::fabs(part.number) > 2 * proto::kFar) return Gone(), false;
        Person &p = people_[id];
        const std::string &name = v["name"].text(), &login = v["login"].text();
        if (!p.here || p.name != name || p.login != login) {   // someone new in this place
            const unsigned since = p.since + 1;
            p = Person();
            p.since = since, p.here = true, p.name = name, p.login = login;
        }
        p.away = away;
        for (int k = 0; k < 3; ++k) p.spot[k] = at[k];
        here[id] = true;
    }
    if (!here[me]) return Gone(), false;
    for (int id = 0; id < proto::kPeople; ++id)
        if (!here[id] && people_[id].here) {
            const unsigned since = people_[id].since;
            people_[id] = Person();
            people_[id].since = since;
        }
    inRoom_ = true, me_ = me;
    if (!Whole(state["away"], 0, 255, myAway_)) myAway_ = people_[me].away;   // an ft-together that doesn't say
    return true;
}

bool People::Looks(const std::string &text) {
    std::string file[proto::kPeople];
    json::Value looks;
    bool ok = inRoom_ && json::Parse(text, looks) && looks.type == json::Value::kObject && looks["people"].type == json::Value::kArray;
    for (size_t i = 0; ok && i < looks["people"].items.size(); ++i) {
        const json::Value &v = looks["people"].items[i], &at = v["file"];
        int id = -1;
        ok = Whole(v["id"], 0, proto::kPeople - 1, id) && at.type == json::Value::kString && (at.string.empty() || (at.string[0] == '/' && at.string.size() <= kMostPath));
        if (ok) file[id] = at.string;
    }
    for (int id = 0; id < proto::kPeople; ++id) people_[id].avatar = ok && people_[id].here && id != me_ ? file[id] : "";
    return ok;
}

bool People::Pose(const uint8_t *data, size_t n, double now) {
    if (!inRoom_ || n < 3 || data[0] != local::kPoseToWatcher || data[1] >= proto::kPeople) return false;
    Person &p = people_[data[1]];
    proto::Pose pose;
    if (!p.here || int(data[1]) == me_ || !proto::Read(data + 2, n - 2, pose)) return false;
    p.pose = pose, p.posed = true, p.posedAt = now;
    return true;
}

}  // namespace roompeople
