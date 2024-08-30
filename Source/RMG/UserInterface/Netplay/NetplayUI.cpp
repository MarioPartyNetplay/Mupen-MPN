#include "NetplayUI.hpp"

NetplayUI::NetplayUI(QWidget *parent)
    : QDialog(parent),
      tabWidget(new QTabWidget(this)),
      createRoomTab(new Host(this)),
      joinRoomTab(new Join(this))
{
    setWindowTitle("Netplay Setup");
    setMinimumWidth(1000);
    setMinimumHeight(500);

    tabWidget->addTab(createRoomTab, "Create Room");
    tabWidget->addTab(joinRoomTab, "Join Room");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(tabWidget);
    setLayout(mainLayout);
}

NetplayUI::~NetplayUI()
{
    tabWidget->removeTab(tabWidget->indexOf(createRoomTab)); // Remove createRoomTab from tabWidget
    tabWidget->removeTab(tabWidget->indexOf(joinRoomTab)); // Remove joinRoomTab from tabWidget

    delete createRoomTab; // Delete createRoomTab
    delete joinRoomTab; // Delete joinRoomTab

    tabWidget->deleteLater(); // Delete tabWidget
    tabWidget = nullptr; // Set tabWidget to nullptr
}