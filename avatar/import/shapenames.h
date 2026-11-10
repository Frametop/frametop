// Which of Frametop's face shapes (bodyfile.h's Shape::Id: the five mouth shapes and the
// blinks) a model's own shape is, by its name, for a file that doesn't say (a VRM does). The
// names are the ones avatars are made with: VRChat's visemes (vrc.v_aa), VRoid's (Fcl_MTH_A,
// Fcl_EYE_Close_L), plain letters, and the Japanese ones MMD models carry.
#pragma once

#include <cstdint>
#include <string>

namespace importer {

// The shape's id, or 0 for a name that isn't one of them.
uint32_t ShapeOfName(const std::string &name);

}  // namespace importer
