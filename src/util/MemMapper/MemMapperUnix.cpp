#include "util/MemMapper/MemMapper.h"

#include <unistd.h>
#include <sys/mman.h>
#include <cerrno>

namespace MemMapper
{
	const size_t sPageSize{ []()
		{
		return (size_t)getpagesize();
	}()
	};

	size_t GetPageSize()
	{
		return sPageSize;
	}

	int GetProt(PAGE_PERMISSION permissionFlags)
	{
		int  p = 0;
		if (permissionFlags == PAGE_PERMISSION::P_NONE)
			return PROT_NONE;
		if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PROT_READ | PROT_WRITE | PROT_EXEC;
		else if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PROT_READ | PROT_WRITE;
		else if (HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_READ) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_WRITE) && !HAS_FLAG(permissionFlags, PAGE_PERMISSION::P_EXECUTE))
			p = PROT_READ;
		else
			cemu_assert_unimplemented();
		return p;
	}

	// mprotect() requires a page-aligned base and operates on whole pages. Apple silicon
	// uses 16 KB pages, so a range whose base or end is only 4 KB-aligned must be widened
	// outward or the call fails with EINVAL and the range is silently left as it was.
	static void AlignToPages(void*& baseAddr, size_t& size)
	{
		const uintptr_t start = (uintptr_t)baseAddr;
		const uintptr_t end = start + size;
		const uintptr_t alignedStart = start & ~(uintptr_t)(sPageSize - 1);
		const uintptr_t alignedEnd = (end + sPageSize - 1) & ~(uintptr_t)(sPageSize - 1);
		baseAddr = (void*)alignedStart;
		size = (size_t)(alignedEnd - alignedStart);
	}

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		return mmap(baseAddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	}

	void FreeReservation(void* baseAddr, size_t size)
	{
		munmap(baseAddr, size);
	}

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation)
	{
		if (!fromReservation)
			return mmap(baseAddr, size, GetProt(permissionFlags), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		void* page = baseAddr;
		size_t pageSize = size;
		AlignToPages(page, pageSize);
		if (mprotect(page, pageSize, GetProt(permissionFlags)) != 0)
		{
			cemuLog_log(LogType::Force, "MemMapper: failed to commit {:#x} bytes at {} (errno {})", size, baseAddr, errno);
			return nullptr;
		}
		// callers expect the original, unaligned address back
		return baseAddr;
	}

	void FreeMemory(void* baseAddr, size_t size, bool fromReservation)
	{
		if (!fromReservation)
		{
			munmap(baseAddr, size);
			return;
		}
		void* page = baseAddr;
		size_t pageSize = size;
		AlignToPages(page, pageSize);
		if (mprotect(page, pageSize, PROT_NONE) != 0)
			cemuLog_log(LogType::Force, "MemMapper: failed to decommit {:#x} bytes at {} (errno {})", size, baseAddr, errno);
	}

};
