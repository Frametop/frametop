// See shapenames.h.
#include "shapenames.h"

#include <algorithm>

#include "../bodyfile.h"
#include "handbones.h"

namespace importer {
namespace {

using Shape = bodyfile::Shape;

bool Has(const std::vector<std::string> &words, std::initializer_list<const char *> any) {
    for (const std::string &w : words)
        for (const char *a : any)
            if (w == a) return true;
    return false;
}

}  // namespace

uint32_t ShapeOfName(const std::string &name) {
    static const struct {
        const char *name;
        uint32_t id;
    } kExact[] = {
        {"\xe3\x81\x82", Shape::kAa}, {"\xe3\x81\x84", Shape::kIh}, {"\xe3\x81\x86", Shape::kOu}, {"\xe3\x81\x88", Shape::kEe},
        {"\xe3\x81\x8a", Shape::kOh}, {"\xe3\x81\xbe\xe3\x81\xb0\xe3\x81\x9f\xe3\x81\x8d", Shape::kBlink},
        {"\xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3\xe3\x82\xaf", Shape::kBlinkLeft},
        {"\xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3\xe3\x82\xaf\xe5\x8f\xb3", Shape::kBlinkRight},
    };
    for (const auto &e : kExact)
        if (name == e.name) return e.id;
    const std::vector<std::string> words = Words(name);
    if (words.empty()) return 0;
    if (Has(words, {"blink"}) || (Has(words, {"eye", "eyes"}) && Has(words, {"close", "closed"}))) {
        if (Has(words, {"l", "left"})) return Shape::kBlinkLeft;
        if (Has(words, {"r", "right"})) return Shape::kBlinkRight;
        return Shape::kBlink;
    }
    // a mouth shape: the name's last word, alone or after a word that says it is a mouth's
    const bool mouth = words.size() == 1 || Has(words, {"v", "viseme", "vrc", "mth", "mouth", "vowel"});
    if (!mouth || words.size() > 4) return 0;
    const std::string &last = words.back();
    if (last == "aa" || last == "a") return Shape::kAa;
    if (last == "ih" || last == "i") return Shape::kIh;
    if (last == "ou" || last == "u") return Shape::kOu;
    if (last == "ee" || last == "e") return Shape::kEe;
    if (last == "oh" || last == "o") return Shape::kOh;
    return 0;
}

}  // namespace importer
