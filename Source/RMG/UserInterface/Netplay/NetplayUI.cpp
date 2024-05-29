#include "NetplayUI.hpp"

#include <QVBoxLayout>

namespace UserInterface {

NetplayUI::NetplayUI(QWidget *parent)
    : QDialog(parent)
{
    tabWidget = new QTabWidget(this);

    createRoomTab = new CreateRoom(this);
    joinRoomTab = new JoinRoom(this);

    tabWidget->addTab(createRoomTab, "Host");
    tabWidget->addTab(joinRoomTab, "Join");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(tabWidget);
    setLayout(mainLayout);
}

}