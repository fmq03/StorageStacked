#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

// Keep test artifacts in the caller's scratch directory, including fixed-name
// diagnostics that are deliberately retained after a failed assertion.
inline std::string test_temp_path(const std::string& name) {
  const char* selected = std::getenv("TMPDIR");
  const auto root = selected && *selected ? std::filesystem::path(selected)
                                        : std::filesystem::current_path() / "build/test-tmp";
  std::filesystem::create_directories(root);
  return (root / name).string();
}
