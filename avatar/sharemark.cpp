// The mark that lets a model be passed on (sharemark.h).
#include "sharemark.h"

#include "bodyfile.h"
#include "modelbytes.h"

#include <utility>
#include <vector>

namespace sharemark {

bool Allow(const std::string &path, modelfile::Info &info, std::string &err) {
    std::vector<uint8_t> data;
    modelfile::Model model;
    std::vector<modelfile::Chunk> chunks;
    bodyfile::Body body;
    bool hasBody = false;
    if (!modelfile::ReadBytes(path, data, err) || !modelfile::Read(data.data(), data.size(), model, err) || !modelfile::Chunks(data.data(), data.size(), chunks, err) ||
        !bodyfile::Read(chunks, body, hasBody, err))
        return false;
    if (!hasBody && !model.hasHand[0] && !model.hasHand[1]) return err = "it has no hands and no body", false;
    info = model.info;
    if (info.mayShare) return true;
    if (info.format.rfind("vrm", 0) == 0) return err = "it came from a VRM whose licence doesn't let it be passed on", false;
    model.info.mayShare = info.mayShare = true;
    // the hands are written again as they were read; every other chunk goes over as it is
    std::vector<modelfile::Extra> more;
    for (const modelfile::Chunk &c : chunks) {
        if (c.tag == modelbytes::Tag("INFO") || c.tag == modelbytes::Tag("HAND")) continue;
        modelfile::Extra extra;
        for (int i = 0; i < 4; ++i) extra.tag[i] = char(c.tag >> (8 * i));
        extra.bytes.assign(c.data, c.data + c.size);
        more.push_back(std::move(extra));
    }
    return modelfile::WriteFile(path, model, err, more);
}

}  // namespace sharemark
