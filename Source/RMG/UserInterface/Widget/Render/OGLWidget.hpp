/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef OGLWIDGET_HPP
#define OGLWIDGET_HPP

#include <QThread>
#include <QResizeEvent>
#include <QWindow>
#include <QTimerEvent>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QWidget>

#ifdef __APPLE__
#include "AngleContext.hpp"
#endif

namespace UserInterface
{
namespace Widget
{
class OGLWidget : public QWindow
{
    Q_OBJECT

  public:
    OGLWidget(QWidget *);
    ~OGLWidget(void);

#ifdef __APPLE__
    Q_INVOKABLE bool prepareNativeSurface();
    Q_INVOKABLE bool createAngleContext();
#endif

    void MoveContextToThread(QThread* thread);
    QOpenGLContext* GetContext();

    void PrepareRenderContext(const QSurfaceFormat& format, QThread* thread);
    bool EnsureRenderContext();
    bool IsContextValid() const;
    bool MakeContextCurrent();
    void SwapContextBuffers();
    void* GetProcAddress(const char* name) const;
    std::uint32_t DefaultFramebufferObject() const;

    void SetHideCursor(bool hide);

    QWidget* GetWidget(void);

  protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void resizeEvent(QResizeEvent *) Q_DECL_OVERRIDE;
    void timerEvent(QTimerEvent *) Q_DECL_OVERRIDE;

  private:
    void queueVideoSizeUpdate(QSize size);

    QWidget* widgetContainer      = nullptr;
    QOpenGLContext* openGLcontext = nullptr;
#ifdef __APPLE__
    AngleContext angleContext;
    int swapInterval = 0;
    int contextMajorVersion = 3;
    int contextMinorVersion = 0;
#endif
    int width   = 0;
    int height  = 0;
    int timerId = 0;
    int appliedWidth  = 0;
    int appliedHeight = 0;
};
} // namespace Widget
} // namespace UserInterface

#endif // OGLWIDGET_HPP
