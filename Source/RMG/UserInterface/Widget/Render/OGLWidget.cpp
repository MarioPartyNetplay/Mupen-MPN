/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "OGLWidget.hpp"
#include "OnScreenDisplay.hpp"

#include <QGuiApplication>
#include <QMetaObject>
#include <QCoreApplication>
#include <QThread>
#include <QMouseEvent>
#include <QCloseEvent>

#include <QEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QSurfaceFormat>

#include <RMG-Core/Video.hpp>
#include <RMG-Core/Callback.hpp>
#include <RMG-Core/Netplay.hpp>

#include <QOpenGLFunctions>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace UserInterface::Widget;

namespace
{

constexpr int kScreenshotBorderThreshold = 4;

bool isBorderPixel(const std::uint8_t* pixel)
{
    return pixel[0] <= kScreenshotBorderThreshold &&
           pixel[1] <= kScreenshotBorderThreshold &&
           pixel[2] <= kScreenshotBorderThreshold;
}

bool cropScreenshotBorders(const std::vector<std::uint8_t>& source, int sourceWidth, int sourceHeight,
                           std::vector<std::uint8_t>& destination, int& destinationWidth, int& destinationHeight)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 || source.empty())
    {
        return false;
    }

    int top = 0;
    int bottom = sourceHeight - 1;
    int left = 0;
    int right = sourceWidth - 1;

    while (top <= bottom)
    {
        bool rowIsBorder = true;
        for (int x = 0; x < sourceWidth; ++x)
        {
            if (!isBorderPixel(&source[(top * sourceWidth + x) * 3]))
            {
                rowIsBorder = false;
                break;
            }
        }

        if (!rowIsBorder)
        {
            break;
        }

        ++top;
    }

    while (bottom >= top)
    {
        bool rowIsBorder = true;
        for (int x = 0; x < sourceWidth; ++x)
        {
            if (!isBorderPixel(&source[(bottom * sourceWidth + x) * 3]))
            {
                rowIsBorder = false;
                break;
            }
        }

        if (!rowIsBorder)
        {
            break;
        }

        --bottom;
    }

    while (left <= right)
    {
        bool columnIsBorder = true;
        for (int y = top; y <= bottom; ++y)
        {
            if (!isBorderPixel(&source[(y * sourceWidth + left) * 3]))
            {
                columnIsBorder = false;
                break;
            }
        }

        if (!columnIsBorder)
        {
            break;
        }

        ++left;
    }

    while (right >= left)
    {
        bool columnIsBorder = true;
        for (int y = top; y <= bottom; ++y)
        {
            if (!isBorderPixel(&source[(y * sourceWidth + right) * 3]))
            {
                columnIsBorder = false;
                break;
            }
        }

        if (!columnIsBorder)
        {
            break;
        }

        --right;
    }

    if (top > bottom || left > right)
    {
        destination = source;
        destinationWidth = sourceWidth;
        destinationHeight = sourceHeight;
        return true;
    }

    destinationWidth = right - left + 1;
    destinationHeight = bottom - top + 1;
    destination.resize(static_cast<size_t>(destinationWidth * destinationHeight * 3));

    for (int y = 0; y < destinationHeight; ++y)
    {
        std::memcpy(&destination[static_cast<size_t>(y * destinationWidth * 3)],
                    &source[static_cast<size_t>(((top + y) * sourceWidth + left) * 3)],
                    static_cast<size_t>(destinationWidth * 3));
    }

    return true;
}

#ifdef __APPLE__
// OpenGL ES (ANGLE on macOS) rejects GL_RGB for glReadPixels. The spec only
// guarantees GL_RGBA, or the implementation's preferred format.
constexpr unsigned int kGlRgb              = 0x1907;
constexpr unsigned int kGlRgba             = 0x1908;
constexpr unsigned int kGlBgra             = 0x80E1;
constexpr unsigned int kGlBgr              = 0x80E0;
constexpr unsigned int kGlUnsignedByte     = 0x1401;
constexpr unsigned int kGlPackAlignment    = 0x0D05;
constexpr unsigned int kGlPackRowLength    = 0x0D02;
constexpr unsigned int kGlPackSkipRows     = 0x0D03;
constexpr unsigned int kGlPackSkipPixels   = 0x0D04;
constexpr unsigned int kGlReadFramebuffer  = 0x8CA8;
constexpr unsigned int kGlColorAttachment0 = 0x8CE0;
constexpr unsigned int kGlBack             = 0x0405;

int readbackBytesPerPixel(unsigned int format)
{
    if (format == kGlRgb || format == kGlBgr)
    {
        return 3;
    }
    if (format == kGlRgba || format == kGlBgra)
    {
        return 4;
    }
    return 0;
}

bool packReadbackToRgb(const std::vector<std::uint8_t>& source, int sourceWidth, int sourceHeight,
                       unsigned int format, std::vector<std::uint8_t>& rgb)
{
    const int bytesPerPixel = readbackBytesPerPixel(format);
    if (bytesPerPixel == 0 || sourceWidth <= 0 || sourceHeight <= 0)
    {
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(sourceWidth) * static_cast<size_t>(sourceHeight);
    if (source.size() < pixelCount * static_cast<size_t>(bytesPerPixel))
    {
        return false;
    }

    rgb.resize(pixelCount * 3u);
    const bool swapRedBlue = format == kGlBgra || format == kGlBgr;
    for (size_t pixel = 0; pixel < pixelCount; ++pixel)
    {
        const std::uint8_t* src = source.data() + pixel * static_cast<size_t>(bytesPerPixel);
        std::uint8_t* dst = rgb.data() + pixel * 3u;
        if (swapRedBlue)
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }
        else
        {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
        }
    }

    return true;
}
#endif // __APPLE__

} // namespace

OGLWidget::OGLWidget(QWidget *parent, bool dedicatedWindow)
    : m_dedicatedWindow(dedicatedWindow)
{
#ifdef __APPLE__
    this->setSurfaceType(QWindow::MetalSurface);
    this->create();
#else
    this->setSurfaceType(QWindow::OpenGLSurface);

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    this->setFormat(format);
    this->openGLcontext = new QOpenGLContext();
    this->openGLcontext->setFormat(format);
#endif

    if (m_dedicatedWindow)
    {
        this->setFlags(Qt::Window);
    }
    else
    {
        // QWindow must be created before embedding it in a container widget.
        this->widgetContainer = QWidget::createWindowContainer(this, parent);
        this->widgetContainer->installEventFilter(this);

        // on wayland we have to make sure that the widget
        // has a black background palette set, else
        // the window will have the theme as background color
        if (QGuiApplication::platformName() == "wayland" ||
            QGuiApplication::platformName() == "cocoa")
        {
            QPalette blackPalette;
            blackPalette.setColor(QPalette::Window, Qt::black);
            this->widgetContainer->setAutoFillBackground(true);
            this->widgetContainer->setPalette(blackPalette);
        }
    }
}

OGLWidget::~OGLWidget(void)
{
#ifndef __APPLE__
    this->openGLcontext->deleteLater();
#endif
}

void OGLWidget::PrepareRenderContext(const QSurfaceFormat& format, QThread* thread)
{
#ifdef __APPLE__
    this->swapInterval = format.swapInterval();
    this->contextMajorVersion = format.majorVersion();
    this->contextMinorVersion = format.minorVersion();
    this->angleContext.moveToThread(thread);
#else
    if (QGuiApplication::platformName() != "wayland")
    {
        this->setFormat(format);
        this->openGLcontext->setFormat(format);
    }
    this->MoveContextToThread(thread);
#endif
}

bool OGLWidget::EnsureRenderContext()
{
#ifdef __APPLE__
    if (this->angleContext.isValid())
    {
        return true;
    }

    bool ready = false;
    if (!QMetaObject::invokeMethod(this, "prepareNativeSurface", Qt::BlockingQueuedConnection,
                                   Q_RETURN_ARG(bool, ready)) ||
        !ready)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error,
                               "Failed to prepare native render surface (winId unavailable)");
        return false;
    }

    bool created = false;
    if (!QMetaObject::invokeMethod(this, "createAngleContext", Qt::BlockingQueuedConnection,
                                   Q_RETURN_ARG(bool, created)) ||
        !created)
    {
        std::string message = "Failed to create ANGLE/EGL render context";
        const std::string& detail = this->angleContext.lastErrorMessage();
        if (!detail.empty())
        {
            message += ": ";
            message += detail;
        }
        CoreAddCallbackMessage(CoreDebugMessageType::Error, message.c_str());
        return false;
    }

    return true;
#else
    return this->openGLcontext != nullptr && this->openGLcontext->isValid();
#endif
}

#ifdef __APPLE__
bool OGLWidget::prepareNativeSurface()
{
    if (this->widgetContainer != nullptr)
    {
        this->widgetContainer->show();
        this->widgetContainer->update();
        if (this->widgetContainer->width() > 0 && this->widgetContainer->height() > 0)
        {
            this->resize(this->widgetContainer->size());
        }
    }
    else if (this->m_dedicatedWindow)
    {
        this->show();
        this->requestUpdate();
    }

    if (!this->handle())
    {
        this->create();
    }

    this->setVisible(true);
    this->requestUpdate();
    (void)this->winId();

    for (int attempt = 0; attempt < 1000 && this->winId() == 0; ++attempt)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }

    return this->winId() != 0;
}

bool OGLWidget::createAngleContext()
{
    if (this->angleContext.isValid())
    {
        return true;
    }

    return this->angleContext.create(this, this->swapInterval, this->contextMajorVersion, this->contextMinorVersion);
}
#endif

void OGLWidget::MoveContextToThread(QThread* thread)
{
#ifdef __APPLE__
    this->angleContext.moveToThread(thread);
#else
    QSurfaceFormat format = this->format();
    if (format.majorVersion() == 0)
    {
        format = QSurfaceFormat::defaultFormat();
    }

    this->GetContext()->setFormat(format);
    this->GetContext()->doneCurrent();
    this->GetContext()->create();
    this->GetContext()->moveToThread(thread);
#endif
}

QOpenGLContext* OGLWidget::GetContext()
{
#ifndef __APPLE__
    return this->openGLcontext;
#else
    return nullptr;
#endif
}

bool OGLWidget::IsContextValid() const
{
#ifdef __APPLE__
    return this->angleContext.isValid();
#else
    return this->openGLcontext != nullptr && this->openGLcontext->isValid();
#endif
}

bool OGLWidget::MakeContextCurrent()
{
#ifdef __APPLE__
    return this->angleContext.makeCurrent(this);
#else
    return this->openGLcontext != nullptr && this->openGLcontext->makeCurrent(this);
#endif
}

void OGLWidget::SwapContextBuffers()
{
#ifdef __APPLE__
    // A macOS system dialog (permission prompt, notification, update) takes the
    // app out of the foreground. ANGLE then blocks forever inside eglSwapBuffers
    // waiting for a Metal drawable, and lockstep stops sending inputs.
    if (CoreIsEmbeddedNetplayActive())
    {
        this->angleContext.setSwapInterval(0);

        if (QGuiApplication::applicationState() != Qt::ApplicationActive)
        {
            return;
        }
    }

    this->angleContext.swapBuffers(this);
    this->angleContext.makeCurrent(this);
#else
    this->openGLcontext->swapBuffers(this);
    this->openGLcontext->makeCurrent(this);
#endif
}

void* OGLWidget::GetProcAddress(const char* name) const
{
#ifdef __APPLE__
    return this->angleContext.getProcAddress(name);
#else
    if (this->openGLcontext == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(this->openGLcontext->getProcAddress(name));
#endif
}

std::uint32_t OGLWidget::DefaultFramebufferObject() const
{
#ifdef __APPLE__
    return this->angleContext.defaultFramebufferObject();
#else
    if (this->openGLcontext == nullptr)
    {
        return 0;
    }
    return this->openGLcontext->defaultFramebufferObject();
#endif
}

bool OGLWidget::CaptureScreenshot(std::vector<std::uint8_t>& rgbData, int& width, int& height) const
{
    // Skip while a deferred CoreSetVideoSize is pending — FB size is unstable
    // and an oversized glReadPixels can crash the driver.
    if (this->timerId != 0)
    {
        return false;
    }

    // Always take the *smaller* of pending vs applied size. Preferring only
    // appliedWidth crashed on shrink (applied stays large ~100ms); preferring
    // only this->width crashed on grow (FB lags behind).
    auto positiveMin = [](int a, int b) -> int {
        if (a <= 0)
        {
            return b;
        }
        if (b <= 0)
        {
            return a;
        }
        return std::min(a, b);
    };

    int captureWidth  = positiveMin(this->width, this->appliedWidth);
    int captureHeight = positiveMin(this->height, this->appliedHeight);

    // Also clamp to the live QWindow surface in device pixels.
    const int surfaceWidth  = static_cast<int>(QWindow::width() * this->devicePixelRatio());
    const int surfaceHeight = static_cast<int>(QWindow::height() * this->devicePixelRatio());
    captureWidth  = positiveMin(captureWidth, surfaceWidth);
    captureHeight = positiveMin(captureHeight, surfaceHeight);

    if (captureWidth <= 0 || captureHeight <= 0)
    {
        return false;
    }

#ifndef __APPLE__
    if (this->openGLcontext == nullptr || !this->openGLcontext->isValid())
    {
        return false;
    }

    if (!this->openGLcontext->makeCurrent(const_cast<OGLWidget*>(this)))
    {
        return false;
    }

    QOpenGLFunctions* glFunctions = this->openGLcontext->functions();
    if (glFunctions == nullptr)
    {
        return false;
    }

    using ReadBufferFn = void (*)(unsigned int);
    using GetErrorFn = unsigned int (*)();
    const auto glReadBuffer = reinterpret_cast<ReadBufferFn>(this->openGLcontext->getProcAddress("glReadBuffer"));
    const auto glGetError = reinterpret_cast<GetErrorFn>(this->openGLcontext->getProcAddress("glGetError"));

    // Clamp to the active viewport when available so we never read past the FB.
    GLint viewport[4] = {0, 0, 0, 0};
    glFunctions->glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] > 0 && viewport[3] > 0)
    {
        captureWidth  = std::min(captureWidth, static_cast<int>(viewport[2]));
        captureHeight = std::min(captureHeight, static_cast<int>(viewport[3]));
    }

    if (captureWidth <= 0 || captureHeight <= 0)
    {
        return false;
    }

    std::vector<std::uint8_t> fullBuffer(static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight) * 3u);

    const std::uint32_t framebuffer = this->DefaultFramebufferObject();
    if (framebuffer != 0)
    {
        glFunctions->glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        if (glReadBuffer != nullptr)
        {
            glReadBuffer(GL_COLOR_ATTACHMENT0);
        }
    }
    else
    {
        glFunctions->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        if (glReadBuffer != nullptr)
        {
            glReadBuffer(GL_BACK);
        }
    }

    if (glGetError != nullptr)
    {
        while (glGetError() != GL_NO_ERROR)
        {
        }
    }

    // Default GL_PACK_ALIGNMENT is 4. For tightly packed RGB rows, widths where
    // (width*3) % 4 != 0 (common when width ≡ 2 mod 4) write past the buffer.
    GLint previousPackAlignment = 4;
    glFunctions->glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
    glFunctions->glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glFunctions->glReadPixels(0, 0, captureWidth, captureHeight, GL_RGB, GL_UNSIGNED_BYTE, fullBuffer.data());
    glFunctions->glFinish();

    glFunctions->glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);

    if (glGetError != nullptr && glGetError() != GL_NO_ERROR)
    {
        return false;
    }

    return cropScreenshotBorders(fullBuffer, captureWidth, captureHeight, rgbData, width, height);
#else
    if (!this->angleContext.isValid() || !this->angleContext.makeCurrent(const_cast<OGLWidget*>(this)))
    {
        return false;
    }

    int angleSurfaceWidth  = 0;
    int angleSurfaceHeight = 0;
    if (this->angleContext.querySurfaceSize(angleSurfaceWidth, angleSurfaceHeight))
    {
        captureWidth  = positiveMin(captureWidth, angleSurfaceWidth);
        captureHeight = positiveMin(captureHeight, angleSurfaceHeight);
    }

    if (captureWidth <= 0 || captureHeight <= 0)
    {
        return false;
    }

    this->angleContext.prepareFramebufferReadback();

    using BindFramebufferFn = void (*)(unsigned int, unsigned int);
    using ReadBufferFn = void (*)(unsigned int);
    using ReadPixelsFn = void (*)(int, int, int, int, unsigned int, unsigned int, void*);
    using FinishFn = void (*)();
    using GetErrorFn = unsigned int (*)();
    using GetIntegervFn = void (*)(unsigned int, int*);
    using PixelStoreiFn = void (*)(unsigned int, int);

    const auto glBindFramebuffer = reinterpret_cast<BindFramebufferFn>(this->angleContext.getProcAddress("glBindFramebuffer"));
    const auto glReadBuffer = reinterpret_cast<ReadBufferFn>(this->angleContext.getProcAddress("glReadBuffer"));
    const auto glReadPixels = reinterpret_cast<ReadPixelsFn>(this->angleContext.getProcAddress("glReadPixels"));
    const auto glFinish = reinterpret_cast<FinishFn>(this->angleContext.getProcAddress("glFinish"));
    const auto glGetError = reinterpret_cast<GetErrorFn>(this->angleContext.getProcAddress("glGetError"));
    const auto glGetIntegerv = reinterpret_cast<GetIntegervFn>(this->angleContext.getProcAddress("glGetIntegerv"));
    const auto glPixelStorei = reinterpret_cast<PixelStoreiFn>(this->angleContext.getProcAddress("glPixelStorei"));

    if (glBindFramebuffer == nullptr || glReadPixels == nullptr || glFinish == nullptr)
    {
        return false;
    }

    const auto drainErrors = [&]() {
        if (glGetError == nullptr)
        {
            return;
        }
        while (glGetError() != 0)
        {
        }
    };

    const std::uint32_t framebuffer = this->DefaultFramebufferObject();
    if (framebuffer != 0)
    {
        glBindFramebuffer(kGlReadFramebuffer, framebuffer);
        if (glReadBuffer != nullptr)
        {
            glReadBuffer(kGlColorAttachment0);
        }
    }
    else
    {
        glBindFramebuffer(kGlReadFramebuffer, 0);
        if (glReadBuffer != nullptr)
        {
            glReadBuffer(kGlBack);
        }
    }

    drainErrors();

    int previousPackAlignment = 4;
    int previousPackRowLength = 0;
    int previousPackSkipRows = 0;
    int previousPackSkipPixels = 0;
    if (glGetIntegerv != nullptr)
    {
        glGetIntegerv(kGlPackAlignment, &previousPackAlignment);
        glGetIntegerv(kGlPackRowLength, &previousPackRowLength);
        glGetIntegerv(kGlPackSkipRows, &previousPackSkipRows);
        glGetIntegerv(kGlPackSkipPixels, &previousPackSkipPixels);
    }
    if (glPixelStorei != nullptr)
    {
        glPixelStorei(kGlPackAlignment, 1);
        glPixelStorei(kGlPackRowLength, 0);
        glPixelStorei(kGlPackSkipRows, 0);
        glPixelStorei(kGlPackSkipPixels, 0);
    }

    const auto readFormat = [&](unsigned int format, std::vector<std::uint8_t>& destination) -> bool {
        const int bytesPerPixel = readbackBytesPerPixel(format);
        if (bytesPerPixel == 0)
        {
            return false;
        }

        destination.resize(static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight) * static_cast<size_t>(bytesPerPixel));
        drainErrors();
        glReadPixels(0, 0, captureWidth, captureHeight, format, kGlUnsignedByte, destination.data());
        glFinish();
        if (glGetError != nullptr && glGetError() != 0)
        {
            drainErrors();
            return false;
        }
        return true;
    };

    unsigned int capturedFormat = kGlRgba;
    if (glGetIntegerv != nullptr)
    {
        int implementationFormat = 0;
        int implementationType = 0;
        glGetIntegerv(0x8B9B, &implementationFormat); // GL_IMPLEMENTATION_COLOR_READ_FORMAT
        glGetIntegerv(0x8B9A, &implementationType);   // GL_IMPLEMENTATION_COLOR_READ_TYPE
        drainErrors();
        if (implementationType == static_cast<int>(kGlUnsignedByte) &&
            readbackBytesPerPixel(static_cast<unsigned int>(implementationFormat)) != 0)
        {
            capturedFormat = static_cast<unsigned int>(implementationFormat);
        }
    }

    std::vector<std::uint8_t> readBuffer;
    bool readOk = readFormat(capturedFormat, readBuffer);
    if (!readOk && capturedFormat != kGlRgba)
    {
        capturedFormat = kGlRgba;
        readOk = readFormat(capturedFormat, readBuffer);
    }
    if (!readOk && capturedFormat != kGlBgra)
    {
        capturedFormat = kGlBgra;
        readOk = readFormat(capturedFormat, readBuffer);
    }

    if (glPixelStorei != nullptr)
    {
        glPixelStorei(kGlPackAlignment, previousPackAlignment);
        glPixelStorei(kGlPackRowLength, previousPackRowLength);
        glPixelStorei(kGlPackSkipRows, previousPackSkipRows);
        glPixelStorei(kGlPackSkipPixels, previousPackSkipPixels);
    }

    if (!readOk)
    {
        return false;
    }

    std::vector<std::uint8_t> fullBuffer;
    if (!packReadbackToRgb(readBuffer, captureWidth, captureHeight, capturedFormat, fullBuffer))
    {
        return false;
    }

    return cropScreenshotBorders(fullBuffer, captureWidth, captureHeight, rgbData, width, height);
#endif
}

void OGLWidget::SetHideCursor(bool hide)
{
    this->setCursor(hide ? Qt::BlankCursor : Qt::ArrowCursor);
}

void OGLWidget::ApplyDedicatedWindowChrome(const QIcon& icon)
{
    if (!this->m_dedicatedWindow)
    {
        return;
    }

    if (!icon.isNull())
    {
        this->setIcon(icon);
    }
}

void OGLWidget::closeEvent(QCloseEvent* event)
{
    if (!this->m_dedicatedWindow)
    {
        QWindow::closeEvent(event);
        return;
    }

    event->ignore();
    emit dedicatedWindowCloseRequested();
}

void OGLWidget::ShowRenderSurface()
{
    if (this->m_dedicatedWindow)
    {
        this->show();
        this->raise();
        this->requestActivate();
        return;
    }

    if (this->widgetContainer != nullptr)
    {
        this->widgetContainer->show();
    }
}

void OGLWidget::HideRenderSurface()
{
    if (this->m_dedicatedWindow)
    {
        this->hide();
        return;
    }

    if (this->widgetContainer != nullptr)
    {
        this->widgetContainer->hide();
    }
}

QWidget* OGLWidget::GetWidget(void)
{
    return this->widgetContainer;
}

bool OGLWidget::handleMouseEvent(QEvent* event)
{
    switch (event->type())
    {
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease:
    {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
        const float scale       = static_cast<float>(this->devicePixelRatio());
        const float x           = static_cast<float>(mouseEvent->position().x()) * scale;
        const float y           = static_cast<float>(mouseEvent->position().y()) * scale;
        const Qt::KeyboardModifiers keyboardModifiers =
            mouseEvent->modifiers() | QGuiApplication::keyboardModifiers();
        const bool configureModifier = (keyboardModifiers & Qt::AltModifier) != 0;
        bool consumed = false;

        switch (event->type())
        {
        case QEvent::MouseButtonPress:
            if (mouseEvent->button() == Qt::LeftButton)
            {
                consumed = OnScreenDisplayHandleMousePress(x, y, configureModifier);
            }
            break;
        case QEvent::MouseMove:
            consumed = OnScreenDisplayHandleMouseMove(x, y, configureModifier);
            break;
        case QEvent::MouseButtonRelease:
            if (mouseEvent->button() == Qt::LeftButton)
            {
                consumed = OnScreenDisplayHandleMouseRelease();
            }
            break;
        default:
            break;
        }

        return consumed || OnScreenDisplayIsDragging();
    }
    default:
        break;
    }

    return false;
}

bool OGLWidget::event(QEvent* event)
{
    if (this->m_dedicatedWindow && this->handleMouseEvent(event))
    {
        return true;
    }

    if (this->m_dedicatedWindow && event->type() == QEvent::Resize)
    {
        this->queueVideoSizeUpdate(static_cast<QResizeEvent*>(event)->size());
    }

    return QWindow::event(event);
}

bool OGLWidget::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->widgetContainer && event->type() == QEvent::Resize)
    {
        this->queueVideoSizeUpdate(static_cast<QResizeEvent *>(event)->size());
    }
    else if (object == this->widgetContainer)
    {
        if (this->handleMouseEvent(event))
        {
            return true;
        }
    }

    return QWindow::eventFilter(object, event);
}

void OGLWidget::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event);
    // Resize is handled via the container event filter; QWindow resize events
    // can report a different size and cause redundant CoreSetVideoSize calls.
}

void OGLWidget::queueVideoSizeUpdate(QSize size)
{
    if (!this->isVisible())
    {
        return;
    }

    // account for HiDPI scaling
    // see https://github.com/Rosalie241/RMG/issues/2
    const int newWidth  = static_cast<int>(size.width()  * this->devicePixelRatio()) & ~0x1;
    const int newHeight = static_cast<int>(size.height() * this->devicePixelRatio()) & ~0x1;

    if (newWidth == this->appliedWidth && newHeight == this->appliedHeight)
    {
        return;
    }

    if (newWidth == this->width && newHeight == this->height)
    {
        return;
    }

    this->width  = newWidth;
    this->height = newHeight;

    if (this->timerId != 0)
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
    }

    this->timerId = this->startTimer(100);

    // Keep OSD anchored while user is actively resizing.
    OnScreenDisplaySetDisplaySize(this->width, this->height);
}

void OGLWidget::timerEvent(QTimerEvent *event)
{
    Q_UNUSED(event);

    if (!this->isVisible())
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
        return;
    }

    if (this->width == this->appliedWidth && this->height == this->appliedHeight)
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
        return;
    }

    // only remove current timer
    // when setting the video size succeeds
    if (CoreSetVideoSize(this->width, this->height))
    {
        this->appliedWidth  = this->width;
        this->appliedHeight = this->height;
        this->killTimer(this->timerId);
        this->timerId = 0;
    }
}
