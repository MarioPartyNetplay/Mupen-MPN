/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef VKWIDGET_HPP
#define VKWIDGET_HPP

#include <QThread>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QWindow>
#include <QTimerEvent>
#include <QWidget>
#include <QIcon>

namespace UserInterface
{
namespace Widget
{
class VKWidget : public QWindow
{
    Q_OBJECT

  public:
    VKWidget(QWidget *parent = nullptr, bool dedicatedWindow = false);
    ~VKWidget(void);

    bool UsesDedicatedWindow() const { return m_dedicatedWindow; }
    void ApplyDedicatedWindowChrome(const QIcon& icon);
    void ShowRenderSurface();
    void HideRenderSurface();

    void SetHideCursor(bool hide);

    QWidget* GetWidget(void);

  signals:
    void dedicatedWindowCloseRequested();

  protected:
    void closeEvent(QCloseEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    void resizeEvent(QResizeEvent *) Q_DECL_OVERRIDE;
    void timerEvent(QTimerEvent *) Q_DECL_OVERRIDE;

  private:
    void queueVideoSizeUpdate(QSize size);
    bool handleMouseEvent(QEvent* event);

    bool m_dedicatedWindow = false;
    QWidget* widgetContainer = nullptr;
    int width                = 0;
    int height               = 0;
    int timerId              = 0;
    int appliedWidth         = 0;
    int appliedHeight        = 0;
};
} // namespace Widget
} // namespace UserInterface

#endif // VKWIDGET_HPP