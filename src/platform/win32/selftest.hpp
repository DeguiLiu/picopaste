/**
 * MIT License
 *
 * Copyright (c) 2026 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file selftest.hpp
 * @brief Per-capability self-check table.
 *
 * This is how the user verifies the build on their own machine. Every
 * capability prints one PASS / FAIL / SKIP line with a concrete number (byte
 * counts, enforced memory limits, resolved remote path), never a bare "ok".
 */
#pragma once

#include "picopaste/config.hpp"
#include "picopaste/error.hpp"

#include <cstdint>

namespace picopaste::win32 {

struct SelfTestOptions {
  // Parsed configuration. Required.
  const Config* config = nullptr;
  // Reserved for the end-to-end upload check (M5); currently reports SKIP.
  bool e2e = false;
};

/**
 * @brief Run the table to stdout.
 * @return kOk only when no mandatory capability failed.
 */
Status RunSelfTest(const SelfTestOptions& options) noexcept;

}  // namespace picopaste::win32
