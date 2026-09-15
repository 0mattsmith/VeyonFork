include(BuildVeyonPlugin)

build_veyon_plugin(remotefilebrowser
	NAME RemoteFileBrowser
	SOURCES
	RemoteFileBrowserPlugin.cpp
	RemoteFileBrowserPlugin.h
	RemoteFileBrowserDialog.cpp
	RemoteFileBrowserDialog.h
	remotefilebrowser.qrc
	)
