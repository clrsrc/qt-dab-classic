/*
 *    Copyright (C) 2025
 *    Qt-DAB Winamp Edition
 *
 *    This file is part of Qt-DAB
 *
 *    Qt-DAB is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 */

#include	"unified-timer-widget.h"
#include	"unified-timer-model.h"
#include	<QSettings>
#include	<QMessageBox>
#include	<QGridLayout>
#include	<QLineEdit>

	UnifiedTimerWidget::UnifiedTimerWidget (UnifiedTimerModel *model,
	                                        QSettings *settings,
	                                        QWidget *parent):
	                                        superFrame (parent),
	                                        theModel (model),
	                                        theSettings (settings) {
	setWindowTitle ("Timer");
	resize (500, 400);
	setupUi ();
}

	UnifiedTimerWidget::~UnifiedTimerWidget () {
}

void	UnifiedTimerWidget::setupUi () {
QVBoxLayout *mainLayout = new QVBoxLayout (this);

	// timer table
	timerTableView = new QTableView (this);
	timerTableView->setModel (theModel);
	timerTableView->setSelectionBehavior (QAbstractItemView::SelectRows);
	timerTableView->setSelectionMode (QAbstractItemView::SingleSelection);
	timerTableView->horizontalHeader ()->setStretchLastSection (true);
	timerTableView->setAlternatingRowColors (true);
	timerTableView->verticalHeader ()->hide ();
	timerTableView->setColumnWidth (UnifiedTimerModel::COL_TYPE, 110);
	timerTableView->setColumnWidth (UnifiedTimerModel::COL_SERVICE, 120);
	timerTableView->setColumnWidth (UnifiedTimerModel::COL_DATETIME, 130);
	timerTableView->setColumnWidth (UnifiedTimerModel::COL_DURATION, 60);
	timerTableView->setColumnWidth (UnifiedTimerModel::COL_DELETE, 30);
	connect (timerTableView, &QTableView::clicked,
	         this, &UnifiedTimerWidget::handleDoubleClick);
	mainLayout->addWidget (timerTableView);

	// add timer form
	QGroupBox *addGroup = new QGroupBox (tr ("Timer hinzufuegen"), this);
	QGridLayout *formLayout = new QGridLayout (addGroup);

	formLayout->addWidget (new QLabel (tr ("Typ:"), this), 0, 0);
	typeSelector = new QComboBox (this);
	typeSelector->addItem ("Umschalten",	(int)TimerType::ManualSwitch);
	typeSelector->addItem ("Aufnahme",	(int)TimerType::ManualRecord);
	formLayout->addWidget (typeSelector, 0, 1);

	formLayout->addWidget (new QLabel (tr ("Service:"), this), 0, 2);
	serviceCombo = new QComboBox (this);
	serviceCombo->setEditable (true);
	serviceCombo->setInsertPolicy (QComboBox::NoInsert);
	serviceCombo->lineEdit ()->setPlaceholderText ("z.B. Dlf");
	formLayout->addWidget (serviceCombo, 0, 3);

	formLayout->addWidget (new QLabel (tr ("Start:"), this), 1, 0);
	startTimeEdit = new QDateTimeEdit (
	   QDateTime::currentDateTime ().addSecs (3600), this);
	startTimeEdit->setDisplayFormat ("dd.MM.yyyy HH:mm");
	startTimeEdit->setCalendarPopup (true);
	formLayout->addWidget (startTimeEdit, 1, 1);

	formLayout->addWidget (new QLabel (tr ("Dauer (min):"), this), 1, 2);
	durationSpin = new QSpinBox (this);
	durationSpin->setRange (0, 1440);
	durationSpin->setValue (0);
	durationSpin->setSpecialValueText ("-");
	formLayout->addWidget (durationSpin, 1, 3);

	formLayout->addWidget (new QLabel (tr ("Ende:"), this), 2, 0);
	endTimeLabel = new QLabel ("-", this);
	formLayout->addWidget (endTimeLabel, 2, 1);

	addButton = new QPushButton (tr ("Hinzufuegen"), this);
	connect (addButton, &QPushButton::clicked,
	         this, &UnifiedTimerWidget::handleAddTimer);
	formLayout->addWidget (addButton, 2, 3);

	mainLayout->addWidget (addGroup);

	// update end time when start or duration changes
	connect (startTimeEdit, &QDateTimeEdit::dateTimeChanged,
	         this, &UnifiedTimerWidget::updateEndTime);
	connect (durationSpin, QOverload<int>::of (&QSpinBox::valueChanged),
	         this, [this] (int) { updateEndTime (); });

	// bottom buttons
	QHBoxLayout *buttonLayout = new QHBoxLayout ();

	clearButton = new QPushButton (tr ("Alle loeschen"), this);
	connect (clearButton, &QPushButton::clicked,
	         this, &UnifiedTimerWidget::handleClearAll);
	buttonLayout->addWidget (clearButton);

	buttonLayout->addStretch ();
	mainLayout->addLayout (buttonLayout);

	updateEndTime ();
}

void	UnifiedTimerWidget::setServiceList (const QStringList &services) {
	QString current = serviceCombo->currentText ();
	serviceCombo->clear ();
	serviceCombo->addItems (services);
	if (!current.isEmpty ()) {
	   int idx = serviceCombo->findText (current);
	   if (idx >= 0)
	      serviceCombo->setCurrentIndex (idx);
	   else
	      serviceCombo->setEditText (current);
	}
}

void	UnifiedTimerWidget::updateEndTime () {
	int duration = durationSpin->value ();
	if (duration <= 0) {
	   endTimeLabel->setText ("-");
	   return;
	}
	QDateTime end = startTimeEdit->dateTime ().addSecs (duration * 60);
	endTimeLabel->setText (end.toString ("dd.MM.yyyy HH:mm"));
}

void	UnifiedTimerWidget::handleAddTimer () {
	QString service = serviceCombo->currentText ().trimmed ();
	if (service.isEmpty ()) {
	   QMessageBox::warning (this, tr ("Fehler"),
	                         tr ("Bitte einen Service waehlen."));
	   return;
	}

	TimerType type = (TimerType)typeSelector->currentData ().toInt ();
	QDateTime start = startTimeEdit->dateTime ();
	int duration = durationSpin->value ();

	if (start <= QDateTime::currentDateTime ()) {
	   QMessageBox::warning (this, tr ("Fehler"),
	                         tr ("Startzeit muss in der Zukunft liegen."));
	   return;
	}

	// conflict check
	int conflict = theModel->findConflict (start, duration);
	if (conflict >= 0) {
	   const TimerEntry &existing = theModel->timerAt (conflict);
	   QString msg = tr ("Timer-Konflikt!\n\n"
	      "Bestehender Timer:\n  %1 um %2\n\n"
	      "Neuer Timer:\n  %3 um %4\n\n"
	      "Bestehenden Timer ersetzen?")
	      .arg (existing.programTitle.isEmpty () ?
	            existing.serviceName : existing.programTitle)
	      .arg (existing.startTime.toString ("dd.MM. HH:mm"))
	      .arg (service)
	      .arg (start.toString ("dd.MM. HH:mm"));
	   int ret = QMessageBox::question (this, tr ("Timer-Konflikt"),
	                msg, QMessageBox::Yes | QMessageBox::No);
	   if (ret == QMessageBox::No)
	      return;
	   theModel->removeTimer (conflict);
	}

	theModel->addTimer (type, service, "", start, duration);

	// reset form
	serviceCombo->clearEditText ();
	startTimeEdit->setDateTime (QDateTime::currentDateTime ().addSecs (3600));
	durationSpin->setValue (0);
}

void	UnifiedTimerWidget::handleClearAll () {
	if (theModel->timerCount () == 0)
	   return;

	int ret = QMessageBox::question (this, tr ("Alle loeschen"),
	            tr ("Wirklich alle Timer loeschen?"),
	            QMessageBox::Yes | QMessageBox::No);
	if (ret == QMessageBox::Yes)
	   theModel->clearAll ();
}

void	UnifiedTimerWidget::handleDoubleClick (const QModelIndex &index) {
	if (!index.isValid ())
	   return;
	if (index.column () == UnifiedTimerModel::COL_DELETE)
	   theModel->removeTimer (index.row ());
}
