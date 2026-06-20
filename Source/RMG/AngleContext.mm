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

#include <QGuiApplication>
#include <QThread>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <sstream>

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#endif

static void runOnMainThread(void (^block)(void))
{
    if ([NSThread isMainThread])
    {
        block();
        return;
    }

    dispatch_sync(dispatch_get_main_queue(), block);
}

static void configureMetalLayer(CAMetalLayer* metalLayer, QWindow* window)
{
    const qreal devicePixelRatio = window->devicePixelRatio();
    int width  = static_cast<int>(window->width()  * devicePixelRatio);
    int height = static_cast<int>(window->height() * devicePixelRatio);

    if (width < 1 || height < 1)
    {
        NSView* view = reinterpret_cast<NSView*>(window->winId());
        if (view != nil)
        {
            const NSRect bounds = [view bounds];
            width  = static_cast<int>(bounds.size.width  * devicePixelRatio);
            height = static_cast<int>(bounds.size.height * devicePixelRatio);
        }
    }

    if (width < 1)
    {
        width = 640;
    }
    if (height < 1)
    {
        height = 480;
    }

    metalLayer.contentsScale   = devicePixelRatio;
    metalLayer.framebufferOnly = YES;
    metalLayer.drawableSize    = CGSizeMake(width, height);
}

static CAMetalLayer* findMetalLayerInLayer(CALayer* layer)
{
    if (layer == nil)
    {
        return nil;
    }

    if ([layer isKindOfClass:[CAMetalLayer class]])
    {
        return static_cast<CAMetalLayer*>(layer);
    }

    for (CALayer* sublayer in [layer sublayers])
    {
        CAMetalLayer* found = findMetalLayerInLayer(sublayer);
        if (found != nil)
        {
            return found;
        }
    }

    return nil;
}

static CAMetalLayer* findMetalLayerInView(NSView* view)
{
    if (view == nil)
    {
        return nil;
    }

    [view setWantsLayer:YES];
    CAMetalLayer* layer = findMetalLayerInLayer([view layer]);
    if (layer != nil)
    {
        return layer;
    }

    for (NSView* subview in [view subviews])
    {
        layer = findMetalLayerInView(subview);
        if (layer != nil)
        {
            return layer;
        }
    }

    return nil;
}

static CAMetalLayer* createMetalLayerForView(NSView* view)
{
    [view setWantsLayer:YES];
    if ([view layer] == nil)
    {
        [view setLayer:[CALayer layer]];
    }

    CAMetalLayer* metalLayer = [CAMetalLayer layer];
    metalLayer.frame = [view bounds];
    metalLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    [[view layer] addSublayer:metalLayer];
    return metalLayer;
}

static CAMetalLayer* metalLayerForWindow(QWindow* window, std::string* errorOut)
{
    if (window == nullptr || window->winId() == 0)
    {
        if (errorOut != nullptr)
        {
            *errorOut = "native window handle is unavailable";
        }
        return nil;
    }

    NSView* view = reinterpret_cast<NSView*>(window->winId());
    CAMetalLayer* metalLayer = findMetalLayerInView(view);
    if (metalLayer == nil)
    {
        metalLayer = createMetalLayerForView(view);
    }

    if (metalLayer == nil)
    {
        if (errorOut != nullptr)
        {
            std::ostringstream message;
            message << "failed to acquire CAMetalLayer (winId=" << window->winId() << ")";
            CALayer* layer = [view layer];
            if (layer != nil)
            {
                message << ", layer=" << [[layer className] UTF8String];
            }
            else
            {
                message << ", layer=null";
            }
            *errorOut = message.str();
        }
        return nil;
    }

    configureMetalLayer(metalLayer, window);
    return metalLayer;
}

static EGLContext createEglContext(EGLDisplay eglDisplay, EGLConfig eglConfig, int majorVersion, int minorVersion)
{
    if (majorVersion < 2)
    {
        majorVersion = 2;
        minorVersion = 0;
    }

    if (majorVersion == 2)
    {
        const EGLint contextAttributes[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE,
        };
        return eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttributes);
    }

    if (majorVersion > 3 || (majorVersion == 3 && minorVersion > 2))
    {
        majorVersion = 3;
        minorVersion = 2;
    }

    const EGLint contextAttributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, majorVersion,
        EGL_CONTEXT_MINOR_VERSION, minorVersion,
        EGL_NONE,
    };
    return eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttributes);
}

static void clearEglErrors()
{
    while (eglGetError() != EGL_SUCCESS)
    {
    }
}

static EGLContext createEglContextWithFallback(EGLDisplay eglDisplay, EGLConfig eglConfig, int majorVersion, int minorVersion)
{
    struct VersionAttempt
    {
        int major;
        int minor;
    };

    VersionAttempt attempts[] = {
        {majorVersion, minorVersion},
        {3, 2},
        {3, 1},
        {3, 0},
        {2, 0},
    };

    for (const VersionAttempt& attempt : attempts)
    {
        clearEglErrors();

        EGLContext eglContext = createEglContext(eglDisplay, eglConfig, attempt.major, attempt.minor);
        if (eglContext != EGL_NO_CONTEXT)
        {
            return eglContext;
        }
    }

    return EGL_NO_CONTEXT;
}

const std::string& AngleContext::lastErrorMessage() const
{
    return this->lastError;
}

AngleContext::~AngleContext()
{
    this->destroy();
}

bool AngleContext::create(QWindow* window, int swapInterval_, int majorVersion, int minorVersion)
{
    this->destroy();
    this->swapInterval = swapInterval_;
    this->lastError.clear();

    if (window == nullptr)
    {
        this->lastError = "QWindow handle is null";
        return false;
    }

    __block bool success = false;
    runOnMainThread(^{
        CAMetalLayer* metalLayer = metalLayerForWindow(window, &this->lastError);
        if (metalLayer == nil)
        {
            if (this->lastError.empty())
            {
                std::ostringstream message;
                message << "failed to acquire CAMetalLayer (winId=" << window->winId() << ")";
                this->lastError = message.str();
            }
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
            std::ostringstream message;
            message << "eglInitialize failed (error=" << eglGetError() << ")";
            this->lastError = message.str();
            return;
        }

        if (!eglBindAPI(EGL_OPENGL_ES_API))
        {
            this->lastError = "eglBindAPI(EGL_OPENGL_ES_API) failed";
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
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_NONE,
        };

        EGLConfig eglConfig = nullptr;
        EGLint configCount = 0;
        if (!eglChooseConfig(eglDisplay, configAttributes, &eglConfig, 1, &configCount) || configCount < 1)
        {
            const EGLint fallbackConfigAttributes[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT,
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_NONE,
            };
            if (!eglChooseConfig(eglDisplay, fallbackConfigAttributes, &eglConfig, 1, &configCount) || configCount < 1)
            {
                std::ostringstream message;
                message << "eglChooseConfig failed (error=" << eglGetError() << ")";
                this->lastError = message.str();
                eglTerminate(eglDisplay);
                return;
            }
        }

        // ANGLE's Metal backend expects a CAMetalLayer*, not an NSView*.
        EGLSurface eglSurface = eglCreateWindowSurface(eglDisplay, eglConfig, metalLayer, nullptr);
        if (eglSurface == EGL_NO_SURFACE)
        {
            std::ostringstream message;
            message << "eglCreateWindowSurface failed (error=" << eglGetError() << ")";
            this->lastError = message.str();
            eglTerminate(eglDisplay);
            return;
        }

        EGLContext eglContext = createEglContextWithFallback(
            eglDisplay, eglConfig, majorVersion, minorVersion);

        if (eglContext == EGL_NO_CONTEXT)
        {
            std::ostringstream message;
            message << "eglCreateContext failed (error=" << eglGetError() << ")";
            this->lastError = message.str();
            eglDestroySurface(eglDisplay, eglSurface);
            eglTerminate(eglDisplay);
            return;
        }

        if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext))
        {
            std::ostringstream message;
            message << "eglMakeCurrent failed (error=" << eglGetError() << ")";
            this->lastError = message.str();
            eglDestroyContext(eglDisplay, eglContext);
            eglDestroySurface(eglDisplay, eglSurface);
            eglTerminate(eglDisplay);
            return;
        }

        eglSwapInterval(eglDisplay, this->swapInterval);

        this->display = eglDisplay;
        this->surface = eglSurface;
        this->context = eglContext;

        // Leave the context unbound so the render thread can make it current.
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
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
