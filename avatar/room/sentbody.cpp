// The body of an avatar someone in the room sent (sentbody.h).
#include "sentbody.h"

namespace sentbody {

bool Read(const std::string &file, bodyfile::Body &body, std::string &why) {
    modelfile::Model model;
    bool has = false;
    if (!bodyfile::ReadFile(file, model, body, has, why)) return false;
    if (!has) return why = "it has hands and no body", false;
    if (!model.info.mayShare) return why = "it isn't marked as one its owner lets others have", false;
    return true;
}

}  // namespace sentbody
