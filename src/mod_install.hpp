// Installs mods dropped into mods/ as .zip files: each archive is unpacked
// into its own folder (a single top-level folder is kept as the mod name) and
// then moved to mods/.installed/. Entries that would escape the folder are
// skipped.
#pragma once

#include <string>

namespace gh {

int installModZips(const std::string& modsDir, std::string& report);

}  // namespace gh
