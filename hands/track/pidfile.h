// --pidfile PATH: ft-hands writes its process id there, so the unit that started it stops this one
// ft-hands and no other (hands/ft-hands-stop). From the container its pid is the host's too (the dev
// container shares the host's process ids), and a signal to scripts/in-box doesn't reach it.
#pragma once

#include <string>

// Writes the pid to PATH through a temporary file and a rename, refusing a link at PATH. False on failure.
bool pidfile_write(const std::string &path);
// Removes PATH if it still holds this process's pid (a newer ft-hands may have written it since).
void pidfile_remove(const std::string &path);
