#pragma once

// threads_interlock.h -- portable home for the MSVC "Interlocked" intrinsics.
//
// On Windows this header contributes nothing: <windows.h> (via winnt.h) already
// declares the Interlocked* family, and those definitions are left exactly as
// they are.
//
// On Linux there is no <windows.h> and clang does not supply the unprefixed
// names, so they are implemented here on top of the compiler's __atomic
// builtins.
//
// Every function below reproduces the Win32 return contract, which is what the
// callers depend on:
//
//   InterlockedIncrement / InterlockedDecrement  -> the NEW value
//   InterlockedExchange                          -> the OLD value
//   InterlockedExchangeAdd                       -> the OLD value
//   InterlockedCompareExchange                   -> the OLD (initial) value
//   InterlockedExchangePointer                   -> the OLD pointer
//
// Ordering is __ATOMIC_SEQ_CST throughout. That is the ordering the Win32
// intrinsics imply on x86, so callers that use the result to decide whether they
// won a race keep working.
//
// The operand type is a template parameter rather than a fixed LONG because the
// code base passes a mixture of `long *`, `int *` and `volatile uint32_t *`
// (FastCriticalSection::readCount and FileHandle_s::hasOpenFile are both
// `volatile uint32_t`). On Windows all of those reach the same 4-byte intrinsic,
// so templating keeps the source identical on both platforms instead of forcing
// casts at every call site.

#if defined(__linux__)

namespace qcommon_interlock
{
// Value/compare arguments are passed through a non-deduced context so that the
// operand type is deduced solely from the pointer. That lets callers keep
// writing plain integer literals -- `InterlockedCompareExchange(&w, 1, 0)` on a
// `volatile uint32_t` -- which the Win32 intrinsics accept by conversion.
// Deducting the literal too would find `uint32_t` from the pointer and `int`
// from the literal and reject the call as ambiguous.
template <typename T>
struct same
{
    using type = T;
};
} // namespace qcommon_interlock

// Guarded individually so that a compatibility header may still provide any of
// the names as macros without colliding with what is defined here.
#ifndef InterlockedIncrement
template <typename T>
inline T InterlockedIncrement(volatile T *addend)
{
    static_assert(sizeof(T) == 4, "InterlockedIncrement needs a 4-byte integer type");
    return __atomic_add_fetch(addend, T(1), __ATOMIC_SEQ_CST);
}
#endif

#ifndef InterlockedDecrement
template <typename T>
inline T InterlockedDecrement(volatile T *addend)
{
    static_assert(sizeof(T) == 4, "InterlockedDecrement needs a 4-byte integer type");
    return __atomic_sub_fetch(addend, T(1), __ATOMIC_SEQ_CST);
}
#endif

#ifndef InterlockedExchange
template <typename T>
inline T InterlockedExchange(volatile T *target, typename qcommon_interlock::same<T>::type value)
{
    static_assert(sizeof(T) == 4, "InterlockedExchange needs a 4-byte integer type");
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
#endif

#ifndef InterlockedExchangeAdd
template <typename T>
inline T InterlockedExchangeAdd(volatile T *addend, typename qcommon_interlock::same<T>::type increment)
{
    static_assert(sizeof(T) == 4, "InterlockedExchangeAdd needs a 4-byte integer type");
    return __atomic_fetch_add(addend, increment, __ATOMIC_SEQ_CST);
}
#endif

#ifndef InterlockedCompareExchange
template <typename T>
inline T InterlockedCompareExchange(
    volatile T *destination,
    typename qcommon_interlock::same<T>::type exchange,
    typename qcommon_interlock::same<T>::type comparand)
{
    static_assert(sizeof(T) == 4, "InterlockedCompareExchange needs a 4-byte integer type");

    // __atomic_compare_exchange_n writes the observed value back through
    // `expected`. On success that is the comparand, on failure it is whatever was
    // actually in the slot, so returning it gives the Win32 "old value" result in
    // both cases.
    T expected = comparand;
    __atomic_compare_exchange_n(
        destination, &expected, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
#endif

#ifndef InterlockedExchangePointer
template <typename T>
inline T *InterlockedExchangePointer(T *volatile *target, typename qcommon_interlock::same<T *>::type value)
{
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
#endif

#endif // __linux__
