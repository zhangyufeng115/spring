/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _ARCHIVE_LOADER_H
#define _ARCHIVE_LOADER_H

#include <array>
#include <string>

class IArchive;
class IArchiveFactory;

enum {
	ARCHIVE_TYPE_SDP = 0, // pool
	ARCHIVE_TYPE_SDD = 1, // dir
	ARCHIVE_TYPE_SDZ = 2, // zip
	ARCHIVE_TYPE_SD7 = 3, // 7zip
	ARCHIVE_TYPE_SDV = 4, // virtual
	ARCHIVE_TYPE_CNT = 5,
};

/**
 * Engine side interface for loading of different archive types.
 * This loader is responsible for offering access to different archive types,
 * without the rest of the engine having to know anything about those types.
 */
class CArchiveLoader
{
	CArchiveLoader();

public:
	static const CArchiveLoader& GetInstance();

	/// Returns true if the indicated file is in fact an archive
	bool IsArchiveFile(const std::string& fileName) const;

	/// Returns a pointer to a new'ed suitable subclass of IArchive
	IArchive* OpenArchive(const std::string& fileName, const std::string& type = "") const;

private:
	/// maps the default-extension to the corresponding archive factory
	std::array< std::pair<std::string, IArchiveFactory*>, ARCHIVE_TYPE_CNT> archiveFactories;
};

#define archiveLoader CArchiveLoader::GetInstance()

#endif // _ARCHIVE_LOADER_H
