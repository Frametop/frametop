// The mark on a model file that says its owner lets the people they share a room with have a
// copy of it (modelfile.h's Info::mayShare; docs/together.md). A VRM carries a licence, and the
// importer set the mark from it. A glTF or an FBX can't say, so its person does: when it is
// imported (ft-model-import --may-share), or later, here (ft-model-import --allow-share).
#pragma once

#include "modelfile.h"

#include <string>

namespace sharemark {

// Marks the model file at `path` as one that may be passed on, and gives its facts as they are
// then. False with `err`, and the file as it was, when it isn't a model file read whole by the
// strict readers, has neither hands nor a body, or came from a VRM without the mark: its licence decided, and nobody's
// say-so overrides it. Everything in the file but the mark stays byte for byte.
bool Allow(const std::string &path, modelfile::Info &info, std::string &err);

}  // namespace sharemark
