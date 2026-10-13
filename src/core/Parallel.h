#pragma once

#include <functional>

// Runs fn(begin, end) over [0, n) in chunks on a shared worker pool. The
// calling thread works too, so this never deadlocks even when the pool is
// busy with other callers.
void parallelFor(int n, const std::function<void(int begin, int end)>& fn, int minChunk = 8);
