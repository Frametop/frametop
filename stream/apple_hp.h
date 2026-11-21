// Clean-room Apple macOS High Performance Screen Sharing client core (MIT).
// Speaks screensharingd's RFB 003.889 + RSA1/SRP-6a + enc1103 control channel (TCP 5900)
// and AVCMediaStreamNegotiator + RFC 3711 SRTP H.264 4:2:0 media stream (UDP 5900/5901).
//
// The client is split by job; this header includes them all.
#pragma once

#include "apple_hp_session.h"
