/* Copyright (C) 2006 J.F.Dockes 
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifndef _SSEARCH_W_H_INCLUDED_
#define _SSEARCH_W_H_INCLUDED_
#include "autoconfig.h"

#include <string>
#include <vector>
#include <memory>

#include <QVariant>
#include <QWidget>
#include <QAbstractTableModel>
#include <QVariant>
#include <QPixmap>

#include "recoll.h"
#include "searchdata.h"

#include "ui_ssearchb.h"

struct SSearchDef;
class SSearch;
class QCompleter;
class QTimer;
class QShortcut;


class RclCompleterModel : public QAbstractTableModel {
    Q_OBJECT

public:
    RclCompleterModel(SSearch *parent = 0)
        : QAbstractTableModel((QWidget*)parent), m_parent(parent) {
        init();
    }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
public slots:
    virtual void onPartialWord(int, const QString&, const QString&);
private:
    void init();
    std::vector<std::pair<QString, int>> currentlist;
    int firstfromindex;
    QPixmap clockPixmap;
    QPixmap interroPixmap;
    SSearch *m_parent{nullptr};
};

class SSearch : public QWidget, public Ui::SSearchBase {
    Q_OBJECT

public:
    // The values MUST NOT change, there are assumptions about them in
    // different parts of the code
    enum SSearchType {SST_ANY = 0, SST_ALL = 1, SST_FNM = 2, SST_LANG = 3};

    SSearch(QWidget* parent = 0, const char * = 0)
        : QWidget(parent) {
        setupUi(this);
        init();
    }

    virtual void init();
    virtual void setAnyTermMode();
    virtual bool hasSearchString();
    // Return last performed search as XML text.
    virtual std::string asXML();
    // Restore ssearch UI from saved search
    virtual bool fromXML(const SSearchDef& fxml);
    virtual QString currentText();
    virtual bool eventFilter(QObject *target, QEvent *event);
    virtual void setupButtons();
                               
public slots:
    virtual void onSearchTypeChanged(int);
    virtual void setSearchString(const QString& text);
    virtual void startSimpleSearch();
    virtual void addTerm(QString);
    virtual void onWordReplace(const QString&, const QString&);
    virtual void takeFocus();
    // Forget current entry and any state (history)
    virtual void clearAll();
    virtual void setPrefs();
    virtual void onNewShortcuts();

private slots:
    virtual void searchTextChanged(const QString&);
    virtual void searchTextEdited(const QString&);
    virtual void onCompletionActivated(const QString&);
    virtual void restoreText();
    virtual void onHistoryClicked();
    virtual void onCompleterShown();
    
signals:
    void startSearch(std::shared_ptr<Rcl::SearchData>, bool);
    void setDescription(QString);
    void clearSearch();
    void partialWord(int, const QString& text, const QString &partial);
    void ssearchTypeChanged(int typ);
    
private:
    bool startSimpleSearch(const std::string& q, int maxexp = -1);
    bool checkExtIndexes(const std::vector<std::string>& dbs);

    RclCompleterModel *m_completermodel{nullptr};
    QCompleter *m_completer{nullptr};
    QShortcut *m_histsc{nullptr};
    /* We save multiword entries because the completer replaces them with
       the completion */
    QString m_savedEditText;
     /* Saved xml version of the search, as we start it */
    std::string m_xml;
};


#endif /* _SSEARCH_W_H_INCLUDED_ */
