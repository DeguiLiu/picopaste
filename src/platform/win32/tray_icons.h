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
 * @file tray_icons.h
 * @brief Resource ids shared by picopaste.rc and tray.cpp.
 *
 * Resource ids shared by picopaste.rc (which defines the icons) and tray.cpp
 * (which loads them). Kept in one place so the two cannot drift.
 *
 * The lowest id must stay the executable's own icon: the shell takes the
 * lowest-numbered ICON resource as the file's icon, so PICOPASTE_ICON_OK is
 * also what Explorer, the taskbar and Alt-Tab show.
 */
#pragma once

#define PICOPASTE_ICON_OK 100    // healthy: blue clipboard
#define PICOPASTE_ICON_WARN 101  // degraded / retrying: amber
#define PICOPASTE_ICON_ERR 102   // failed / stopped: red
