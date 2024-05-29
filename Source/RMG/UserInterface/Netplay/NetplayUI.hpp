#ifndef NETPLAYUI_HPP
#define NETPLAYUI_HPP

#include <QDialog>
#include <QTabWidget>
#include "createroom.h"
#include "joinroom.h"

namespace UserInterface {

class NetplayUI : public QDialog
{
    Q_OBJECT

public:
    NetplayUI(QWidget *parent = nullptr);

private:
    QTabWidget *tabWidget;
    CreateRoom *createRoomTab;
    JoinRoom *joinRoomTab;
};

} // namespace UserInterface

#endif // NETPLAYUI_HPP