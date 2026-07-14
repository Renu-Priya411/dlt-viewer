/**
 * @licence app begin@
 * Copyright (C) 2011-2012  BMW AG
 *
 * This file is part of COVESA Project Dlt Viewer.
 *
 * Contributions are licensed to the COVESA Alliance under one or more
 * Contribution License Agreements.
 *
 * \copyright
 * This Source Code Form is subject to the terms of the
 * Mozilla Public License, v. 2.0. If a  copy of the MPL was not distributed with
 * this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * \file searchdialog.cpp
 * For further information see http://www.covesa.global/.
 * @licence end@
 */

#include "searchdialog.h"
#include "ui_searchdialog.h"
#include "decodemanager.h"
#include "qdltoptmanager.h"
#include "tablemodel.h"

#include <dltmessagematcher.h>
#include <QtConcurrent/QtConcurrent>

#include <QApplication>
#include <QMessageBox>
#include <QPixmap>
#include <QSettings>
#include <QSignalBlocker>
#include <QColorDialog>
#include <QAction>
#include <QDebug>

SearchDialog::SearchDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SearchDialog)
{
    ui->setupUi(this);

    regexpCheckBox = ui->checkBoxRegExp;
    match = false;
    startLine = -1;

    lineEdits.append(ui->lineEditSearch);
    table = nullptr;

    // at start we want to know if single step search or "fill search table mode" is active !
    bool checked = QDltSettingsManager::getInstance()->value("other/search/checkBoxSearchIndex", bool(true)).toBool();
    ui->checkBoxFindAll->setChecked(checked);

    checked = QDltSettingsManager::getInstance()->value("other/search/checkBoxHeader", bool(true)).toBool();
    ui->checkBoxHeader->setChecked(checked);

    checked = QDltSettingsManager::getInstance()->value("other/search/checkBoxCasesensitive", bool(true)).toBool();
    ui->checkBoxCaseSensitive->setChecked(checked);

    checked = QDltSettingsManager::getInstance()->value("other/search/checkBoxRegEx", bool(true)).toBool();
    ui->checkBoxRegExp->setChecked(checked);

    ui->stackedWidgetRange->setCurrentIndex(0); // default Timestamp range
    connect(ui->radioTimestamp, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked)
            ui->stackedWidgetRange->setCurrentIndex(0);
    });
    connect(ui->radioTime, &QRadioButton::toggled, this, [this] (bool checked) {
        if (checked) {
            // switch from timestamp range to time range requires time range reset
            m_timeRangeResetNeeded = true;
            ui->stackedWidgetRange->setCurrentIndex(1);
        }
    });
    // user interaction with time range edits sets need for reset to false
    connect(ui->dateTimeStart, &QDateTimeEdit::dateTimeChanged, this, [this]() {
        m_timeRangeResetNeeded = false;
    });
    connect(ui->dateTimeEnd, &QDateTimeEdit::dateTimeChanged, this, [this]() {
        m_timeRangeResetNeeded = false;
    });

    // OK button triggers find next
    connect(this, &SearchDialog::accepted, this, &SearchDialog::findNextClicked);

    // Find-All batch results are emitted from the worker thread; the queued connection
    // ensures addToSearchIndexBatch runs on the UI thread.
    connect(this, &SearchDialog::findAllBatchReady, this,
            [this](QList<unsigned long> batch) {
                addToSearchIndexBatch(batch);
                maybeEmitFindAllRefresh();
            }, Qt::QueuedConnection);

    fSilentMode = !QDltOptManager::getInstance()->issilentMode();

    updateColorbutton();
}

SearchDialog::~SearchDialog()
{
    // Cancel and join any in-progress Find-All before releasing resources.
    isSearchCancelled.store(true, std::memory_order_relaxed);
    if (m_findAllFuture.isRunning())
        m_findAllFuture.waitForFinished();
    clearCacheHistory();
    delete ui;
}

void SearchDialog::selectText() {
    ui->lineEditSearch->setFocus();
    ui->lineEditSearch->selectAll();
}

void SearchDialog::setHeader(bool header) { ui->checkBoxHeader->setCheckState(header?Qt::Checked:Qt::Unchecked);}
void SearchDialog::setPayload(bool payload) { ui->checkBoxPayload->setCheckState(payload?Qt::Checked:Qt::Unchecked);}
void SearchDialog::setCaseSensitive(bool caseSensitive) { ui->checkBoxCaseSensitive->setCheckState(caseSensitive?Qt::Checked:Qt::Unchecked);}
void SearchDialog::setRegExp(bool regExp) { ui->checkBoxRegExp->setCheckState(regExp?Qt::Checked:Qt::Unchecked);}
void SearchDialog::setNextClicked(bool next){nextClicked = next;}
void SearchDialog::setMatch(bool matched){match=matched;}

void SearchDialog::setTimeRange(const QDateTime& min, const QDateTime& max) {
    ui->dateTimeStart->setDateTimeRange(min, max);
    ui->dateTimeEnd->setDateTimeRange(min, max);
    ui->dateTimeStart->setDateTime(min);
    ui->dateTimeEnd->setDateTime(max);
}

bool SearchDialog::needTimeRangeReset() const { return m_timeRangeResetNeeded; }

void SearchDialog::appendLineEdit(QLineEdit *lineEdit){ lineEdits.append(lineEdit);}

QString SearchDialog::getText() { return ui->lineEditSearch->text(); }

void SearchDialog::abortSearch()
{
    isSearchCancelled.store(true, std::memory_order_relaxed);
}

void SearchDialog::reportProgress(int progress)
{
    emit searchProgressValueChanged(progress);
}

bool SearchDialog::getHeader()
{
    return (ui->checkBoxHeader->checkState() == Qt::Checked);
}

bool SearchDialog::getPayload()
{
    return (ui->checkBoxPayload->checkState() == Qt::Checked);
}

bool SearchDialog::getRegExp()
{
    return (ui->checkBoxRegExp->checkState() == Qt::Checked);
}

bool SearchDialog::getNextClicked(){return nextClicked;}

QString SearchDialog::getApIDText(){ return ui->lineEditApld->text();}
QString SearchDialog::getCtIDText(){ return ui->lineEditCtid->text();}

QString SearchDialog::getTimeStampStart()
{
    //qDebug() << "content of start time" << ui->timeStartlineEdit->text()<< __LINE__;
    return ui->lineEditTimestampStart->text();
}

QString SearchDialog::getTimeStampEnd()
{
    //qDebug() << "content of end time" << ui->timeEndlineEdit->text() << __LINE__;
    return ui->lineEditTimestampEnd->text();
}

bool SearchDialog::getCaseSensitive()
{
    //qDebug() << "getCaseSensitive is" << ui->checkBoxCaseSensitive->checkState() << __LINE__;
    return (ui->checkBoxCaseSensitive->checkState() == Qt::Checked);
}

bool SearchDialog::searchtoIndex()
{
    //qDebug() << "searchtoIndex is" << ui->checkBoxSearchIndex->checkState() << __LINE__;
    return (ui->checkBoxFindAll->checkState() == Qt::Checked);
}


bool SearchDialog::getSearchFromBeginning()
{
    return (ui->radioButtonPosBeginning->isChecked());
}

void SearchDialog::setStartLine(long int start)
{
  startLine=start;
}

void SearchDialog::setSearchColour(QLineEdit *lineEdit,int result)
{
    QPalette palette = lineEdit->palette();
    QColor text0 = QColor(255,255,255);
    QColor text1 = QColor(0,0,0);
    QColor background0 = QColor(255,102,102);
    QColor background1 = QColor(255,255,255);

    if (QDltSettingsManager::UI_Colour::UI_Dark == QDltSettingsManager::getInstance()->uiColour)
    {
        background1 = QColor(31,31,31);
        text1 = QColor(255,255,255);
    }

    switch(result){
    case 0:
        palette.setColor(QPalette::Text,text0);
        lineEdit->setPalette(palette);
        palette.setColor(QPalette::Base,background0);
        lineEdit->setPalette(palette);
        break;
    case 1:
        palette.setColor(QPalette::Text,text1);
        lineEdit->setPalette(palette);
        palette.setColor(QPalette::Base,background1);
        lineEdit->setPalette(palette);
        break;
    }
}

void SearchDialog::focusRow(long int searchLine)
{
    TableModel *model = qobject_cast<TableModel *>(table->model());
    if(!model || !table)
    {
        return;
    }

    if(searchLine < 0 || searchLine >= model->rowCount())
    {
        // Clear marker state without trying to navigate to an invalid model index.
        model->setMarker(-1, highlightColor);
        model->setLastSearchIndex(-1);
        if(table->selectionModel())
        {
            table->selectionModel()->clear();
        }
        table->viewport()->update();
        return;
    }

    QModelIndex idx = model->index(searchLine, 0, QModelIndex());
    //qDebug() << "Focus row in message table window" << searchLine << __FILE__ << __LINE__;

    table->scrollTo(idx, QAbstractItemView::EnsureVisible);
    table->scrollTo(idx, QAbstractItemView::PositionAtCenter);

    model->setMarker(searchLine, highlightColor);

    model->setLastSearchIndex(searchLine);
    if(table->selectionModel())
    {
        table->selectionModel()->clear();
    }
    table->viewport()->update();
}

int SearchDialog::find()
{
    // Cancel and join any previous async Find-All before starting a new search.
    if (m_findAllFuture.isRunning()) {
        isSearchCancelled.store(true, std::memory_order_relaxed);
        m_findAllFuture.waitForFinished();
    }
    isSearchCancelled.store(false, std::memory_order_relaxed);

    emit addActionHistory();
    QRegularExpression searchTextRegExpression;
    is_TimeStampSearchSelected = false;
    long int searchBorder;
    long int lStartLine;

    emit searchProgressChanged(true);

    const int totalRows = file ? file->sizeFilter() : 0;

    if(totalRows == 0)
    {
        emit searchProgressChanged(false);
        return 0;
    }

   if( ( (match == true) || ( getSearchFromBeginning() == false )) && false == searchtoIndex() )
    {
        // single step search
        QModelIndexList list = table->selectionModel()->selection().indexes();
        if(list.count() > 0)
        {
            QModelIndex index;
            for(int num=0; num < list.count();num++)
            {
                index = list[num];
                if(index.column()==0)
                {
                    break;
                }
            }
            setStartLine(index.row());
        }
    }
   else
   {
      focusRow(-1);
   }


    if ( true == getSearchFromBeginning() )
    {
      //qDebug() << "Start from the beginning" << __LINE__;
    }
    else
    {
        if (table->selectionModel() != nullptr )
         {
          {
           if ( false == table->selectionModel()->selectedIndexes().isEmpty() )
            {
             if (table->selectionModel()->selectedIndexes().first().row() > -1)
              {
               lStartLine = table->selectionModel()->selectedIndexes().first().row();
               setStartLine( lStartLine );
              }
           }
          }
         }
    }

    searchBorder = startLine;
    if(searchBorder < 0 || searchtoIndex())
    {
        if(getNextClicked() || searchtoIndex())
        {
            searchBorder = totalRows - 1;
        }
        else
        {
            searchBorder = 0;
        }

    }

    if(getRegExp() == true)
    {
        searchTextRegExpression.setPattern(getText());
        if (searchTextRegExpression.isValid() == false)
        {
            if ( false == fSilentMode)
            {
            QMessageBox::warning(0, QString("Search"), QString("Invalid regular expression!"));
            }
            emit searchProgressChanged(false);
            return 1;
        }

        int options = QRegularExpression::DotMatchesEverythingOption;
        if (!getCaseSensitive())
            options |= QRegularExpression::CaseInsensitiveOption;
        searchTextRegExpression.setPatternOptions(static_cast<QRegularExpression::PatternOption>(options));
    }

    // check timestamp search pattern
    const QString timeStampStartTime = getTimeStampStart();
    const QString timeStampStopTime = getTimeStampEnd();

    if (!timeStampStartTime.isEmpty() && !timeStampStopTime.isEmpty())
    {
        dTimeStampStart = timeStampStartTime.toDouble();
        dTimeStampStop = timeStampStopTime.toDouble();
        if( (dTimeStampStop -  dTimeStampStart) >= 0 )
         {
         //qDebug() << "Timestamp search enabled" << dTimeStampStart << dTimeStampStop << __LINE__;
         is_TimeStampSearchSelected = true;
         }
        else
        {
         qDebug() << "Invalid timestamp range" << dTimeStampStart << dTimeStampStop << __LINE__;
         is_TimeStampSearchSelected = false;
         if ( false == fSilentMode)
         {
         QMessageBox::warning(0, QString("Search"), QString("Invalid timestamp range !"));
         }
         emit searchProgressChanged(false);
         return 1;
        }
    }

    // check APID and CTID search
    stApid = getApIDText();
    stCtid = getCtIDText();
    if( stApid.size() > 0 || stCtid.size() > 0 ) // so we need to consider what is given here
    {
        if( stApid.size() > 4 || stCtid.size() > 4 )
        {
            qDebug() << "Given APID or CTID exceeds limit !";
            if ( false == fSilentMode)
            {
            QMessageBox::warning(0, QString("Search"), QString("Given APID or CTID exceeds limit !"));
            }
            emit searchProgressChanged(false);
            return 2;
        }
    }

    if (searchtoIndex() == true)
    {
        m_findAllUiUpdateTimer.restart();
        m_findAllLastUiUpdateMs = 0;
        m_findAllAddedSinceLastUiUpdate = 0;

        // Build the matcher on the UI thread before handing off to the worker.
        const Qt::CaseSensitivity caseSens = getCaseSensitive() ? Qt::CaseSensitive : Qt::CaseInsensitive;
        DltMessageMatcher capturedMatcher;
        capturedMatcher.setCaseSentivity(caseSens);
        capturedMatcher.setSearchAppId(stApid);
        capturedMatcher.setSearchCtxId(stCtid);
        if (ui->radioTimestamp->isChecked() && is_TimeStampSearchSelected)
            capturedMatcher.setTimestampRange(dTimeStampStart, dTimeStampStop);
        if (ui->radioTime->isChecked())
            capturedMatcher.setTimeRange(ui->dateTimeStart->dateTime(), ui->dateTimeEnd->dateTime());
        const bool msgIdEnabled = QDltSettingsManager::getInstance()->value("startup/showMsgId", true).toBool();
        const QString msgIdFormat = QDltSettingsManager::getInstance()->value("startup/msgIdFormat", "0x%x").toString();
        if (msgIdEnabled)
            capturedMatcher.setMessageIdFormat(msgIdFormat);
        capturedMatcher.setHeaderSearchEnabled(getHeader());
        capturedMatcher.setPayloadSearchEnabled(getPayload());

        DltMessageMatcher::Pattern capturedPattern = getRegExp()
            ? DltMessageMatcher::Pattern(searchTextRegExpression)
            : DltMessageMatcher::Pattern(getText());

        m_searchtablemodel->clear_SearchResults();

        ++m_findAllGeneration;
        const int generation      = m_findAllGeneration;
        const long int capSLine   = startLine;
        const long int capSBorder = searchBorder;
        const int capTotalRows    = totalRows;

        m_findAllFuture = QtConcurrent::run(
            [this, generation, capSLine, capSBorder, capTotalRows,
             capturedPattern = std::move(capturedPattern),
             capturedMatcher = std::move(capturedMatcher)]() mutable {
                runFindAllWorker(capSLine, capSBorder, capTotalRows,
                                 std::move(capturedPattern), std::move(capturedMatcher));
                // Notify the UI thread when the worker is done.
                QMetaObject::invokeMethod(this, [this, generation]() {
                    onFindAllFinished(generation);
                }, Qt::QueuedConnection);
            });

        return -1;  // async; UI update and colour setting happen in onFindAllFinished
    }

    findMessages(startLine,searchBorder,searchTextRegExpression);

    emit searchProgressChanged(false);

    if(match == true )
    {
        return 1;
    }
    setStartLine(-1); // so we do not miss index 0 any longer ...
    return 0;
}

void SearchDialog::findMessages(long int searchLine, long int searchBorder, QRegularExpression &searchTextRegExp)
{

    QDltMsg msg;
    QByteArray buf;
    int ctr = 0;
    Qt::CaseSensitivity is_Case_Sensitive = Qt::CaseInsensitive;

    if(getCaseSensitive() == true)
    {
        is_Case_Sensitive = Qt::CaseSensitive;
    }

    m_searchtablemodel->clear_SearchResults();

    const int totalRows = file ? file->sizeFilter() : 0;
    if(totalRows <= 0)
    {
        match = false;
        return;
    }

    bool msgIdEnabled=QDltSettingsManager::getInstance()->value("startup/showMsgId", true).toBool();
    QString msgIdFormat=QDltSettingsManager::getInstance()->value("startup/msgIdFormat", "0x%x").toString();

    DltMessageMatcher matcher;
    matcher.setCaseSentivity(is_Case_Sensitive);
    matcher.setSearchAppId(stApid);
    matcher.setSearchCtxId(stCtid);

    if (ui->radioTimestamp->isChecked() && is_TimeStampSearchSelected) {
        matcher.setTimestampRange(dTimeStampStart, dTimeStampStop);
    }
    if (ui->radioTime->isChecked()) {
        matcher.setTimeRange(ui->dateTimeStart->dateTime(), ui->dateTimeEnd->dateTime());
    }

    if (msgIdEnabled) {
        matcher.setMessageIdFormat(msgIdFormat);
    }
    const bool headerEnabled = getHeader();
    const bool payloadEnabled = getPayload();
    matcher.setHeaderSearchEnabled(headerEnabled);
    matcher.setPayloadSearchEnabled(payloadEnabled);

    const bool decodeEnabledSetting = QDltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool();
    const bool shouldDecodeForSearch = decodeEnabledSetting && payloadEnabled;
    const DltMessageMatcher::Pattern searchPattern = getRegExp()
        ? DltMessageMatcher::Pattern(searchTextRegExp)
        : DltMessageMatcher::Pattern(getText());

    do
    {
        ctr++; // for file progress indication

        if(getNextClicked())
        {
            searchLine++;
            if(searchLine >= totalRows)
            {
                searchLine = 0;
            }
        }
        else // go back
        {
            searchLine--;
            if(searchLine <= -1)
            {
                searchLine = totalRows-1;
            }
        }

        // Update progress every 0.5%
        if(searchLine%1000 == 0)
        {
            QApplication::processEvents();
            if (isSearchCancelled.load(std::memory_order_relaxed)) {
                break;
            }
            emit searchProgressValueChanged(static_cast<int>(ctr * 100.0 / totalRows));
        }

        /* get the message with the selected item id */
        const int msgIndex = file->getMsgFilterPos(searchLine);
        buf = file->getMsg(msgIndex);
        if(!msg.setMsg(buf))
        {
            continue;
        }
        msg.setIndex(msgIndex);

        if(!matcher.matchMeta(msg))
        {
            match = false;
            continue;
        }

        bool matchFound = false;
        if(headerEnabled)
        {
            matchFound = matcher.matchHeader(msg, searchPattern);
        }

        if(!matchFound && payloadEnabled)
        {
            matchFound = matcher.matchPayload(msg, searchPattern);
        }

        if(!matchFound && shouldDecodeForSearch)
        {
            DecodeManager::instance().decode(pluginManager, msg, decodeEnabledSetting, fSilentMode);
            matchFound = matcher.matchPayload(msg, searchPattern);
        }

        if (!matchFound)
        {
            match = false;
            continue;
        }

        if (foundLine(searchLine, static_cast<unsigned long>(msgIndex)))
            break;
    }
    while( searchBorder != searchLine );
}

void SearchDialog::runFindAllWorker(long int searchLine, long int searchBorder, int totalRows,
                                    DltMessageMatcher::Pattern pattern, DltMessageMatcher matcher)
{
    // This method runs on a thread-pool thread.
    // It does NOT call decode (no mutex contention) and does NOT call QApplication::processEvents().
    QDltMsg msg;
    QList<unsigned long> batch;
    batch.reserve(512);
    int ctr = 0;

    // Raw-bytes prefilter setup.
    // For plain-text payload-only searches, the search term appears verbatim (UTF-8) in the
    // raw DLT wire bytes for string-type arguments.  Checking the raw bytes before calling the
    // expensive setMsg() + toStringPayload() avoids ~60 µs of parsing per non-matching message.
    // Note: numeric DLT arguments (int, float) are stored as binary, so a search for "42" will
    // not be prefiltered — those messages fall through to full parsing as before.
    QByteArray prefilterBytes;
    QByteArray prefilterBytesLower;
    bool usePrefilter = false;
    if (!matcher.isHeaderSearchEnabled() &&
         matcher.isPayloadSearchEnabled() &&
         std::holds_alternative<QString>(pattern))
    {
        const QString& text = std::get<QString>(pattern);
        if (!text.isEmpty()) {
            prefilterBytes      = text.toUtf8();
            prefilterBytesLower = prefilterBytes.toLower();
            usePrefilter = true;
        }
    }
    const bool caseSensitive = (matcher.caseSensitivity() == Qt::CaseSensitive);

    do {
        ctr++;
        searchLine++;
        if (searchLine >= totalRows)
            searchLine = 0;

        if (ctr % 1000 == 0) {
            if (isSearchCancelled.load(std::memory_order_relaxed))
                break;
            emit searchProgressValueChanged(static_cast<int>(ctr * 100.0 / totalRows));
        }

        const int msgIndex = file->getMsgFilterPos(searchLine);
        const QByteArray buf = file->getMsg(msgIndex);

        // Apply raw-bytes prefilter before the expensive DLT parse.
        if (usePrefilter) {
            if (caseSensitive) {
                if (!buf.contains(prefilterBytes))
                    continue;
            } else {
                // toLower() + contains() is still ~20x cheaper than setMsg + toStringPayload.
                if (!buf.toLower().contains(prefilterBytesLower))
                    continue;
            }
        }

        if (!msg.setMsg(buf))
            continue;
        msg.setIndex(msgIndex);

        if (!matcher.matchMeta(msg))
            continue;

        const bool matchFound = matcher.matchHeader(msg, pattern) ||
                                matcher.matchPayload(msg, pattern);
        if (!matchFound)
            continue;

        batch.append(static_cast<unsigned long>(msgIndex));
        if (batch.size() >= 512) {
            emit findAllBatchReady(batch);
            batch.clear();
        }
    } while (searchBorder != searchLine &&
             !isSearchCancelled.load(std::memory_order_relaxed));

    if (!batch.isEmpty())
        emit findAllBatchReady(batch);
}

void SearchDialog::onFindAllFinished(int generation)
{
    // Guard against stale completions from a cancelled search.
    if (generation != m_findAllGeneration)
        return;

    cacheSearchHistory();
    match = (m_searchtablemodel && m_searchtablemodel->get_SearchResultListSize() > 0);
    maybeEmitFindAllRefresh(true);
    emit searchProgressChanged(false);

    const int result = match ? 1 : 0;
    for (int i = 0; i < lineEdits.size(); i++)
        setSearchColour(lineEdits.at(i), result);
}

bool SearchDialog::foundLine(long int searchLine, unsigned long msgIndex)
{
    match = true;

    if (searchtoIndex() == true)
    {
        addToSearchIndex(msgIndex);
        emit refreshedSearchIndex();
    }
    else
    {
        focusRow(searchLine); // focus the line ein message table view
        setStartLine(searchLine);
        //qDebug() << "Single line hit in  " << searchLine << __LINE__;
        return true;//found single result, and breaking here
    }
    return false;//don't break search here
}

void SearchDialog::findNextClicked()
{
    setNextClicked(true);

    const int result = find();
    if (result >= 0) {
        for(int i=0; i<lineEdits.size();i++)
            setSearchColour(lineEdits.at(i),result);
    }
    // result == -1 means async Find-All was launched; colour is set in onFindAllFinished.
}

void SearchDialog::findPreviousClicked()
{
    setNextClicked(false);

    const int result = find();
    if (result >= 0) {
        for(int i=0; i<lineEdits.size();i++)
            setSearchColour(lineEdits.at(i),result);
    }
    // result == -1 means async Find-All was launched; colour is set in onFindAllFinished.
}

void SearchDialog::on_lineEditSearch_textEdited(QString newText)
{
        {
            // block signal so that it does not trigger a setText back on lineEdits->at(0)!
            QSignalBlocker signalBlocker(lineEdits.at(1));
            lineEdits.at(1)->setText(newText);
        }
        for(int i=0; i<lineEdits.size();i++){
            if(lineEdits.at(0)->text().isEmpty())
                setSearchColour(lineEdits.at(i),1);
        }
}
void SearchDialog::textEditedFromToolbar(QString newText)
{
        {
            // block signal so that it does not trigger a setText back on lineEdits->at(1)!
            QSignalBlocker signalBlocker(lineEdits.at(0));
            lineEdits.at(0)->setText(newText);
        }
        for(int i=0; i<lineEdits.size();i++){
            if(lineEdits.at(0)->text().isEmpty())
                setSearchColour(lineEdits.at(i),1);
        }
}

void SearchDialog::on_buttonHighlightColor_clicked()
{
    QString color = QDltSettingsManager::getInstance()->value("other/searchResultColor", QString("#00AAFF")).toString();
    QColor oldColor(color);
    QColor newColor = QColorDialog::getColor(oldColor, this, "Pick color for Search Highlight");
    if(false == newColor.isValid())
    {
        // User cancelled
        return;
    }

    QDltSettingsManager::getInstance()->setValue("other/searchResultColor", newColor.name());
    updateColorbutton();
}

void SearchDialog::updateColorbutton()
{
    QString color = QDltSettingsManager::getInstance()->value("other/searchResultColor", QString("#00AAFF")).toString();
    QColor lhlColor(color);
    highlightColor = lhlColor;
    QPixmap px(12, 12);
    px.fill(highlightColor);
    ui->buttonHighlightColor->setIcon(px);
}


void SearchDialog::addToSearchIndex(unsigned long msgIndex)
{
    //qDebug() << "Add hit line to search table with msg index" << msgIndex << __LINE__;
    m_searchtablemodel->add_SearchResultEntry(msgIndex);
 }

void SearchDialog::addToSearchIndexBatch(const QList<unsigned long> &msgIndices)
{
    if(msgIndices.isEmpty())
    {
        return;
    }

    m_searchtablemodel->add_SearchResultEntries(msgIndices);
    m_findAllAddedSinceLastUiUpdate += msgIndices.size();
}

void SearchDialog::maybeEmitFindAllRefresh(bool force)
{
    if(!searchtoIndex())
    {
        return;
    }

    if(force)
    {
        m_findAllLastUiUpdateMs = m_findAllUiUpdateTimer.elapsed();
        m_findAllAddedSinceLastUiUpdate = 0;
        emit refreshedSearchIndex();
        return;
    }

    const qint64 nowMs = m_findAllUiUpdateTimer.elapsed();
    const bool timeToUpdate = (nowMs - m_findAllLastUiUpdateMs) >= 200;
    const bool enoughItems = m_findAllAddedSinceLastUiUpdate >= 1000;

    if(timeToUpdate || enoughItems)
    {
        m_findAllLastUiUpdateMs = nowMs;
        m_findAllAddedSinceLastUiUpdate = 0;
        emit refreshedSearchIndex();
    }
}

void SearchDialog::registerSearchTableModel(SearchTableModel *model)
{
    m_searchtablemodel = model;    
}


void SearchDialog::loadSearchHistory()
{
    // getting text of the action button clicked to load search history.
    QAction *action = qobject_cast<QAction *>(sender());
    QString text;
    if(action)
    {
        text = action->text();
    }

    // creating a local list to store the indexes related to the key retrieved from the cache.
    QList <unsigned long> tmp ;
    if(cachedHistoryKey.size() > 0)
    {
        tmp = cachedHistoryKey[text];

        //deleting the previous search list and adding the cached search obtained to the model.
        m_searchtablemodel->clear_SearchResults();
        m_searchtablemodel->add_SearchResultEntries(tmp);
    }
    emit refreshedSearchIndex();
}

void SearchDialog::cacheSearchHistory()
{
    // if it is a new search then add all the indexes of the search to a list(m_searchHistory).
    QString searchBoxText = getText();  
    m_searchHistory.append(m_searchtablemodel->m_searchResultList);
    cachedHistoryKey.insert(searchBoxText,m_searchHistory.last());    
}

void SearchDialog::clearCacheHistory()
{
    // obtaining the list of keys stored in cache
    cachedHistoryKey.clear();
}

void SearchDialog::saveSearchHistory(QStringList& searchHistory) {
    //To save the search history
    QSettings settings("MyApp", "SearchHistory");
    settings.beginWriteArray("history");
    int count = qMin(searchHistory.size(), 20);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        settings.setValue("entry", searchHistory.at(i));
    }
    settings.endArray();
}

void SearchDialog::loadSearchHistoryList(QStringList& searchHistory)
{
  //To retrive the search history once DLT Viewer restarts
    QSettings settings("MyApp", "SearchHistory");
    searchHistory.clear();
    int size = settings.beginReadArray("history");
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        searchHistory.append(settings.value("entry").toString());
    }
    settings.endArray();
}

void SearchDialog::on_checkBoxHeader_toggled(bool checked)
{
   QDltSettingsManager::getInstance()->setValue("other/search/checkBoxHeader", checked);
}

void SearchDialog::on_checkBoxFindAll_toggled(bool checked)
{
    QDltSettingsManager::getInstance()->setValue("other/search/checkBoxSearchIndex", checked);
    setStartLine(-1);
}

void SearchDialog::on_checkBoxCaseSensitive_toggled(bool checked)
{
    QDltSettingsManager::getInstance()->setValue("other/search/checkBoxCasesensitive", checked);
}

void SearchDialog::on_checkBoxRegExp_toggled(bool checked)
{
    QDltSettingsManager::getInstance()->setValue("other/search/checkBoxRegEx", checked);
}


