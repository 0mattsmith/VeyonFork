/*
 * RemoteFileBrowserPlugin.h - interactive remote file browsing/retrieval
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

#include <QFile>
#include <QPointer>
#include <QTimer>
#include <QUuid>

#include "FeatureProviderInterface.h"
#include "PluginInterface.h"

class RemoteFileBrowserDialog;

// clazy:excludeall=copyable-polymorphic

class RemoteFileBrowserPlugin : public QObject, FeatureProviderInterface, PluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.RemoteFileBrowser")
	Q_INTERFACES(PluginInterface FeatureProviderInterface)
public:
	enum class FeatureCommand
	{
		// master -> client
		GetDrives,
		ListDirectory,
		StartDownload,
		CancelDownload,
		StopWorker,
		// client -> master
		DriveList,
		DirectoryListing,
		DownloadInfo,
		DownloadDataChunk,
		DownloadFinished,
	};
	Q_ENUM(FeatureCommand)

	enum class Argument
	{
		RequestId,
		Path,
		Entries,
		TransferId,
		FileName,
		FileSize,
		DataChunk,
		Error,
	};
	Q_ENUM(Argument)

	explicit RemoteFileBrowserPlugin( QObject* parent = nullptr );
	~RemoteFileBrowserPlugin() override = default;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("7f3c1e64-9a2b-4d51-8c77-2e5b9d41af03") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber(1, 0);
	}

	QString name() const override
	{
		return QStringLiteral("RemoteFileBrowser");
	}

	QString description() const override
	{
		return tr( "Browse and retrieve files from remote computers" );
	}

	QString vendor() const override
	{
		return QStringLiteral("VeyonFork");
	}

	QString copyright() const override
	{
		return QStringLiteral("VeyonFork contributors");
	}

	const FeatureList& featureList() const override
	{
		return m_features;
	}

	bool controlFeature( Feature::Uid featureUid, Operation operation, const QVariantMap& arguments,
						 const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool startFeature( VeyonMasterInterface& master, const Feature& feature,
					   const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
							   const FeatureMessage& message ) override;

	bool handleFeatureMessage( VeyonServerInterface& server,
							   const MessageContext& messageContext,
							   const FeatureMessage& message ) override;

	bool handleFeatureMessageFromWorker( VeyonServerInterface& server,
										 const FeatureMessage& message ) override;

	bool handleFeatureMessage( VeyonWorkerInterface& worker,
							   const FeatureMessage& message ) override;

	// --- master-side API used by RemoteFileBrowserDialog ---
	void requestDrives( const ComputerControlInterface::Pointer& computer );
	void requestDirectory( const ComputerControlInterface::Pointer& computer, const QString& path );
	void startDownload( const ComputerControlInterface::Pointer& computer,
						QUuid transferId, const QString& remotePath );
	void cancelDownload( const ComputerControlInterface::Pointer& computer, QUuid transferId );

	// keys used within an entry of the Entries argument (a QVariantList of QVariantMap)
	static QString entryKeyName() { return QStringLiteral("name"); }
	static QString entryKeyIsDir() { return QStringLiteral("dir"); }
	static QString entryKeySize() { return QStringLiteral("size"); }
	static QString entryKeyModified() { return QStringLiteral("mtime"); }

private:
	static QString featureIconUrl();

	// worker-side handlers
	bool workerGetDrives( VeyonWorkerInterface& worker );
	bool workerListDirectory( VeyonWorkerInterface& worker, const FeatureMessage& message );
	bool workerStartDownload( VeyonWorkerInterface& worker, const FeatureMessage& message );
	void workerCancelDownload();
	void pumpDownload();

	const Feature m_feature;
	const FeatureList m_features;

	// master side
	QPointer<RemoteFileBrowserDialog> m_dialog;

	// server side: remember which master asked, so worker replies can be routed back
	MessageContext m_masterContext{};

	// worker side download state
	VeyonWorkerInterface* m_worker{nullptr};
	QFile m_downloadFile;
	QUuid m_downloadTransferId;
	QTimer m_downloadTimer;

	static constexpr qint64 ChunkSize = 128 * 1024;
};
