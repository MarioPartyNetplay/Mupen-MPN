/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "AddCheatDialog.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <QPushButton>
#include <QTextBlock>
#include <QCloseEvent>
#include <QRegularExpression>
#include <QFileInfo>

#include <RMG-Core/Error.hpp>

using namespace UserInterface::Dialog;
using namespace Utilities;

AddCheatDialog::AddCheatDialog(QWidget *parent, QString file) : QDialog(parent)
{
    this->setupUi(this);

    this->file = file;

    this->validateOkButton();
}

AddCheatDialog::~AddCheatDialog(void)
{
}

void AddCheatDialog::setModificationInternalName(const QString& internalName)
{
    this->modificationInternalName = internalName;
    if (!this->modificationInternalName.isEmpty())
    {
        this->configureModificationUi();
    }
}

void AddCheatDialog::configureModificationUi(void)
{
    this->setWindowTitle(this->updateMode ? "Edit Code" : "Add New Code");

    this->label->setVisible(false);
    this->label_2->setVisible(false);
    this->authorLineEdit->setVisible(false);
    this->label_3->setVisible(false);
    this->label_4->setVisible(false);
    this->label_5->setVisible(false);
    this->label_6->setVisible(false);
    this->label_7->setVisible(false);
    this->optionsTextEdit->setVisible(false);
    this->codeTextEdit->setVisible(false);

    this->nameLineEdit->setPlaceholderText("Name");
    this->notesTextEdit->setPlaceholderText("Description");
}

void AddCheatDialog::SetCheat(CoreCheat cheat)
{
    // change window title
    this->setWindowTitle(this->modificationInternalName.isEmpty() ? "Edit Cheat" : "Edit Code");

    if (!this->modificationInternalName.isEmpty())
    {
        this->configureModificationUi();
    }

    // fill in UI elements
    this->nameLineEdit->setText(QString::fromStdString(cheat.Name));
    this->authorLineEdit->setText(QString::fromStdString(cheat.Author));
    this->notesTextEdit->setPlainText(QString::fromStdString(cheat.Note));

    this->nameLineEdit->setCursorPosition(0);
    this->authorLineEdit->setCursorPosition(0);

    std::vector<std::string> codeLines;
    std::vector<std::string> optionLines;

    // try to retrieve the cheat text lines
    if (!CoreGetCheatLines(cheat, codeLines, optionLines))
    {
        return;
    }

    this->setPlainTextEditLines(this->codeTextEdit, codeLines);
    this->setPlainTextEditLines(this->optionsTextEdit, optionLines);

    this->updateMode = true;
    this->oldCheat = cheat;
}

void AddCheatDialog::setPlainTextEditLines(QPlainTextEdit* plainTextEdit, const std::vector<std::string>& lines)
{
    // reset text edit
    plainTextEdit->clear();

    // add lines
    for (const std::string& line : lines)
    {
        plainTextEdit->appendPlainText(QString::fromStdString(line));
    }

    // reset cursor
    plainTextEdit->moveCursor(QTextCursor::Start);
    plainTextEdit->ensureCursorVisible();

    // reset redo & undo stack
    plainTextEdit->document()->clearUndoRedoStacks();
}

bool AddCheatDialog::validate(void)
{
    QStringList documentLines;
    bool foundOption = false;
    int  optionSize  = -1;
    QRegularExpression hexRegExpr("^[0-9A-F]+$");

    // ensure name isn't empty
    QString name = this->nameLineEdit->text();
    if (name.isEmpty())
    {
        return false;
    }
    else
    { // name can't start with or end with '\'
        if (name.startsWith("\\") ||
            name.endsWith("\\"))
        {
            return false;
        }
    }

    documentLines = this->getLines(this->codeTextEdit->document());

    // ensure code lines aren't empty
    if (documentLines.isEmpty())
    {
        return this->updateMode && !this->modificationInternalName.isEmpty();
    }

    // parse code lines
    for (const QString& line : documentLines)
    {
        QStringList splitLine = line.split(' ');

        if (splitLine.size() != 2)
        {
            return false;
        }

        QString address = splitLine.at(0);
        QString value = splitLine.at(1);

        if (address.size() != 8 || value.size() != 4)
        {
            return false;
        }
        
        // address needs to be hex
        if (!hexRegExpr.match(address).hasMatch())
        {
            return false;
        }

        QString tmpValue = value;
        tmpValue.remove("?");

        // value without '?' should be hex
        if (!tmpValue.isEmpty() && !hexRegExpr.match(tmpValue).hasMatch())
        {
            return false;
        }

        int optionCount = value.count("?");
        if (optionCount > 0)
        {
            if (!foundOption)
            {
                optionSize = optionCount;
                foundOption = true;
            }
            else if (foundOption && optionCount != optionSize)
            { // if it doesn't match the existing size, fail
                return false;
            }
        }
    }

    // parse option lines
    documentLines = this->getLines(this->optionsTextEdit->document());
    if (!foundOption)
    {
        return documentLines.isEmpty();
    }

    for (const QString& line : documentLines)
    {
        QStringList splitLine = line.split(' ');

        if (splitLine.size() < 2)
        {
            return false;
        }

        QString value = splitLine.at(0);
        QString name = splitLine.at(1);

        if (value.size() != optionSize)
        {
            return false;
        }

        if (name.isEmpty())
        {
            return false;
        }

        // value needs to be hex
        if (!hexRegExpr.match(value).hasMatch())
        {
            return false;
        }
    }

    return true;
}

void AddCheatDialog::validateOkButton(void)
{
    QPushButton* okButton = this->buttonBox->button(QDialogButtonBox::Ok);
    okButton->setEnabled(this->validate());
}

QStringList AddCheatDialog::getLines(QTextDocument* textDocument)
{
    QString line;
    QStringList lines;

    lines.reserve(textDocument->lineCount());

    for (int i = 0; i < textDocument->lineCount(); i++)
    {
        line = textDocument->findBlockByLineNumber(i).text();
        if (!line.isEmpty())
        {
            lines.push_back(line);
        }
    }

    return lines;
}

bool AddCheatDialog::getCheat(CoreCheat& cheat)
{
    QStringList qLines;
    std::vector<std::string> lines;
    std::vector<std::string> codeLines;
    std::vector<std::string> optionLines;

    const QString name = this->nameLineEdit->text();
    const QString author = this->authorLineEdit->text();
    const QString note = this->notesTextEdit->toPlainText();

    lines.push_back("$" + name.toStdString());

    if (this->modificationInternalName.isEmpty())
    {
        if (!author.isEmpty())
        {
            lines.push_back("Author=" + author.toStdString());
        }
    }

    if (!note.isEmpty())
    {
        lines.push_back("Note=" + note.toStdString());
    }

    if (!this->modificationInternalName.isEmpty() && this->updateMode)
    {
        if (!CoreGetCheatLines(this->oldCheat, codeLines, optionLines))
        {
            return false;
        }
    }
    else
    {
        qLines = this->getLines(this->codeTextEdit->document());
        for (const QString& line : qLines)
        {
            codeLines.push_back(line.toStdString());
        }
        qLines = this->getLines(this->optionsTextEdit->document());
        for (const QString& line : qLines)
        {
            optionLines.push_back(line.toStdString());
        }
    }

    for (const std::string& line : codeLines)
    {
        lines.push_back(line);
    }
    for (const std::string& line : optionLines)
    {
        lines.push_back(line);
    }

    if (!CoreParseCheat(lines, cheat))
    {
        QtMessageBox::Error(this, "CoreParseCheat() Failed", QString::fromStdString(CoreGetError()));
        return false;
    }

    return true;
}

bool AddCheatDialog::addCheat(void)
{
    CoreCheat cheat;

    if (!this->getCheat(cheat))
    {
        return false;
    }

    if (!CoreAddCheat(this->file.toStdU32String(), cheat))
    {
        QtMessageBox::Error(this, "CoreAddCheat() Failed", QString::fromStdString(CoreGetError()));
        return false;
    }

    return true;
}

bool AddCheatDialog::addModificationCheat(void)
{
    CoreCheat cheat;

    if (!this->getCheat(cheat))
    {
        return false;
    }

    if (!CoreAddModificationCheat(this->modificationInternalName.toStdString(), cheat))
    {
        QtMessageBox::Error(this, "CoreAddModificationCheat() Failed", QString::fromStdString(CoreGetError()));
        return false;
    }

    return true;
}

bool AddCheatDialog::updateCheat(void)
{
    CoreCheat cheat;

    if (!this->getCheat(cheat))
    {
        return false;
    }

    // we don't need to do anything,
    // when the user didn't change anything
    if (this->oldCheat == cheat)
    {
        return true;
    }

    if (!CoreUpdateCheat(this->file.toStdU32String(), this->oldCheat, cheat))
    {
        QtMessageBox::Error(this, "CoreUpdateCheat() Failed", QString::fromStdString(CoreGetError()));
        return false;
    }

    return true;
}

bool AddCheatDialog::updateModificationCheat(void)
{
    CoreCheat cheat;

    if (!this->getCheat(cheat))
    {
        return false;
    }

    if (this->oldCheat == cheat)
    {
        return true;
    }

    if (!CoreUpdateModificationCheat(this->modificationInternalName.toStdString(), this->oldCheat, cheat))
    {
        QtMessageBox::Error(this, "CoreUpdateModificationCheat() Failed", QString::fromStdString(CoreGetError()));
        return false;
    }

    return true;
}

void AddCheatDialog::accept(void)
{
    bool ret;

    if (!this->validate())
    {
        QtMessageBox::Error(this, "Validating Cheat Failed");
        return;
    }

    if (this->updateMode)
    {
        ret = this->modificationInternalName.isEmpty() ? this->updateCheat() : this->updateModificationCheat();
    }
    else
    {
        ret = this->modificationInternalName.isEmpty() ? this->addCheat() : this->addModificationCheat();
    }

    // don't close dialog on failure
    if (!ret)
    {
        return;
    }

    QDialog::accept();
}

void AddCheatDialog::on_nameLineEdit_textChanged(void)
{
    this->validateOkButton();
}

void AddCheatDialog::on_codeTextEdit_textChanged(void)
{
    this->validateOkButton();
}

void AddCheatDialog::on_optionsTextEdit_textChanged(void)
{
    this->validateOkButton();
}
