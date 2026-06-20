/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef ANGLECONTEXT_HPP
#define ANGLECONTEXT_HPP

#include <cstdint>

class QWindow;
class QThread;

class AngleContext
{
public:
    AngleContext() = default;
    ~AngleContext();

    bool create(QWindow* window, int swapInterval);
    void destroy();

    bool isValid() const;
    bool makeCurrent(QWindow* window);
    void doneCurrent();
    void swapBuffers(QWindow* window);
    void* getProcAddress(const char* name) const;
    std::uint32_t defaultFramebufferObject() const;

    void moveToThread(QThread* thread);

private:
    void* display = nullptr;
    void* surface = nullptr;
    void* context = nullptr;
    int swapInterval = 0;
};

#endif // ANGLECONTEXT_HPP
