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

#pragma once

#include	<QObject>
#include	<QWidget>
#include	<QPoint>
#include	<QSize>
#include	<QSettings>
#include	<QList>
#include	<QMap>
#include	<QString>

enum class DockPosition {
	None	= 0,
	Below	= 1,
	Right	= 2,
	Left	= 3,
	Above	= 4
};

struct DockInfo {
	QWidget		*widget;
	QString		name;
	DockPosition	position;
	bool		docked;
};

class DockManager : public QObject {
Q_OBJECT
public:
		DockManager	(QWidget *mainWindow,
		                 QSettings *settings,
		                 QObject *parent = nullptr);
		~DockManager	();

	void	registerPanel	(QWidget *panel, const QString &name,
	                         DockPosition defaultPos = DockPosition::Below);
	void	unregisterPanel	(QWidget *panel);

	void	updatePositions	();
	void	saveLayout	();
	void	restoreLayout	();

	static const int SNAP_DISTANCE = 10;

protected:
	bool	eventFilter	(QObject *obj, QEvent *event) override;

private:
	void	snapToMain	(QWidget *panel, DockInfo &info);
	QPoint	dockedPosition	(const DockInfo &info);

	QWidget			*theMainWindow;
	QSettings		*theSettings;
	QList<DockInfo>		panels;
};
