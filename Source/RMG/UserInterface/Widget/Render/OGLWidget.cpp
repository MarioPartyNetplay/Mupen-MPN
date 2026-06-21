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

#include <QEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QSurfaceFormat>

#include <RMG-Core/Video.hpp>
#include <RMG-Core/Callback.hpp>

#include <string>

using namespace UserInterface::Widget;

OGLWidget::OGLWidget(QWidget *parent)
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

void OGLWidget::SetHideCursor(bool hide)
{
    this->setCursor(hide ? Qt::BlankCursor : Qt::ArrowCursor);
}

QWidget* OGLWidget::GetWidget(void)
{
    return this->widgetContainer;
}

bool OGLWidget::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->widgetContainer && event->type() == QEvent::Resize)
    {
        this->queueVideoSizeUpdate(static_cast<QResizeEvent *>(event)->size());
    }
    else if (object == this->widgetContainer)
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
            const bool configureModifier = (mouseEvent->modifiers() & Qt::AltModifier) != 0;
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

            if (consumed || OnScreenDisplayIsDragging())
            {
                return true;
            }
            break;
        }
        default:
            break;
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
