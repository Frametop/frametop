// A model's pictures: PNG and JPEG bytes into pixels (stb_image), and halving them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "scene.h"

namespace importer {

// The picture in the bytes, halved until no side is over `maxSide`. False when the bytes aren't
// a PNG or JPEG, or the picture is larger than 8192 a side.
bool DecodeImage(const uint8_t *data, size_t size, int maxSide, Image &out);

// Whether a file a model names may be read: one beside it, named plainly (not somewhere else on
// the disk, whose bytes would end up in the model).
bool PlainFileName(const char *name);

// The picture in a file beside the model (`folder`), when its name is a plain one.
bool DecodeImageFile(const std::string &folder, const char *name, int maxSide, Image &out);

// Half the size each way (a side of one stays): each pixel the mean of the four it covers, its
// colour by their alpha so a see-through edge doesn't darken.
void Halve(Image &im);

}  // namespace importer
