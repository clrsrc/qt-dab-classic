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

#include	"dock-manager.h"
#include	"settingNames.h"
#include	<QEvent>
#include	<QMoveEvent>
#include	<cmath>

	DockManager::DockManager (QWidget *mainWindow,
	                           QSettings *settings,
	                           QObject *parent):
	                           QObject (parent),
	                           theMainWindow (mainWindow),
	                           theSettings (settings) {
	theMainWindow->installEventFilter (this);
}

	DockManager::~DockManager () {
	saveLayout ();
}

void	DockManager::registerPanel (QWidget *panel, const QString &name,
	                             DockPosition defaultPos) {
	DockInfo info;
	info.widget	= panel;
	info.name	= name;
	info.position	= defaultPos;
	info.docked	= true;

	panels.append (info);
	panel->installEventFilter (this);
}

void	DockManager::unregisterPanel (QWidget *panel) {
	for (int i = 0; i < panels.size (); i++) {
	   if (panels [i].widget == panel) {
	      panel->removeEventFilter (this);
	      panels.removeAt (i);
	      return;
	   }
	}
}

bool	DockManager::eventFilter (QObject *obj, QEvent *event) {
	if (event->type () == QEvent::Move) {
	   // main window moved -> reposition all docked panels
	   if (obj == theMainWindow) {
	      updatePositions ();
	      return false;
	   }

	   // a panel moved -> check if it should snap/undock
	   for (auto &info : panels) {
	      if (obj == info.widget) {
	         QPoint mainPos = theMainWindow->pos ();
	         QPoint panelPos = info.widget->pos ();
	         QPoint expectedPos = dockedPosition (info);

	         int dist = std::abs (panelPos.x () - expectedPos.x ()) +
	                    std::abs (panelPos.y () - expectedPos.y ());

	         if (dist <= SNAP_DISTANCE && !info.docked) {
	            info.docked = true;
	            info.widget->move (expectedPos);
	         }
	         else if (dist > SNAP_DISTANCE * 3 && info.docked) {
	            info.docked = false;
	         }
	         break;
	      }
	   }
	}

	return QObject::eventFilter (obj, event);
}

void	DockManager::updatePositions () {
	for (auto &info : panels) {
	   if (info.docked && info.widget->isVisible ()) {
	      info.widget->move (dockedPosition (info));
	   }
	}
}

QPoint	DockManager::dockedPosition (const DockInfo &info) {
	QPoint mainPos = theMainWindow->pos ();
	QSize mainSize = theMainWindow->size ();

	switch (info.position) {
	   case DockPosition::Below:
	      return QPoint (mainPos.x (),
	                     mainPos.y () + mainSize.height ());
	   case DockPosition::Right:
	      return QPoint (mainPos.x () + mainSize.width (),
	                     mainPos.y ());
	   case DockPosition::Left:
	      return QPoint (mainPos.x () - info.widget->width (),
	                     mainPos.y ());
	   case DockPosition::Above:
	      return QPoint (mainPos.x (),
	                     mainPos.y () - info.widget->height ());
	   case DockPosition::None:
	   default:
	      return info.widget->pos ();
	}
}

void	DockManager::saveLayout () {
	theSettings->beginGroup (WINAMP_DOCK);
	for (const auto &info : panels) {
	   theSettings->setValue (info.name + "_docked", info.docked);
	   theSettings->setValue (info.name + "_position",
	                          (int)info.position);
	   theSettings->setValue (info.name + "_visible",
	                          info.widget->isVisible ());
	   theSettings->setValue (info.name + "_pos",
	                          info.widget->pos ());
	}
	theSettings->endGroup ();
}

void	DockManager::restoreLayout () {
	theSettings->beginGroup (WINAMP_DOCK);
	for (auto &info : panels) {
	   info.docked = theSettings->value (
	      info.name + "_docked", true).toBool ();
	   info.position = (DockPosition)theSettings->value (
	      info.name + "_position", (int)info.position).toInt ();
	   bool visible = theSettings->value (
	      info.name + "_visible", false).toBool ();

	   if (visible) {
	      info.widget->show ();
	      if (info.docked)
	         info.widget->move (dockedPosition (info));
	      else {
	         QPoint pos = theSettings->value (
	            info.name + "_pos", QPoint (0, 0)).toPoint ();
	         info.widget->move (pos);
	      }
	   }
	}
	theSettings->endGroup ();
}
