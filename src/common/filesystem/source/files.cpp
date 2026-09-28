/*
** files.cpp
**
** Implements classes for reading from files or memory blocks
**
**---------------------------------------------------------------------------
**
** Copyright 1998-2016 Marisa Heit
** Copyright 2005-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include <string>
#include <memory>
#include <algorithm>
#include <assert.h>
#include <string.h>
#include "files_internal.h"
#include "m_crc32.h"
#include "engineerrors.h"

namespace FileSys {

#ifdef _WIN32
std::wstring toWide(const char* str);
#endif

FILE *myfopen(const char *filename, const char *flags)
{
#ifndef _WIN32
	return fopen(filename, flags);
#else
	auto widename = toWide(filename);
	auto wideflags = toWide(flags);
	return _wfopen(widename.c_str(), wideflags.c_str());
#endif
}

#ifdef _WIN32
#define fseek _fseeki64
#define ftell _ftelli64
#endif


#if defined(__SWITCH__) || defined(FS_BLOCKCACHE)
#define FS_USE_BLOCKCACHE 1
}	// namespace FileSys
#include <mutex>
#include <list>
#include <unordered_map>
#include <stdlib.h>
#include <strings.h>
namespace FileSys {

//==========================================================================
//
// Block cache for resource archives (Switch).
//
// On the Switch every fread that misses stdio's tiny buffer is an IPC
// round trip to the FS sysmodule plus an SD card access. Opening big mods
// (a large gameplay mod has tens of thousands of lumps) touches the header of nearly every
// lump in an order that has nothing to do with the physical file layout,
// which resulted in several hundred thousand small random reads.
// This cache reads archives in large aligned blocks and keeps them in an
// LRU list, so that startup becomes a few hundred large sequential reads.
//
//==========================================================================

namespace {

struct BCKey
{
	uint32_t file;
	uint32_t block;
	bool operator==(const BCKey &o) const { return file == o.file && block == o.block; }
};

struct BCKeyHash
{
	size_t operator()(const BCKey &k) const { return (size_t(k.file) * 0x9E3779B97F4A7C15ull) ^ k.block; }
};

struct BCBlock
{
	BCKey key;
	uint32_t size;
	uint8_t *data;
};

struct BlockCache
{
	std::mutex mtx;
	std::list<BCBlock> lru;	// front = most recently used
	std::unordered_map<BCKey, std::list<BCBlock>::iterator, BCKeyHash> map;
	std::unordered_map<std::string, uint32_t> ids;
	uint32_t nextId = 1;
	size_t used = 0;
	size_t blockSize = size_t(1) << 20;	// startup: 1 MB blocks
	size_t capacity = size_t(128) << 20;	// startup: 128 MB
	uint64_t hits = 0, misses = 0, bytesRead = 0;

	void Clear()
	{
		for (auto &b : lru) free(b.data);
		lru.clear();
		map.clear();
		used = 0;
	}

	void Evict()
	{
		// never evict the most recent block
		while (used > capacity && lru.size() > 1)
		{
			auto &b = lru.back();
			used -= b.size;
			free(b.data);
			map.erase(b.key);
			lru.pop_back();
		}
	}
};

BlockCache &GetBlockCache()
{
	static BlockCache bc;
	return bc;
}

bool IsCacheableFile(const char *filename)
{
	const char *dot = strrchr(filename, '.');
	const char *slash = strrchr(filename, '/');
	if (dot == nullptr || (slash != nullptr && dot < slash)) return false;
	static const char *const exts[] = { ".wad", ".iwad", ".pk3", ".ipk3", ".pk7", ".ipk7", ".zip", ".pkz", ".pke", ".pk4", ".7z", ".grp", ".rff", ".dat", ".sf2", nullptr };
	for (int i = 0; exts[i]; i++)
	{
		if (!strcasecmp(dot, exts[i])) return true;
	}
	return false;
}

uint32_t BC_GetId(const char *filename, ptrdiff_t size)
{
	if (!IsCacheableFile(filename)) return 0;
	auto &bc = GetBlockCache();
	std::lock_guard<std::mutex> lock(bc.mtx);
	std::string key = filename;
	key += '|';
	key += std::to_string((long long)size);
	auto it = bc.ids.find(key);
	if (it != bc.ids.end()) return it->second;
	uint32_t id = bc.nextId++;
	bc.ids[key] = id;
	return id;
}

ptrdiff_t BC_Read(uint32_t id, FILE *f, ptrdiff_t physSize, ptrdiff_t pos, void *buffer, ptrdiff_t len)
{
	// The SD card is only accessed with the lock released, so a background
	// thread reading a block never makes the game (or the music) wait.
	auto &bc = GetBlockCache();
	std::unique_lock<std::mutex> lock(bc.mtx);
	const size_t B = bc.blockSize;
	uint8_t *out = (uint8_t *)buffer;

	if ((size_t)len >= B)
	{
		// large read - no point in caching it.
		lock.unlock();
		if (fseek(f, pos, SEEK_SET) != 0) return 0;
		size_t r = fread(buffer, 1, len, f);
		lock.lock();
		bc.bytesRead += r;
		return (ptrdiff_t)r;
	}

	ptrdiff_t done = 0;
	while (done < len)
	{
		ptrdiff_t p = pos + done;
		if (p >= physSize) break;
		BCKey key{ id, uint32_t(size_t(p) / B) };
		std::list<BCBlock>::iterator bit;
		auto it = bc.map.find(key);
		if (it != bc.map.end())
		{
			bit = it->second;
			if (bit != bc.lru.begin()) bc.lru.splice(bc.lru.begin(), bc.lru, bit);
			bc.hits++;
		}
		else
		{
			size_t start = size_t(key.block) * B;
			size_t sz = std::min<size_t>(B, size_t(physSize) - start);
			lock.unlock();
			uint8_t *data = (uint8_t *)malloc(sz);
			if (data == nullptr)
			{
				// out of memory - drop the cache and read directly
				lock.lock();
				bc.Clear();
				lock.unlock();
				if (fseek(f, p, SEEK_SET) != 0) return done;
				return done + (ptrdiff_t)fread(out + done, 1, len - done, f);
			}
			size_t r = 0;
			if (fseek(f, (long)start, SEEK_SET) == 0) r = fread(data, 1, sz, f);
			lock.lock();
			if (B != bc.blockSize)
			{
				// the cache was reconfigured meanwhile: finish this read without it
				lock.unlock();
				free(data);
				if (fseek(f, p, SEEK_SET) != 0) return done;
				return done + (ptrdiff_t)fread(out + done, 1, len - done, f);
			}
			bc.misses++;
			bc.bytesRead += r;
			if (r == 0)
			{
				free(data);
				break;
			}
			auto again = bc.map.find(key);
			if (again != bc.map.end())
			{
				// somebody else read the same block in the meantime
				free(data);
				bit = again->second;
				if (bit != bc.lru.begin()) bc.lru.splice(bc.lru.begin(), bc.lru, bit);
			}
			else
			{
				bc.lru.push_front(BCBlock{ key, (uint32_t)r, data });
				bit = bc.lru.begin();
				bc.map[key] = bit;
				bc.used += r;
				bc.Evict();
			}
		}
		size_t off = size_t(p) - size_t(bit->key.block) * B;
		if (off >= bit->size) break;
		size_t n = std::min<size_t>(bit->size - off, size_t(len - done));
		memcpy(out + done, bit->data + off, n);
		done += (ptrdiff_t)n;
	}
	return done;
}

} // anonymous namespace

void SetBlockCacheMode(bool bulk)
{
	auto &bc = GetBlockCache();
	std::lock_guard<std::mutex> lock(bc.mtx);
	size_t bs = bulk ? (size_t(1) << 20) : (size_t(256) << 10);
	size_t cap = bulk ? (size_t(128) << 20) : (size_t(48) << 20);
	if (bs != bc.blockSize) bc.Clear();
	bc.blockSize = bs;
	bc.capacity = cap;
	bc.Evict();
}

void GetBlockCacheStats(uint64_t &hits, uint64_t &misses, uint64_t &bytesread)
{
	auto &bc = GetBlockCache();
	std::lock_guard<std::mutex> lock(bc.mtx);
	hits = bc.hits;
	misses = bc.misses;
	bytesread = bc.bytesRead;
}
#else
void SetBlockCacheMode(bool) {}
void GetBlockCacheStats(uint64_t &hits, uint64_t &misses, uint64_t &bytesread) { hits = misses = bytesread = 0; }
#endif

//==========================================================================
//
// StdFileReader
//
// reads data from an stdio FILE* or part of it.
//
//==========================================================================

class StdFileReader : public FileReaderInterface
{
	FILE *File = nullptr;
	ptrdiff_t StartPos = 0;
	ptrdiff_t FilePos = 0;
#ifdef FS_USE_BLOCKCACHE
	uint32_t CacheId = 0;
	ptrdiff_t PhysSize = 0;
#endif

public:
	StdFileReader()
	{}

	~StdFileReader()
	{
		if (File != nullptr)
		{
			fclose(File);
		}
		File = nullptr;
	}

	bool Open(const char *filename, ptrdiff_t startpos = 0, ptrdiff_t len = -1)
	{
		File = myfopen(filename, "rb");
		if (File == nullptr) return false;
		FilePos = startpos;
		StartPos = startpos;
		Length = CalcFileLen();
#ifdef FS_USE_BLOCKCACHE
		PhysSize = Length;
		CacheId = BC_GetId(filename, PhysSize);
#endif
		if (len >= 0 && len < Length) Length = len;
		if (startpos > 0) Seek(0, SEEK_SET);
		return true;
	}

	ptrdiff_t Tell() const override
	{
		return FilePos - StartPos;
	}

	ptrdiff_t Seek(ptrdiff_t offset, int origin) override
	{
		if (origin == SEEK_SET)
		{
			offset += StartPos;
		}
		else if (origin == SEEK_CUR)
		{
			offset += FilePos;
		}
		else if (origin == SEEK_END)
		{
			offset += StartPos + Length;
		}
		if (offset < StartPos || offset > StartPos + Length) return -1;	// out of scope

#ifdef FS_USE_BLOCKCACHE
		if (CacheId != 0)
		{
			// the actual seek is done lazily by the reading code.
			FilePos = offset;
			return 0;
		}
#endif
		if (0 == fseek(File, offset, SEEK_SET))
		{
			FilePos = offset;
			return 0;
		}
		return -1;
	}

	ptrdiff_t Read(void *buffer, ptrdiff_t len) override
	{
		assert(len >= 0);
		if (len <= 0) return 0;
		if (FilePos + len > StartPos + Length)
		{
			len = Length - FilePos + StartPos;
		}
		if (len <= 0) return 0;
#ifdef FS_USE_BLOCKCACHE
		if (CacheId != 0)
		{
			len = BC_Read(CacheId, File, PhysSize, FilePos, buffer, len);
			FilePos += len;
			return len;
		}
#endif
		len = fread(buffer, 1, len, File);
		FilePos += len;
		return len;
	}

	char *Gets(char *strbuf, ptrdiff_t len) override
	{
		if (len <= 0 || len > 0x7fffffff || FilePos >= StartPos + Length) return nullptr;
#ifdef FS_USE_BLOCKCACHE
		if (CacheId != 0) fseek(File, FilePos, SEEK_SET);
#endif
		char *p = fgets(strbuf, (int)len, File);
		if (p != nullptr)
		{
			ptrdiff_t old = FilePos;
			FilePos = ftell(File);
			if (FilePos - StartPos > Length)
			{
				strbuf[Length - old + StartPos] = 0;
			}
		}
		return p;
	}

private:
	ptrdiff_t CalcFileLen() const
	{
		ptrdiff_t endpos;

		fseek(File, 0, SEEK_END);
		endpos = ftell(File);
		fseek(File, 0, SEEK_SET);
		return endpos;
	}
};

//==========================================================================
//
// FileReaderRedirect
//
// like the above, but uses another File reader as its backing data
//
//==========================================================================

class FileReaderRedirect : public FileReaderInterface
{
	FileReader *mReader = nullptr;
	ptrdiff_t StartPos = 0;
	ptrdiff_t FilePos = 0;

public:
	FileReaderRedirect(FileReader &parent, ptrdiff_t start, ptrdiff_t length)
	{
		mReader = &parent;
		FilePos = start;
		StartPos = start;
		Length = length;
		Seek(0, SEEK_SET);
	}

	virtual ptrdiff_t Tell() const override
	{
		return FilePos - StartPos;
	}

	virtual ptrdiff_t Seek(ptrdiff_t offset, int origin) override
	{
		switch (origin)
		{
		case SEEK_SET:
			offset += StartPos;
			break;

		case SEEK_END:
			offset += StartPos + Length;
			break;

		case SEEK_CUR:
			offset += mReader->Tell();
			break;
		}
		if (offset < StartPos || offset > StartPos + Length) return -1;	// out of scope
		if (mReader->Seek(offset, FileReader::SeekSet) == 0)
		{
			FilePos = offset;
			return 0;
		}
		return -1;
	}

	virtual ptrdiff_t Read(void *buffer, ptrdiff_t len) override
	{
		assert(len >= 0);
		if (len <= 0) return 0;
		if (FilePos + len > StartPos + Length)
		{
			len = Length - FilePos + StartPos;
		}
		len = mReader->Read(buffer, len);
		FilePos += len;
		return len;
	}

	virtual char *Gets(char *strbuf, ptrdiff_t len) override
	{
		if (len <= 0 || FilePos >= StartPos + Length) return nullptr;
		char *p = mReader->Gets(strbuf, len);
		if (p != nullptr)
		{
			ptrdiff_t old = FilePos;
			FilePos = mReader->Tell();
			if (FilePos - StartPos > Length)
			{
				strbuf[Length - old + StartPos] = 0;
			}
		}
		return p;
	}

};

//==========================================================================
//
// MemoryReader
//
// reads data from a block of memory
//
//==========================================================================

ptrdiff_t MemoryReader::Tell() const
{
	return FilePos;
}

ptrdiff_t MemoryReader::Seek(ptrdiff_t offset, int origin)
{
	switch (origin)
	{
	case SEEK_CUR:
		offset += FilePos;
		break;

	case SEEK_END:
		offset += Length;
		break;

	}
	if (offset < 0 || offset > Length) return -1;
	FilePos = std::clamp<ptrdiff_t>(offset, 0, Length);
	return 0;
}

ptrdiff_t MemoryReader::Read(void *buffer, ptrdiff_t len)
{
	if (len > Length - FilePos) len = Length - FilePos;
	if (len<0) len = 0;
	memcpy(buffer, bufptr + FilePos, len);
	FilePos += len;
	return len;
}

char *MemoryReader::Gets(char *strbuf, ptrdiff_t len)
{
	if (len>Length - FilePos) len = Length - FilePos;
	if (len <= 0) return nullptr;

	char *p = strbuf;
	while (len > 1)
	{
		if (bufptr[FilePos] == 0)
		{
			FilePos++;
			break;
		}
		if (bufptr[FilePos] != '\r')
		{
			*p++ = bufptr[FilePos];
			len--;
			if (bufptr[FilePos] == '\n')
			{
				FilePos++;
				break;
			}
		}
		FilePos++;
	}
	if (p == strbuf) return nullptr;
	*p++ = 0;
	return strbuf;
}

int BufferingReader::FillBuffer(ptrdiff_t newpos)
{
	if (newpos > Length) newpos = Length;
	if (newpos <= bufferpos) return 0;
	auto read = baseReader->Read(&buf.writable()[bufferpos], newpos - bufferpos);
	bufferpos += read;
	if (bufferpos == Length)
	{
		// we have read the entire file, so delete our data provider.
		baseReader.reset();
	}
	return newpos == bufferpos ? 0 : -1;
}

ptrdiff_t BufferingReader::Seek(ptrdiff_t offset, int origin)
{
	if (-1 == MemoryReader::Seek(offset, origin)) return -1;
	return FillBuffer(FilePos);
}

ptrdiff_t BufferingReader::Read(void* buffer, ptrdiff_t len)
{
	if (FillBuffer(FilePos + len) < 0) return 0;
	return MemoryReader::Read(buffer, len);
}

char* BufferingReader::Gets(char* strbuf, ptrdiff_t len)
{
	if (FillBuffer(FilePos + len) < 0) return nullptr;
	return MemoryReader::Gets(strbuf, len);
}

//==========================================================================
//
// FileReader
//
// this wraps the different reader types in an object with value semantics.
//
//==========================================================================

bool FileReader::OpenFile(const char *filename, FileReader::Size start, FileReader::Size length, bool buffered)
{
	auto reader = new StdFileReader;
	if (!reader->Open(filename, start, length))
	{
		delete reader;
		return false;
	}
	Close();
	if (buffered) mReader = new BufferingReader(reader);
	else mReader = reader;
	return true;
}

bool FileReader::OpenFilePart(FileReader &parent, FileReader::Size start, FileReader::Size length)
{
	auto reader = new FileReaderRedirect(parent, start, length);
	Close();
	mReader = reader;
	return true;
}

bool FileReader::OpenMemory(const void *mem, FileReader::Size length)
{
	Close();
	mReader = new MemoryReader((const char *)mem, length);
	return true;
}

bool FileReader::OpenMemoryArray(FileData& data)
{
	Close();
	if (data.size() > 0) mReader = new MemoryArrayReader(data);
	return true;
}

FileData FileReader::Read(size_t len)
{
	FileData buffer;
	if (len > 0)
	{
		Size length = mReader->Read(buffer.allocate(len), len);
		if ((size_t)length < len) buffer.allocate(length);
	}
	return buffer;
}

FileData FileReader::ReadPadded(size_t padding)
{
	auto len = GetLength();
	FileData buffer;

	if (len > 0)
	{
		auto p = (char*)buffer.allocate(len + padding);
		Size length = mReader->Read(p, len);
		if (length < len) buffer.clear();
		else memset(p + len, 0, padding);
	}
	return buffer;
}



//==========================================================================
//
// FileWriter (the motivation here is to have a buffer writing subclass)
//
//==========================================================================

bool FileWriter::OpenDirect(const char *filename)
{
	File = myfopen(filename, "wb");
	return (File != nullptr);
}

FileWriter *FileWriter::Open(const char *filename)
{
	FileWriter *fwrit = new FileWriter();
	if (fwrit->OpenDirect(filename))
	{
		return fwrit;
	}
	delete fwrit;
	return nullptr;
}

size_t FileWriter::Write(const void *buffer, size_t len)
{
	if (File != nullptr)
	{
		return fwrite(buffer, 1, len, File);
	}
	else
	{
		return 0;
	}
}

ptrdiff_t FileWriter::Tell()
{
	if (File != nullptr)
	{
		return ftell(File);
	}
	else
	{
		return 0;
	}
}

ptrdiff_t FileWriter::Seek(ptrdiff_t offset, int mode)
{
	if (File != nullptr)
	{
		return fseek(File, offset, mode);
	}
	else
	{
		return 0;
	}
}

size_t FileWriter::Printf(const char *fmt, ...)
{
	char c[300];
	va_list arglist;
	va_start(arglist, fmt);
	auto n = vsnprintf(c, 300, fmt, arglist);
	std::string buf;
	buf.resize(n + 1);
	va_start(arglist, fmt);
	vsnprintf(&buf.front(), n + 1, fmt, arglist);
	va_end(arglist);
	return Write(buf.c_str(), strlen(buf.c_str()));	// Make sure we write no nullptr bytes.
}

size_t BufferWriter::Write(const void *buffer, size_t len)
{
	size_t ofs = mBuffer.size();
	mBuffer.resize(ofs + len);
	memcpy(&mBuffer[ofs], buffer, len);
	return len;
}

}
