// When ft-hands parks (see "Parked" at the top of main.cpp), from the files that ask for it. Read every 250 ms.
//   park       run_dir/park exists: the input relay, while Frametop is paused for a VR game
//   testbench  $XDG_RUNTIME_DIR/frametop-testbench/hands is fresh: the testbench publishes fake hands
//   held       run_dir/held is fresh: another producer publishes the hands (include/fh_hands.md, "Producers")
//   needed     run_dir/needed is fresh: a reader needs live hands and the cameras' frames (the hand recorder's
//              session touches it every second), so the relay's park doesn't hold; the testbench and another
//              producer still do, since the hands file has hands then
//   recording  this ft-hands records: it never parks
// "Fresh" is touched within the last 3 s (file_fresh), so a reader or producer that died lets go by itself.
#pragma once

#include <string>

struct ParkInputs {
    bool park = false, testbench = false, held = false, needed = false, recording = false;
};

enum class ParkWhy { none, testbench, other, park };

struct ParkState {
    bool parked = false;
    bool lent = false;   // parked for another producer, which reads the ring: hands-want is left alone
    ParkWhy why = ParkWhy::none;
};

ParkState park_decide(const ParkInputs &in);

// PATH's modification time is within 3 s of now (either way: a clock step doesn't hold it for long).
bool file_fresh(const std::string &path);
