// The hand models ft-handview can draw, by number: the ones compiled in (handasset.h, in its
// order, so 0 is "asset"), then a person's own, read from their avatars folder as Frametop model
// files (avatar/modelfile.h: what ft-model-import writes). A model's name is HANDS_VIEW_MODEL's
// value: the file's name without ".ftmodel".
//
// A model keeps its number while the program runs: a file read again replaces its model in place
// and moves its generation on (the GPU's copy is then out of date), and a file that's gone leaves
// its model as it was last read. A changed file the format refuses leaves it too.
//
// One thread only. No GPU or SteamVR calls here (tests use it).
#pragma once

#include <string>

#include "handasset.h"

namespace handmodels {

int Count();
// A number that isn't a model's stands for model 0 in these three.
const char *Name(int model);
const handasset::Hand &Hand(int model, bool right);
unsigned Generation(int model);   // 0 for a bundled model; a person's starts at 1
int Find(const char *name);       // -1 if there's none

// Whether a person's model may have the name: one a model file may (modelfile::NameOk) that the
// program doesn't use already (a bundled model's, robot, capsules).
constexpr int kMaxName = 31;
bool NameOk(const std::string &name);

// The person's folder (modelfile::Folder).
std::string Folder();

// Reads the folder's *.ftmodel files that are new or changed since the last scan. A file the
// format refuses, one without both hands or one with a name that isn't allowed is left out, and
// `log` says why. Returns how many models were added or replaced.
int Scan(const std::string &folder, std::string &log);

}  // namespace handmodels
