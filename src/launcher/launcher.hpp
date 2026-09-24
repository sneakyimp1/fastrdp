#pragma once

namespace fastrdp {

// Runs the Qt connection manager. Sessions are started as separate fastrdp processes.
int runLauncher(int argc, char** argv);

} // namespace fastrdp
