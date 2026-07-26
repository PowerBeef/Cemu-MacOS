#pragma once

// Non-recursive lock, backed by os_unfair_lock.
//
// This was previously a true spinlock (atomic exchange + `yield`). That becomes
// actively dangerous once threads run at different QoS classes, which they now do:
// the guest cores and the render thread sit at USER_INTERACTIVE while the recompiler
// and the shader compilers sit at UTILITY. A spin loop performs no priority donation,
// so a USER_INTERACTIVE thread spinning on a lock held by a descheduled UTILITY
// thread spins for a full scheduler quantum -- on a 4P+4E machine with the efficiency
// cluster saturated that is a hang, not a slowdown.
//
// os_unfair_lock donates priority to the owning thread on contention and is about as
// cheap as a spinlock when uncontended. It is also strictly checked: unlocking from a
// thread that does not own the lock traps instead of silently corrupting state.
//
// Note it is NOT recursive -- same as the spinlock it replaces.

#include <os/lock.h>

class FSpinlock
{
public:
	// implement BasicLockable and Lockable
	void lock() const
	{
		os_unfair_lock_lock(&m_lock);
	}

	bool try_lock() const
	{
		return os_unfair_lock_trylock(&m_lock);
	}

	void unlock() const
	{
		os_unfair_lock_unlock(&m_lock);
	}

	// os_unfair_lock exposes no queryable "is it locked" state, by design -- such a
	// query is inherently racy and almost always misused. The supported primitives are
	// ownership assertions, which are what the call sites actually wanted: they trap
	// when the current thread does (or does not) hold the lock.
	void assert_owner() const
	{
		os_unfair_lock_assert_owner(&m_lock);
	}

	void assert_not_owner() const
	{
		os_unfair_lock_assert_not_owner(&m_lock);
	}

private:
	mutable os_unfair_lock m_lock = OS_UNFAIR_LOCK_INIT;
};
