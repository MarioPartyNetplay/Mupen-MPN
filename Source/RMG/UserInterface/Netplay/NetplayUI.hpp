#ifndef NETPLAYDIALOG_H
#define NETPLAYDIALOG_H

#include <QDialog>
#include <QTabWidget>
#include <QPointer>
#include "Host.hpp"
#include "Join.hpp"

class NetplayUI : public QDialog
{
    Q_OBJECT
public:
    explicit NetplayUI(QWidget *parent = nullptr);
    ~NetplayUI();

private:
    QTabWidget *tabWidget;
    QPointer<Host> createRoomTab;
    QPointer<Join> joinRoomTab;
};

#endif // NETPLAYDIALOG_H