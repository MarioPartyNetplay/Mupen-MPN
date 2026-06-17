/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "NetplayLobbyDialog.hpp"
#include "CreateNetplaySessionDialog.hpp"
#include "NetplaySessionBrowserDialog.hpp"
#include "NetplayCommon.hpp"

#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>

#include <RMG-Core/Settings.hpp>

using namespace UserInterface::Dialog;

NetplayLobbyDialog::NetplayLobbyDialog(QWidget* parent, Netplay::NetplayCoordinator* coordinator,
                                       const QMap<QString, CoreRomSettings>& romData, InitialTab initialTab)
    : QDialog(parent), coordinator(coordinator)
{
    this->setupUi(this);

    this->createPage = new CreateNetplaySessionDialog(this->hostTab, this->coordinator, romData);
    this->createPage->setEmbeddedMode(true);
    this->createPage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    this->hostTabLayout->addWidget(this->createPage);

    this->joinPage = new NetplaySessionBrowserDialog(this->joinTab, this->coordinator, romData);
    this->joinPage->setEmbeddedMode(true);
    this->joinPage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    this->joinTabLayout->addWidget(this->joinPage);

    QRegularExpression nicknameRe(NETPLAYCOMMON_NICKNAME_REGEX);
    this->nickNameLineEdit->setValidator(new QRegularExpressionValidator(nicknameRe, this));
    {
        QSignalBlocker blocker(this->nickNameLineEdit);
        this->nickNameLineEdit->setText(QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Netplay_Nickname)));
    }

    if (QPushButton* actionButton = this->buttonBox->button(QDialogButtonBox::Ok)) {
        actionButton->setText("Create");
    }

    connect(this->createPage, &CreateNetplaySessionDialog::canSubmitChanged,
            this, &NetplayLobbyDialog::onCreatePageCanSubmitChanged);
    connect(this->joinPage, &NetplaySessionBrowserDialog::canSubmitChanged,
            this, &NetplayLobbyDialog::onJoinPageCanSubmitChanged);
    connect(this->createPage, &CreateNetplaySessionDialog::sessionAccepted,
            this, &NetplayLobbyDialog::onPageSessionAccepted);
    connect(this->joinPage, &NetplaySessionBrowserDialog::sessionAccepted,
            this, &NetplayLobbyDialog::onPageSessionAccepted);
    connect(this->buttonBox, &QDialogButtonBox::accepted, this, &NetplayLobbyDialog::accept);

    this->tabWidget->setCurrentIndex(static_cast<int>(initialTab));
    this->syncNicknameToPages();
    this->updateActionButton();
}

void NetplayLobbyDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    if (!this->joinPageRefreshed && this->joinPage != nullptr)
    {
        this->joinPageRefreshed = true;
        this->joinPage->refreshRoomList();
    }
}

NetplayLobbyDialog::~NetplayLobbyDialog(void)
{
    const QString nickname = this->nickNameLineEdit->text();
    if (!nickname.isEmpty())
    {
        CoreSettingsSetValue(SettingsID::Netplay_Nickname, nickname.toStdString());
    }
}

QString NetplayLobbyDialog::GetSessionFile(void)
{
    if (this->tabWidget->currentIndex() == 0)
    {
        return this->createPage->GetSessionFile();
    }

    return this->joinPage->GetSessionFile();
}

void NetplayLobbyDialog::syncNicknameToPages(void)
{
    if (this->createPage == nullptr || this->joinPage == nullptr) {
        return;
    }

    const QString nickname = this->nickNameLineEdit->text();
    this->createPage->setNickname(nickname);
    this->joinPage->setNickname(nickname);
}

void NetplayLobbyDialog::updateActionButton(void)
{
    QPushButton* actionButton = this->buttonBox->button(QDialogButtonBox::Ok);
    if (actionButton == nullptr)
    {
        return;
    }

    const bool nicknameValid = NetplayCommon::IsValidNickname(this->nickNameLineEdit->text());

    if (this->tabWidget->currentIndex() == 0)
    {
        actionButton->setText("Create");
        actionButton->setEnabled(nicknameValid && this->createPage->canSubmit());
    }
    else
    {
        actionButton->setText("Join");
        actionButton->setEnabled(nicknameValid && this->joinPage->canSubmit());
    }
}

void NetplayLobbyDialog::on_nickNameLineEdit_textChanged(void)
{
    this->syncNicknameToPages();
    this->updateActionButton();
}

void NetplayLobbyDialog::on_tabWidget_currentChanged(int index)
{
    if (index == 1 && !this->joinPageRefreshed && this->joinPage != nullptr)
    {
        this->joinPageRefreshed = true;
        this->joinPage->refreshRoomList();
    }

    this->updateActionButton();
}

void NetplayLobbyDialog::onCreatePageCanSubmitChanged(bool canSubmit)
{
    Q_UNUSED(canSubmit);
    if (this->tabWidget->currentIndex() == 0)
    {
        this->updateActionButton();
    }
}

void NetplayLobbyDialog::onJoinPageCanSubmitChanged(bool canSubmit)
{
    Q_UNUSED(canSubmit);
    if (this->tabWidget->currentIndex() == 1)
    {
        this->updateActionButton();
    }
}

void NetplayLobbyDialog::onPageSessionAccepted(void)
{
    QDialog::accept();
}

void NetplayLobbyDialog::accept(void)
{
    this->syncNicknameToPages();

    if (this->tabWidget->currentIndex() == 0)
    {
        this->createPage->submit();
    }
    else
    {
        this->joinPage->submit();
    }
}
