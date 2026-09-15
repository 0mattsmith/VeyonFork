/*
 * RemoteFileBrowserPlugin.cpp - interactive remote file browsing/retrieval
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

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStorageInfo>

#include "ComputerControlInterface.h"
#include "FeatureWorkerManager.h"
#include "RemoteFileBrowserDialog.h"
#include "RemoteFileBrowserPlugin.h"
#include "VeyonCore.h"
#include "VeyonMasterInterface.h"
#include "VeyonServerInterface.h"
#include "VeyonWorkerInterface.h"


QString RemoteFileBrowserPlugin::featureIconUrl()
{
	// pick the dark variant so the feature is dark-mode aware from day one
	return VeyonCore::useDarkMode()
			? QStringLiteral(":/remotefilebrowser/remote-file-browser-dark.png")
			: QStringLiteral(":/remotefilebrowser/remote-file-browser.png");
}



RemoteFileBrowserPlugin::RemoteFileBrowserPlugin( QObject* parent ) :
	QObject( parent ),
	m_feature( QStringLiteral("RemoteFileBrowser"),
			   Feature::Flag::Action | Feature::Flag::AllComponents,
			   Feature::Uid("b1d9f27a-4c86-4f1e-9a3d-6e0c85f7b214"),
			   Feature::Uid(),
			   tr( "File browser" ), {},
			   tr( "Click this button to browse the files on a remote computer and retrieve them." ),
			   featureIconUrl() ),
	m_features( { m_feature } )
{
	m_downloadTimer.setInterval( 0 );
	connect( &m_downloadTimer, &QTimer::timeout, this, &RemoteFileBrowserPlugin::pumpDownload );
}



bool RemoteFileBrowserPlugin::controlFeature( Feature::Uid featureUid, Operation operation,
											  const QVariantMap& arguments,
											  const ComputerControlInterfaceList& computerControlInterfaces )
{
	Q_UNUSED(featureUid)
	Q_UNUSED(operation)
	Q_UNUSED(arguments)
	Q_UNUSED(computerControlInterfaces)

	// the file browser is an interactive, GUI-driven feature - there is no meaningful
	// headless/CLI control for it yet
	return false;
}



bool RemoteFileBrowserPlugin::startFeature( VeyonMasterInterface& master, const Feature& feature,
											const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( feature != m_feature )
	{
		return false;
	}

	if( computerControlInterfaces.isEmpty() )
	{
		QMessageBox::information( master.mainWindow(), tr( "File browser" ),
								  tr( "Please select a computer to browse." ) );
		return true;
	}

	if( computerControlInterfaces.count() > 1 )
	{
		QMessageBox::information( master.mainWindow(), tr( "File browser" ),
								  tr( "Multiple computers are selected. The file browser will open "
									  "for the first one." ) );
	}

	if( m_dialog )
	{
		m_dialog->raise();
		m_dialog->activateWindow();
		return true;
	}

	auto dialog = new RemoteFileBrowserDialog( this, computerControlInterfaces.first(), master.mainWindow() );
	m_dialog = dialog;
	connect( dialog, &QDialog::finished, dialog, &QDialog::deleteLater );
	dialog->open();

	return true;
}



// ---------------------------------------------------------------------------
// master side: replies coming back from a client
// ---------------------------------------------------------------------------
bool RemoteFileBrowserPlugin::handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
													const FeatureMessage& message )
{
	Q_UNUSED(computerControlInterface)

	if( message.featureUid() != m_feature.uid() )
	{
		return false;
	}

	if( m_dialog.isNull() )
	{
		return true;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::DriveList:
		m_dialog->setDrives( message.argument( Argument::Entries ).toList() );
		break;

	case FeatureCommand::DirectoryListing:
		m_dialog->setDirectoryListing( message.argument( Argument::Path ).toString(),
									   message.argument( Argument::Entries ).toList(),
									   message.argument( Argument::Error ).toString() );
		break;

	case FeatureCommand::DownloadInfo:
		m_dialog->downloadStarted( message.argument( Argument::TransferId ).toUuid(),
								   message.argument( Argument::FileSize ).toLongLong(),
								   message.argument( Argument::Error ).toString() );
		break;

	case FeatureCommand::DownloadDataChunk:
		m_dialog->downloadDataReceived( message.argument( Argument::TransferId ).toUuid(),
										message.argument( Argument::DataChunk ).toByteArray() );
		break;

	case FeatureCommand::DownloadFinished:
		m_dialog->downloadFinished( message.argument( Argument::TransferId ).toUuid() );
		break;

	default:
		break;
	}

	return true;
}



// ---------------------------------------------------------------------------
// server side (student PC): remember the caller and hand work to the session worker
// ---------------------------------------------------------------------------
bool RemoteFileBrowserPlugin::handleFeatureMessage( VeyonServerInterface& server,
													const MessageContext& messageContext,
													const FeatureMessage& message )
{
	if( message.featureUid() != m_feature.uid() )
	{
		return false;
	}

	// remember where the request came from so worker replies can be routed back
	m_masterContext = messageContext;

	if( message.command<FeatureCommand>() == FeatureCommand::StopWorker )
	{
		if( server.featureWorkerManager().isWorkerRunning( m_feature.uid() ) )
		{
			server.featureWorkerManager().sendMessageToUnmanagedSessionWorker( message );
			server.featureWorkerManager().stopWorker( m_feature.uid() );
		}
		return true;
	}

	// the worker runs in the user's session and therefore sees the user's files
	server.featureWorkerManager().sendMessageToUnmanagedSessionWorker( message );

	return true;
}



bool RemoteFileBrowserPlugin::handleFeatureMessageFromWorker( VeyonServerInterface& server,
															  const FeatureMessage& message )
{
	if( message.featureUid() != m_feature.uid() )
	{
		return false;
	}

	// forward the worker's reply to the master that asked
	return server.sendFeatureMessageReply( m_masterContext, message );
}



// ---------------------------------------------------------------------------
// worker side (user session): the actual filesystem access
// ---------------------------------------------------------------------------
bool RemoteFileBrowserPlugin::handleFeatureMessage( VeyonWorkerInterface& worker,
													const FeatureMessage& message )
{
	if( message.featureUid() != m_feature.uid() )
	{
		return false;
	}

	m_worker = &worker;

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::GetDrives:
		return workerGetDrives( worker );

	case FeatureCommand::ListDirectory:
		return workerListDirectory( worker, message );

	case FeatureCommand::StartDownload:
		return workerStartDownload( worker, message );

	case FeatureCommand::CancelDownload:
	case FeatureCommand::StopWorker:
		workerCancelDownload();
		return true;

	default:
		break;
	}

	return true;
}



bool RemoteFileBrowserPlugin::workerGetDrives( VeyonWorkerInterface& worker )
{
	QVariantList entries;

	const auto volumes = QStorageInfo::mountedVolumes();
	for( const auto& volume : volumes )
	{
		if( volume.isValid() == false || volume.isReady() == false )
		{
			continue;
		}

		QVariantMap entry;
		entry[entryKeyName()] = volume.rootPath();
		entry[entryKeyIsDir()] = true;
		entry[entryKeySize()] = static_cast<qlonglong>( volume.bytesTotal() );
		entry[entryKeyModified()] = QVariant();
		entries.append( entry );
	}

	return worker.sendFeatureMessageReply(
				FeatureMessage( m_feature.uid(), FeatureCommand::DriveList )
					.addArgument( Argument::Entries, entries ) );
}



bool RemoteFileBrowserPlugin::workerListDirectory( VeyonWorkerInterface& worker,
												   const FeatureMessage& message )
{
	const auto path = message.argument( Argument::Path ).toString();

	QVariantList entries;
	QString error;

	const QDir dir( path );
	if( dir.exists() == false )
	{
		error = tr( "Directory does not exist or is not accessible." );
	}
	else
	{
		const auto infoList = dir.entryInfoList( QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
												 QDir::DirsFirst | QDir::Name );
		entries.reserve( infoList.count() );

		for( const auto& info : infoList )
		{
			QVariantMap entry;
			entry[entryKeyName()] = info.fileName();
			entry[entryKeyIsDir()] = info.isDir();
			entry[entryKeySize()] = static_cast<qlonglong>( info.isDir() ? 0 : info.size() );
			entry[entryKeyModified()] = info.lastModified().toMSecsSinceEpoch();
			entries.append( entry );
		}
	}

	return worker.sendFeatureMessageReply(
				FeatureMessage( m_feature.uid(), FeatureCommand::DirectoryListing )
					.addArgument( Argument::Path, dir.absolutePath() )
					.addArgument( Argument::Entries, entries )
					.addArgument( Argument::Error, error ) );
}



bool RemoteFileBrowserPlugin::workerStartDownload( VeyonWorkerInterface& worker,
												   const FeatureMessage& message )
{
	// only one download at a time in this version
	workerCancelDownload();

	m_downloadTransferId = message.argument( Argument::TransferId ).toUuid();
	const auto path = message.argument( Argument::Path ).toString();

	m_downloadFile.setFileName( path );

	QString error;
	qint64 size = 0;

	const QFileInfo fileInfo( path );
	if( fileInfo.exists() == false || fileInfo.isFile() == false )
	{
		error = tr( "File does not exist." );
	}
	else if( m_downloadFile.open( QFile::ReadOnly ) == false )
	{
		error = tr( "Could not open file: %1" ).arg( m_downloadFile.errorString() );
	}
	else
	{
		size = m_downloadFile.size();
	}

	worker.sendFeatureMessageReply(
				FeatureMessage( m_feature.uid(), FeatureCommand::DownloadInfo )
					.addArgument( Argument::TransferId, m_downloadTransferId )
					.addArgument( Argument::FileName, fileInfo.fileName() )
					.addArgument( Argument::FileSize, size )
					.addArgument( Argument::Error, error ) );

	if( error.isEmpty() )
	{
		m_downloadTimer.start();
	}

	return true;
}



void RemoteFileBrowserPlugin::pumpDownload()
{
	if( m_worker == nullptr || m_downloadFile.isOpen() == false )
	{
		m_downloadTimer.stop();
		return;
	}

	const auto data = m_downloadFile.read( ChunkSize );

	if( data.isEmpty() )
	{
		m_downloadTimer.stop();
		m_downloadFile.close();

		m_worker->sendFeatureMessageReply(
					FeatureMessage( m_feature.uid(), FeatureCommand::DownloadFinished )
						.addArgument( Argument::TransferId, m_downloadTransferId ) );
		return;
	}

	m_worker->sendFeatureMessageReply(
				FeatureMessage( m_feature.uid(), FeatureCommand::DownloadDataChunk )
					.addArgument( Argument::TransferId, m_downloadTransferId )
					.addArgument( Argument::DataChunk, data ) );
}



void RemoteFileBrowserPlugin::workerCancelDownload()
{
	m_downloadTimer.stop();

	if( m_downloadFile.isOpen() )
	{
		m_downloadFile.close();
	}
}



// ---------------------------------------------------------------------------
// master-side senders used by the dialog
// ---------------------------------------------------------------------------
void RemoteFileBrowserPlugin::requestDrives( const ComputerControlInterface::Pointer& computer )
{
	computer->sendFeatureMessage( FeatureMessage( m_feature.uid(), FeatureCommand::GetDrives ) );
}



void RemoteFileBrowserPlugin::requestDirectory( const ComputerControlInterface::Pointer& computer,
												const QString& path )
{
	computer->sendFeatureMessage( FeatureMessage( m_feature.uid(), FeatureCommand::ListDirectory )
								  .addArgument( Argument::Path, path ) );
}



void RemoteFileBrowserPlugin::startDownload( const ComputerControlInterface::Pointer& computer,
											 QUuid transferId, const QString& remotePath )
{
	computer->sendFeatureMessage( FeatureMessage( m_feature.uid(), FeatureCommand::StartDownload )
								  .addArgument( Argument::TransferId, transferId )
								  .addArgument( Argument::Path, remotePath ) );
}



void RemoteFileBrowserPlugin::cancelDownload( const ComputerControlInterface::Pointer& computer,
											  QUuid transferId )
{
	computer->sendFeatureMessage( FeatureMessage( m_feature.uid(), FeatureCommand::CancelDownload )
								  .addArgument( Argument::TransferId, transferId ) );
}
