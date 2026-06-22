/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "BoardDownloaderDialog.hpp"
#include "BoardDownloaderDetailDialog.hpp"
#include "Utilities/QtMessageBox.hpp"

#include "BoardDownloaderCommon.hpp"

#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrlQuery>

#include <functional>

using namespace UserInterface::Dialog;
using namespace Utilities;

namespace
{

QString truncateDescription(const QString& text, int wordLimit = 12)
{
    const QStringList words = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (words.size() <= wordLimit)
    {
        return text;
    }

    return words.mid(0, wordLimit).join(QStringLiteral(" ")) + QStringLiteral("...");
}

QFrame* createProjectCard(QWidget* parent,
                          const QString& title,
                          const QString& description,
                          const QPixmap& icon,
                          const std::function<void()>& onMoreInfo)
{
    auto* card = new QFrame(parent);
    card->setFrameShape(QFrame::StyledPanel);
    card->setMinimumSize(300, 220);
    card->setMaximumWidth(360);

    auto* layout = new QVBoxLayout(card);

    auto* iconLabel = new QLabel(card);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setMinimumHeight(96);
    if (!icon.isNull())
    {
        iconLabel->setPixmap(icon.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    else
    {
        iconLabel->setPixmap(QIcon::fromTheme("image-line").pixmap(64, 64));
    }
    layout->addWidget(iconLabel);

    auto* titleLabel = new QLabel(title, card);
    titleLabel->setWordWrap(true);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    auto* descriptionLabel = new QLabel(description, card);
    descriptionLabel->setWordWrap(true);
    descriptionLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    layout->addWidget(descriptionLabel);

    auto* button = new QPushButton(QStringLiteral("More Info"), card);
    QObject::connect(button, &QPushButton::clicked, card, onMoreInfo);
    layout->addWidget(button);

    return card;
}

} // namespace

BoardDownloaderDialog::BoardDownloaderDialog(QWidget* parent) : QDialog(parent)
{
    this->setupUi(this);
    this->setWindowIcon(QIcon::fromTheme("download-cloud-line", QIcon(":Resource/RMG.png")));
    this->networkManager = new QNetworkAccessManager(this);
}

BoardDownloaderDialog::~BoardDownloaderDialog(void) = default;

void BoardDownloaderDialog::clearResults(void)
{
    QLayoutItem* item = nullptr;
    while ((item = this->resultsGridLayout->takeAt(0)) != nullptr)
    {
        if (item->widget() != nullptr)
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
}

void BoardDownloaderDialog::setSearching(bool searching)
{
    this->searchButton->setEnabled(!searching);
    this->searchLineEdit->setEnabled(!searching);
}

void BoardDownloaderDialog::on_searchButton_clicked(void)
{
    const QString searchTerm = this->searchLineEdit->text().trimmed();
    if (searchTerm.isEmpty())
    {
        return;
    }

    if (this->searchReply != nullptr)
    {
        this->searchReply->abort();
        this->searchReply->deleteLater();
        this->searchReply = nullptr;
    }

    this->clearResults();
    this->setSearching(true);
    this->statusLabel->setText(QStringLiteral("Searching for \"%1\"...").arg(searchTerm));

    QUrl url(boardDownloaderApiBaseUrl() + QStringLiteral("/project/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("searchTerm"), searchTerm);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    this->searchReply = this->networkManager->get(request);
    connect(this->searchReply, &QNetworkReply::finished, this, &BoardDownloaderDialog::on_searchReply_finished);
}

void BoardDownloaderDialog::on_searchLineEdit_returnPressed(void)
{
    this->on_searchButton_clicked();
}

void BoardDownloaderDialog::on_searchReply_finished(void)
{
    this->setSearching(false);

    if (this->searchReply == nullptr)
    {
        return;
    }

    if (this->searchReply->error() != QNetworkReply::NoError)
    {
        QtMessageBox::Error(this, QStringLiteral("Search failed"), this->searchReply->errorString());
        this->statusLabel->setText(QStringLiteral("Search failed."));
        this->searchReply->deleteLater();
        this->searchReply = nullptr;
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(this->searchReply->readAll());
    this->searchReply->deleteLater();
    this->searchReply = nullptr;

    const QJsonArray results = document.array();
    if (results.isEmpty())
    {
        this->statusLabel->setText(QStringLiteral("No projects found."));
        return;
    }

    this->statusLabel->setText(QStringLiteral("Found %1 project(s). Loading details...").arg(results.size()));

    for (const QJsonValue& value : results)
    {
        const QJsonObject project = value.toObject();
        const int projectId = project.value(QStringLiteral("projectId")).toInt();
        const QString projectName = project.value(QStringLiteral("name")).toString();
        const int gameId = project.value(QStringLiteral("gameId")).toInt();
        if (projectId <= 0)
        {
            continue;
        }

        this->fetchProjectDetails(projectId, projectName, gameId);
    }
}

void BoardDownloaderDialog::fetchProjectDetails(int projectId, const QString& projectName, int gameId)
{
    const QUrl url(boardDownloaderApiBaseUrl() + QStringLiteral("/project/%1").arg(projectId));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = this->networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, projectId, projectName, gameId]() {
        this->handleProjectDetailsReply(reply, projectId, projectName, gameId);
    });
}

void BoardDownloaderDialog::handleProjectDetailsReply(QNetworkReply* reply, int projectId, const QString& projectName, int gameId)
{
    if (reply->error() != QNetworkReply::NoError)
    {
        this->addProjectCard(projectId,
                             projectName,
                             QJsonObject{
                                 {QStringLiteral("name"), projectName},
                                 {QStringLiteral("gameId"), gameId},
                                 {QStringLiteral("description"), QStringLiteral("Failed to load project details.")},
                             },
                             QPixmap());
        reply->deleteLater();
        return;
    }

    QJsonObject details = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();

    details.insert(QStringLiteral("name"), details.value(QStringLiteral("name")).toString(projectName));
    details.insert(QStringLiteral("creation_date"), details.value(QStringLiteral("creation_date")).toString());
    if (gameId > 0)
    {
        details.insert(QStringLiteral("gameId"), gameId);
    }
    details.insert(QStringLiteral("custom_events"),
                   details.contains(QStringLiteral("customEvents"))
                       ? details.value(QStringLiteral("customEvents")).toBool()
                       : details.value(QStringLiteral("custom_events")).toInt() != 0);
    details.insert(QStringLiteral("custom_music"),
                   details.contains(QStringLiteral("customMusic"))
                       ? details.value(QStringLiteral("customMusic")).toBool()
                       : details.value(QStringLiteral("custom_music")).toInt() != 0);

    this->fetchProjectIcon(projectId, details);
}

void BoardDownloaderDialog::fetchProjectIcon(int projectId, const QJsonObject& details)
{
    const QString iconUrl = details.value(QStringLiteral("icon")).toString();
    if (iconUrl.isEmpty())
    {
        this->addProjectCard(projectId, details.value(QStringLiteral("name")).toString(), details, QPixmap());
        return;
    }

    const QUrl iconRequestUrl(iconUrl);
    QNetworkRequest request(iconRequestUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = this->networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, projectId, details]() {
        QPixmap icon;
        if (reply->error() == QNetworkReply::NoError)
        {
            icon.loadFromData(reply->readAll());
        }

        reply->deleteLater();
        this->addProjectCard(projectId, details.value(QStringLiteral("name")).toString(), details, icon);
    });
}

void BoardDownloaderDialog::addProjectCard(int projectId,
                                           const QString& projectName,
                                           const QJsonObject& details,
                                           const QPixmap& icon)
{
    const QString author = details.value(QStringLiteral("author")).toString();
    const QString description = truncateDescription(
        details.value(QStringLiteral("description")).toString(QStringLiteral("No description available")));

    const int cardCount = this->resultsGridLayout->count();
    const int row = cardCount / 2;
    const int column = cardCount % 2;

    QJsonObject detailCopy = details;

    QFrame* card = createProjectCard(
        this->resultsContainer,
        author.isEmpty() ? projectName : QStringLiteral("%1: by %2").arg(projectName, author),
        description,
        icon,
        [this, projectId, detailCopy, icon]() {
            BoardDownloaderDetailDialog dialog(this, projectId, detailCopy, icon);
            dialog.exec();
        });

    this->resultsGridLayout->addWidget(card, row, column);
    this->statusLabel->setText(QStringLiteral("Loaded %1 project(s).").arg(this->resultsGridLayout->count()));
}
