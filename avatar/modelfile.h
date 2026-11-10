// Frametop's own model file (.ftmodel): a person's look, as ft-model-import writes it from a
// VRM, glTF or FBX file (docs/together.md). It is the only model format the programs that draw
// read, and the only one a peer may send, so this reader is the one place a stranger's model is
// parsed. It trusts nothing: every count, size, index and number is checked before use, and a
// file that fails any check is refused whole.
//
// A file holds hands, each a skinned mesh on WebXR's 25 hand joints in the layout ft-handview
// draws (screens/handasset.h), and for an avatar its body, in chunks of its own (bodyfile.h).
// This reader takes the hands and skips the rest.
//
// The file, little-endian: "FTMD", a version (u32, 1), then chunks to the end: a four-letter tag,
// its payload's size (u32), the payload, and zeros to the next multiple of four.
//   INFO  entries of: a key (u8), a length (u16), that many bytes (UTF-8 text unless said).
//         1 name, 2 licence, 3 author, 4 source format, 5 may be sent to peers (one byte, 0 or 1).
//   HAND  side (u8: 0 left, 1 right), three zeros, vertices (u32), triangles (u32), the 25 joints'
//         bind positions (75 f32, metres), then the vertices' positions (i16 x 3, kPosStep each),
//         normals (i8 x 3, out of 127), joints (u8 x 4) and weights (u8 x 4, adding up to 255),
//         and the triangles (u16 x 3); each array padded with zeros to a multiple of four.
// A reader skips chunks it doesn't know.
//
// A person's models are files in one folder (Folder), each named NAME.ftmodel: the name is what
// settings and commands call the model by (NameOk).
//
// No GPU, SteamVR or network calls here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace modelfile {

constexpr int kHandJoints = 25;        // WebXR's hand joints, in its order
constexpr float kPosStep = 1e-05f;     // metres per unit of a hand's positions
constexpr int kMaxHandVerts = 20000;   // a hand's limits: the bundled hands have about 1500
constexpr int kMaxHandTris = 40000;    // vertices and 3000 triangles
constexpr size_t kMaxFileBytes = 64u << 20;
constexpr size_t kMaxText = 96;        // bytes in a name, licence, author or format

struct Hand {
    std::vector<int16_t> pos;       // vertices x 3
    std::vector<int8_t> nrm;        // vertices x 3
    std::vector<uint8_t> joints;    // vertices x 4
    std::vector<uint8_t> weights;   // vertices x 4
    std::vector<uint16_t> idx;      // triangles x 3
    float bind[kHandJoints][3] = {};
    int verts() const { return int(pos.size() / 3); }
    int tris() const { return int(idx.size() / 3); }
};

struct Info {
    std::string name, licence, author, format;
    bool mayShare = false;   // its owner lets peers have the file
};

struct Model {
    Info info;
    bool hasHand[2] = {false, false};   // left, right
    Hand hands[2];
};

// Whether a hand keeps every rule of the format (what Read checks of a file's): `why` says
// which one it broke.
bool Valid(const Hand &h, std::string &why);

// False with `err` when the bytes aren't a model this reader takes. A model may have no hands
// (hasHand): an avatar whose hands couldn't be cut out.
bool Read(const uint8_t *data, size_t size, Model &out, std::string &err);
bool ReadFile(const std::string &path, Model &out, std::string &err);

// The file's chunks, for the readers of what isn't here (bodyfile.h): the header and every
// chunk's bounds are checked, a payload isn't. They point into `data`.
struct Chunk {
    uint32_t tag;
    const uint8_t *data;
    size_t size;
};
bool Chunks(const uint8_t *data, size_t size, std::vector<Chunk> &out, std::string &err);
// A file's bytes, kMaxFileBytes at most.
bool ReadBytes(const std::string &path, std::vector<uint8_t> &data, std::string &err);

// Whether a model's file may have the name: lower-case letters, digits, "-" and "_", 1 to
// kMaxName of them.
constexpr int kMaxName = 31;
bool NameOk(const std::string &name);
// A name that is allowed, made from any text (a file's name): its letters and digits, lower
// case, the rest as "-". "model" when nothing is left.
std::string NameFrom(const std::string &text);
// The person's folder: $FT_AVATARS_DIR, or frametop/avatars in $XDG_DATA_HOME (~/.local/share).
std::string Folder();

// Text as a file may carry it: without control characters, kMaxText bytes at most (cut between
// characters). Write does this to a model's name, licence, author and format.
std::string Plain(const std::string &text);

// The file's bytes. The model must be valid (Valid on each hand it has). `more`: chunks written
// after the hands, as they are (bodyfile.h makes an avatar's).
struct Extra {
    char tag[4];
    std::vector<uint8_t> bytes;
};
std::vector<uint8_t> Write(const Model &m, const std::vector<Extra> &more = {});
// Written beside `path` and renamed over it, so a reader never sees half a file.
bool WriteFile(const std::string &path, const Model &m, std::string &err, const std::vector<Extra> &more = {});

}  // namespace modelfile
