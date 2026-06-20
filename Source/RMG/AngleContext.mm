/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "AngleContext.hpp"

#include <QThread>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#endif

static CAMetalLayer* metalLayerForWindow(QWindow* window)
{
    if (window == nullptr || window->winId() == 0)
    {
        return nil;
    }

    NSView* view = reinterpret_cast<NSView*>(window->winId());
    [view setWantsLayer:YES];

    CALayer* layer = [view layer];
    if (layer == nil || ![layer isKindOfClass:[CAMetalLayer class]])
    {
        [view setLayer:[CAMetalLayer layer]];
        layer = [view layer];
    }

    return [layer isKindOfClass:[CAMetalLayer class]] ? static_cast<CAMetalLayer*>(layer) : nil;
}

static void runOnMainThread(void (^block)(void))
{
    if ([NSThread isMainThread])
    {
        block();
        return;
    }

    dispatch_sync(dispatch_get_main_queue(), block);
}

AngleContext::~AngleContext()
{
    this->destroy();
}

bool AngleContext::create(QWindow* window, int swapInterval_)
{
    this->destroy();
    this->swapInterval = swapInterval_;

    if (window == nullptr)
    {
        return false;
    }

    __block bool success = false;
    runOnMainThread(^{
        CAMetalLayer* metalLayer = metalLayerForWindow(window);
        if (metalLayer == nil)
        {
            return;
        }

        const EGLAttrib displayAttributes[] = {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
            EGL_NONE,
        };

        EGLDisplay eglDisplay = eglGetPlatformDisplay(
            EGL_PLATFORM_ANGLE_ANGLE,
            reinterpret_cast<void*>(static_cast<intptr_t>(EGL_DEFAULT_DISPLAY)),
            displayAttributes);
        if (eglDisplay == EGL_NO_DISPLAY)
        {
            eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        }

        if (eglDisplay == EGL_NO_DISPLAY || !eglInitialize(eglDisplay, nullptr, nullptr))
        {
            return;
        }

        if (!eglBindAPI(EGL_OPENGL_ES_API))
        {
            eglTerminate(eglDisplay);
            return;
        }

        const EGLint configAttributes[] = {
            EGL_BUFFER_SIZE, 32,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_STENCIL_SIZE, 0,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_NONE,
        };

        EGLConfig eglConfig = nullptr;
        EGLint configCount = 0;
        if (!eglChooseConfig(eglDisplay, configAttributes, &eglConfig, 1, &configCount) || configCount < 1)
        {
            eglTerminate(eglDisplay);
            return;
        }

        // ANGLE's Metal backend expects a CAMetalLayer*, not an NSView*.
        EGLSurface eglSurface = eglCreateWindowSurface(eglDisplay, eglConfig, metalLayer, nullptr);
        if (eglSurface == EGL_NO_SURFACE)
        {
            eglTerminate(eglDisplay);
            return;
        }

        EGLContext eglContext = EGL_NO_CONTEXT;
        const EGLint contextAttributes32[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 2,
            EGL_NONE,
        };
        eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttributes32);

        if (eglContext == EGL_NO_CONTEXT)
        {
            const EGLint contextAttributes31[] = {
                EGL_CONTEXT_MAJOR_VERSION, 3,
                EGL_CONTEXT_MINOR_VERSION, 1,
                EGL_NONE,
            };
            eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttributes31);
        }

        if (eglContext == EGL_NO_CONTEXT)
        {
            eglDestroySurface(eglDisplay, eglSurface);
            eglTerminate(eglDisplay);
            return;
        }

        if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext))
        {
            eglDestroyContext(eglDisplay, eglContext);
            eglDestroySurface(eglDisplay, eglSurface);
            eglTerminate(eglDisplay);
            return;
        }

        eglSwapInterval(eglDisplay, this->swapInterval);

        this->display = eglDisplay;
        this->surface = eglSurface;
        this->context = eglContext;
        success = true;
    });

    return success;
}

void AngleContext::destroy()
{
    EGLDisplay eglDisplay = static_cast<EGLDisplay>(this->display);
    EGLSurface eglSurface = static_cast<EGLSurface>(this->surface);
    EGLContext eglContext = static_cast<EGLContext>(this->context);

    if (eglDisplay == EGL_NO_DISPLAY)
    {
        return;
    }

    runOnMainThread(^{
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (eglContext != EGL_NO_CONTEXT)
        {
            eglDestroyContext(eglDisplay, eglContext);
        }

        if (eglSurface != EGL_NO_SURFACE)
        {
            eglDestroySurface(eglDisplay, eglSurface);
        }

        eglTerminate(eglDisplay);
    });

    this->display = nullptr;
    this->surface = nullptr;
    this->context = nullptr;
}

bool AngleContext::isValid() const
{
    return this->display != nullptr && this->surface != nullptr && this->context != nullptr;
}

bool AngleContext::makeCurrent(QWindow* window)
{
    Q_UNUSED(window);

    EGLDisplay eglDisplay = static_cast<EGLDisplay>(this->display);
    EGLSurface eglSurface = static_cast<EGLSurface>(this->surface);
    EGLContext eglContext = static_cast<EGLContext>(this->context);

    if (!this->isValid())
    {
        return false;
    }

    return eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext) == EGL_TRUE;
}

void AngleContext::doneCurrent()
{
    EGLDisplay eglDisplay = static_cast<EGLDisplay>(this->display);
    if (eglDisplay != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}

void AngleContext::swapBuffers(QWindow* window)
{
    Q_UNUSED(window);

    EGLDisplay eglDisplay = static_cast<EGLDisplay>(this->display);
    EGLSurface eglSurface = static_cast<EGLSurface>(this->surface);

    if (!this->isValid())
    {
        return;
    }

    eglSwapBuffers(eglDisplay, eglSurface);
}

void* AngleContext::getProcAddress(const char* name) const
{
    if (name == nullptr)
    {
        return nullptr;
    }

    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

std::uint32_t AngleContext::defaultFramebufferObject() const
{
    return 0;
}

void AngleContext::moveToThread(QThread* thread)
{
    Q_UNUSED(thread);
}
