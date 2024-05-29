#include "NetplayUI.hpp"
#include "createroom.h"
#include "joinroom.h"
#include <QVBoxLayout>

namespace UserInterface {

NetplayUI::NetplayUI(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("NetPlay Setup");
    tabWidget = new QTabWidget(this);
    
    joinRoomTab = new JoinRoom(this);
    createRoomTab = new CreateRoom(this);

    tabWidget->addTab(joinRoomTab, "Connect");
    tabWidget->addTab(createRoomTab, "Host");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(tabWidget);
    setLayout(mainLayout);
    connect(createRoomTab, &CreateRoom::roomClosed, this, &NetplayUI::close);
    connect(joinRoomTab, &JoinRoom::roomClosed, this, &NetplayUI::close);
}
}