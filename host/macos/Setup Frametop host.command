#!/bin/bash
# Double-click to run the Frametop host setup in Terminal.
cd "$(dirname "$0")" && ./frametop-host-setup.sh "$@"
read -r -p "Press Enter to close: " _
