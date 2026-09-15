/*
 * RemoteFileBrowserDialog.cpp - master-side UI for browsing remote files
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

#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "RemoteFileBrowserDialog.h"
#include "RemoteFileBrowserPlugin.h"

namespace {
constexpr int RoleIsDir = Qt::UserRole + 1;
constexpr int RoleName = Qt::UserRole + 2;
}


RemoteFileBrowserDialog::RemoteFileBrowserDialog( RemoteFileBrowserPlugin* plugin,
												  ComputerControlInterface::Pointer computer,
												  QWidget* parent ) :
	QDialog( parent ),
	m_plugin( plugin ),
	m_computer( std::move( computer ) )
{
	setWindowTitle( tr( "Remote file browser - %1" ).arg( m_computer ? m_computer->computer().displayName()
																	 : QString() ) );
	resize( 780, 520 );

	auto layout = new QVBoxLayout( this );

	// --- navigation bar ---
	auto navLayout = new QHBoxLayout;
	auto drivesButton = new QPushButton( tr( "Drives" ), this );
	auto upButton = new QPushButton( tr( "Up" ), this );
	m_pathEdit = new QLineEdit( this );
	auto goButton = new QPushButton( tr( "Go" ), this );
	auto refreshButton = new QPushButton( tr( "Refresh" ), this );

	navLayout->addWidget( drivesButton );
	navLayout->addWidget( upButton );
	navLayout->addWidget( m_pathEdit, 1 );
	navLayout->addWidget( goButton );
	navLayout->addWidget( refreshButton );
	layout->addLayout( navLayout );

	// --- file list ---
	m_tree = new QTreeWidget( this );
	m_tree->setColumnCount( 3 );
	m_tree->setHeaderLabels( { tr( "Name" ), tr( "Size" ), tr( "Modified" ) } );
	m_tree->setRootIsDecorated( false );
	m_tree->setSortingEnabled( false );
	m_tree->setSelectionBehavior( QAbstractItemView::SelectRows );
	m_tree->header()->setStretchLastSection( false );
	m_tree->header()->setSectionResizeMode( 0, QHeaderView::Stretch );
	layout->addWidget( m_tree, 1 );

	// --- transfer area ---
	m_progress = new QProgressBar( this );
	m_progress->setVisible( false );
	layout->addWidget( m_progress );

	auto bottomLayout = new QHBoxLayout;
	m_status = new QLabel( tr( "Ready." ), this );
	m_downloadButton = new QPushButton( tr( "Download..." ), this );
	m_downloadButton->setEnabled( false );
	auto closeButton = new QPushButton( tr( "Close" ), this );

	bottomLayout->addWidget( m_status, 1 );
	bottomLayout->addWidget( m_downloadButton );
	bottomLayout->addWidget( closeButton );
	layout->addLayout( bottomLayout );

	// --- wiring ---
	connect( drivesButton, &QPushButton::clicked, this, &RemoteFileBrowserDialog::showDrives );
	connect( upButton, &QPushButton::clicked, this, &RemoteFileBrowserDialog::navigateUp );
	connect( refreshButton, &QPushButton::clicked, this, &RemoteFileBrowserDialog::refresh );
	connect( goButton, &QPushButton::clicked, this, [this]() { navigateTo( m_pathEdit->text() ); } );
	connect( m_pathEdit, &QLineEdit::returnPressed, this, [this]() { navigateTo( m_pathEdit->text() ); } );
	connect( m_tree, &QTreeWidget::itemDoubleClicked, this, &RemoteFileBrowserDialog::onItemDoubleClicked );
	connect( m_tree, &QTreeWidget::itemSelectionChanged, this, [this]() {
		const auto items = m_tree->selectedItems();
		m_downloadButton->setEnabled( items.isEmpty() == false &&
									  items.first()->data( 0, RoleIsDir ).toBool() == false );
	} );
	connect( m_downloadButton, &QPushButton::clicked, this, &RemoteFileBrowserDialog::downloadSelected );
	connect( closeButton, &QPushButton::clicked, this, &QDialog::accept );

	showDrives();
}



RemoteFileBrowserDialog::~RemoteFileBrowserDialog()
{
	if( m_transferId.isNull() == false && m_plugin && m_computer )
	{
		m_plugin->cancelDownload( m_computer, m_transferId );
	}
	resetDownload();
}



void RemoteFileBrowserDialog::showDrives()
{
	m_currentPath.clear();
	m_pathEdit->clear();
	m_status->setText( tr( "Loading drives..." ) );
	m_plugin->requestDrives( m_computer );
}



void RemoteFileBrowserDialog::navigateTo( const QString& path )
{
	if( path.isEmpty() )
	{
		showDrives();
		return;
	}

	m_status->setText( tr( "Loading %1..." ).arg( path ) );
	m_plugin->requestDirectory( m_computer, path );
}



void RemoteFileBrowserDialog::navigateUp()
{
	if( m_currentPath.isEmpty() )
	{
		showDrives();
		return;
	}

	QDir dir( m_currentPath );
	if( dir.cdUp() )
	{
		navigateTo( dir.absolutePath() );
	}
	else
	{
		showDrives();
	}
}



void RemoteFileBrowserDialog::refresh()
{
	if( m_currentPath.isEmpty() )
	{
		showDrives();
	}
	else
	{
		navigateTo( m_currentPath );
	}
}



void RemoteFileBrowserDialog::setDrives( const QVariantList& drives )
{
	m_tree->clear();
	m_currentPath.clear();
	m_pathEdit->clear();

	for( const auto& driveVariant : drives )
	{
		const auto drive = driveVariant.toMap();
		const auto name = drive.value( RemoteFileBrowserPlugin::entryKeyName() ).toString();

		auto item = new QTreeWidgetItem( m_tree );
		item->setText( 0, name );
		item->setText( 1, formatSize( drive.value( RemoteFileBrowserPlugin::entryKeySize() ).toLongLong() ) );
		item->setText( 2, tr( "Drive" ) );
		item->setData( 0, RoleIsDir, true );
		item->setData( 0, RoleName, name );
	}

	m_status->setText( tr( "%n drive(s)", "", drives.count() ) );
}



void RemoteFileBrowserDialog::setDirectoryListing( const QString& path, const QVariantList& entries,
												   const QString& error )
{
	if( error.isEmpty() == false )
	{
		m_status->setText( error );
		QMessageBox::warning( this, tr( "File browser" ), error );
		return;
	}

	m_tree->clear();
	m_currentPath = path;
	m_pathEdit->setText( path );

	for( const auto& entryVariant : entries )
	{
		const auto entry = entryVariant.toMap();
		const auto name = entry.value( RemoteFileBrowserPlugin::entryKeyName() ).toString();
		const auto isDir = entry.value( RemoteFileBrowserPlugin::entryKeyIsDir() ).toBool();

		auto item = new QTreeWidgetItem( m_tree );
		item->setText( 0, name );
		item->setText( 1, isDir ? QString() : formatSize( entry.value( RemoteFileBrowserPlugin::entryKeySize() ).toLongLong() ) );

		const auto mtime = entry.value( RemoteFileBrowserPlugin::entryKeyModified() );
		if( mtime.isValid() && mtime.isNull() == false )
		{
			item->setText( 2, QDateTime::fromMSecsSinceEpoch( mtime.toLongLong() )
						   .toString( QStringLiteral("yyyy-MM-dd hh:mm") ) );
		}

		item->setData( 0, RoleIsDir, isDir );
		item->setData( 0, RoleName, name );
	}

	m_status->setText( tr( "%n item(s)", "", entries.count() ) );
}



void RemoteFileBrowserDialog::onItemDoubleClicked( QTreeWidgetItem* item, int column )
{
	Q_UNUSED(column)

	if( item == nullptr )
	{
		return;
	}

	const auto name = item->data( 0, RoleName ).toString();

	if( item->data( 0, RoleIsDir ).toBool() )
	{
		// drive entries already carry an absolute root path
		navigateTo( m_currentPath.isEmpty() ? name
										    : QDir( m_currentPath ).filePath( name ) );
	}
	else
	{
		downloadSelected();
	}
}



void RemoteFileBrowserDialog::downloadSelected()
{
	const auto items = m_tree->selectedItems();
	if( items.isEmpty() || items.first()->data( 0, RoleIsDir ).toBool() )
	{
		return;
	}

	if( m_transferId.isNull() == false )
	{
		QMessageBox::information( this, tr( "File browser" ),
								  tr( "A download is already in progress." ) );
		return;
	}

	const auto name = items.first()->data( 0, RoleName ).toString();
	const auto remotePath = QDir( m_currentPath ).filePath( name );

	const auto destination = QFileDialog::getSaveFileName( this, tr( "Save file as" ), name );
	if( destination.isEmpty() )
	{
		return;
	}

	m_localFile.setFileName( destination );
	if( m_localFile.open( QFile::WriteOnly | QFile::Truncate ) == false )
	{
		QMessageBox::warning( this, tr( "File browser" ),
							  tr( "Could not open local file for writing: %1" ).arg( m_localFile.errorString() ) );
		return;
	}

	m_transferId = QUuid::createUuid();
	m_receivedBytes = 0;
	m_expectedSize = 0;

	m_progress->setVisible( true );
	m_progress->setRange( 0, 0 );
	m_status->setText( tr( "Requesting %1..." ).arg( name ) );

	m_plugin->startDownload( m_computer, m_transferId, remotePath );
}



void RemoteFileBrowserDialog::downloadStarted( QUuid transferId, qint64 fileSize, const QString& error )
{
	if( transferId != m_transferId )
	{
		return;
	}

	if( error.isEmpty() == false )
	{
		QMessageBox::warning( this, tr( "File browser" ), error );
		resetDownload();
		return;
	}

	m_expectedSize = fileSize;
	m_progress->setRange( 0, fileSize > 0 ? 100 : 0 );
	m_status->setText( tr( "Downloading... (%1)" ).arg( formatSize( fileSize ) ) );
}



void RemoteFileBrowserDialog::downloadDataReceived( QUuid transferId, const QByteArray& data )
{
	if( transferId != m_transferId || m_localFile.isOpen() == false )
	{
		return;
	}

	m_localFile.write( data );
	m_receivedBytes += data.size();

	if( m_expectedSize > 0 )
	{
		m_progress->setValue( static_cast<int>( ( m_receivedBytes * 100 ) / m_expectedSize ) );
	}
}



void RemoteFileBrowserDialog::downloadFinished( QUuid transferId )
{
	if( transferId != m_transferId )
	{
		return;
	}

	const auto fileName = m_localFile.fileName();
	const auto bytes = m_receivedBytes;

	resetDownload();

	m_status->setText( tr( "Saved %1 (%2)" ).arg( fileName, formatSize( bytes ) ) );
}



void RemoteFileBrowserDialog::resetDownload()
{
	if( m_localFile.isOpen() )
	{
		m_localFile.close();
	}

	m_transferId = QUuid();
	m_receivedBytes = 0;
	m_expectedSize = 0;
	m_progress->setVisible( false );
}



QString RemoteFileBrowserDialog::formatSize( qint64 bytes )
{
	static const QStringList units{ QStringLiteral("B"), QStringLiteral("KB"),
									QStringLiteral("MB"), QStringLiteral("GB"),
									QStringLiteral("TB") };
	auto size = static_cast<double>( bytes );
	int unit = 0;

	while( size >= 1024.0 && unit < units.count() - 1 )
	{
		size /= 1024.0;
		++unit;
	}

	return QStringLiteral("%1 %2").arg( size, 0, 'f', unit == 0 ? 0 : 1 ).arg( units.at( unit ) );
}
