/*
 * RemoteFileBrowserDialog.h - master-side UI for browsing remote files
 *
 * Copyright (c) 2026 VeyonFork contributors
 *
 * This file is part of VeyonFork - based on Veyon - https://veyon.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#pragma once

#include <QDialog>
#include <QFile>
#include <QUuid>

#include "ComputerControlInterface.h"

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class RemoteFileBrowserPlugin;

class RemoteFileBrowserDialog : public QDialog
{
	Q_OBJECT
public:
	RemoteFileBrowserDialog( RemoteFileBrowserPlugin* plugin,
							 ComputerControlInterface::Pointer computer,
							 QWidget* parent = nullptr );
	~RemoteFileBrowserDialog() override;

	// called by the plugin when replies arrive from the remote computer
	void setDrives( const QVariantList& drives );
	void setDirectoryListing( const QString& path, const QVariantList& entries, const QString& error );
	void downloadStarted( QUuid transferId, qint64 fileSize, const QString& error );
	void downloadDataReceived( QUuid transferId, const QByteArray& data );
	void downloadFinished( QUuid transferId );

private:
	void navigateTo( const QString& path );
	void navigateUp();
	void refresh();
	void showDrives();
	void onItemDoubleClicked( QTreeWidgetItem* item, int column );
	void downloadSelected();
	void resetDownload();

	static QString formatSize( qint64 bytes );

	RemoteFileBrowserPlugin* m_plugin{nullptr};
	ComputerControlInterface::Pointer m_computer{};

	QLineEdit* m_pathEdit{nullptr};
	QTreeWidget* m_tree{nullptr};
	QPushButton* m_downloadButton{nullptr};
	QProgressBar* m_progress{nullptr};
	QLabel* m_status{nullptr};

	QString m_currentPath;

	QUuid m_transferId;
	QFile m_localFile;
	qint64 m_expectedSize{0};
	qint64 m_receivedBytes{0};
};
