#pragma once
#include "core/Scanner.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <vector>

namespace minisys {

// v2.5: dev build-artifact caches — VS / C++ / CMake / make outputs
// (bin, obj, .vs, ipch, x64, Debug, Release, build, node_modules, ...)
// found next to a project marker (*.sln, *.vcxproj, CMakeLists.txt, ...).
//
// Safety model: everything is quarantined (reversible from 操作历史), and an
// artifact directory touched within the last 24 h is SKIPPED so an
// in-progress build is never moved out from under the compiler.
namespace DevBuildCache {

// Name classification (pure, unit-tested).
bool IsSafeArtifactName(const std::wstring& name);    // bin/obj/.vs/ipch/x64/...
bool IsCautiousArtifactName(const std::wstring& name); // build/out/target/node_modules/...
bool IsProjectMarkerName(const std::wstring& fileName); // *.sln/*.vcxproj/CMakeLists.txt/...

// Roots searched for project trees (user profile with the noisy subtrees
// pruned, plus conventional code roots when they exist).
std::vector<std::filesystem::path> SearchRoots();

// Walk the roots, size the artifact directories and emit ScanItems
// (category "Dev Build"). Progress/cancel semantics match Scanner::Scan.
void Scan(std::vector<ScanItem>& out,
          const std::function<void(unsigned long long, unsigned long long,
                                   const std::wstring&)>& progress,
          const std::atomic<bool>& cancel);

} // namespace DevBuildCache
} // namespace minisys
