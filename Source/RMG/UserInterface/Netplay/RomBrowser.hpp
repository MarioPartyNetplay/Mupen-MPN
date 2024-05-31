#ifndef ROMBROWSER_HPP
#define ROMBROWSER_HPP

#include <QWidget>
#include <QListWidget>
#include <QStringList>

namespace UserInterface {
namespace Widget {

class RomBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit RomBrowser(QWidget *parent = nullptr);
    static QString cleanRomName(const QString &name);
signals:
    void romDoubleClicked(const QString &romPath);

private slots:
    void onItemDoubleClicked(QListWidgetItem *item);

private:
    QListWidget *listWidget;
    QStringList romList;
    void loadRoms();
    QString getRomGoodName(const QString &romPath);
};

} // namespace Widget
} // namespace UserInterface

#endif // ROMBROWSER_HPP