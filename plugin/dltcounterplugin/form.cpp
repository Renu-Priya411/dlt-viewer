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
 * \file form.cpp
 * For further information see http://www.covesa.global/.
 * @licence end@
 */

#include "form.h"
#include "ui_form.h"
#include "dltcounterplugin.h"

#include <qfiledialog.h>

using namespace DltCounter;

Form::Form(DltCounterPlugin *_plugin,QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Form)
{
    ui->setupUi(this);
    plugin = _plugin;
}

Form::~Form()
{
    delete ui;
}

void Form::on_CounterpushButton_clicked()
{
    qDebug() << "Counter button clicked";
    plugin->dltControl->CounterPluginCall();
}

void Form::on_ExportpushButton_clicked()
{
    qDebug() << "Export Button Clicked";
    plugin->dltControl->ExportPluginCall();
}
