#pragma once
#include "core/Scanner.h"

namespace minisys {

class JunkScanner : public Scanner {
public:
    void Scan(std::vector<ScanItem>& out,
              ProgressFn progress,
              const std::atomic<bool>& cancel) override;
};

} // namespace minisys
