// VirtualAlloc/VirtualFree for the native Linux build. The contract and the
// backing model are described in <linux/sys_virtualmem.h>; this file is the
// bookkeeping that keeps commit, decommit and release inside the regions this
// module actually handed out.
//
// The one invariant everything else follows from: this module never calls
// mmap with an address, so MAP_FIXED never appears and no mapping that belongs
// to something else can be replaced. Commit and decommit translate into
// mprotect/madvise, which are only ever pointed at a region found in the
// reservation table under s_lock, and release is the matching munmap. Address
// arithmetic is done in 64 bits and compared against the host's pointer width,
// so a range that would wrap the 32-bit address space is rejected rather than
// truncated into something that looks valid.

// madvise/MADV_DONTNEED and MAP_ANONYMOUS are guarded by the glibc feature
// macros. The engine build defines _GNU_SOURCE globally, but this file does not
// depend on that: the define has to precede every system header, so it is first.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <linux/sys_virtualmem.h>

#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>

namespace
{
    // -----------------------------------------------------------------------
    // Page geometry
    //
    // Windows rounds a region to the 4 KiB page size and requires bases on the
    // 64 KiB allocation granularity. The Linux mapping is rounded to the host's
    // page size instead, which is what the kernel will round it to anyway; the
    // granularity requirement is a Windows placement detail that has no
    // meaning here, because the engine treats the returned base as opaque.
    // -----------------------------------------------------------------------

    std::uintptr_t QueryPageSize()
    {
        const long size = sysconf(_SC_PAGESIZE);
        // A usable value is a positive power of two. The fallback keeps the
        // masking helpers below from producing a mask of -1 if sysconf were
        // ever to fail.
        if (size <= 0 || (static_cast<unsigned long>(size) & (static_cast<unsigned long>(size) - 1)) != 0)
            return 4096u;
        return static_cast<std::uintptr_t>(size);
    }

    std::uintptr_t PageSize()
    {
        // Function-local static: initialised once, thread-safely, on first use,
        // so there is no static-initialisation-order dependency on sysconf.
        static const std::uintptr_t pageSize = QueryPageSize();
        return pageSize;
    }

    // A value that fits the host pointer width. On the -m32 engine build this
    // rejects anything at or past 4 GiB, which is the case where a rounded end
    // would have wrapped the address space.
    bool Representable(std::uint64_t value)
    {
        return value <= static_cast<std::uint64_t>(UINTPTR_MAX);
    }

    // a + b without wrapping. Used for start + size, where a wrap would make a
    // range that runs off the end of the address space look like a short one.
    bool AddChecked(std::uint64_t a, std::uint64_t b, std::uint64_t *out)
    {
        if (a > UINT64_MAX - b)
            return false;
        *out = a + b;
        return true;
    }

    // Windows page semantics: an operation covers whole pages, so the range
    // starts at the floor of the first address and ends at the ceiling of the
    // last byte plus one. Both ends come back page-aligned.
    std::uint64_t PageFloor(std::uint64_t value)
    {
        return value & ~static_cast<std::uint64_t>(PageSize() - 1);
    }

    bool PageCeilChecked(std::uint64_t value, std::uint64_t *out)
    {
        const std::uint64_t mask = static_cast<std::uint64_t>(PageSize() - 1);
        if (value > UINT64_MAX - mask)
            return false;
        *out = (value + mask) & ~mask;
        return true;
    }

    // -----------------------------------------------------------------------
    // Reservation table
    // -----------------------------------------------------------------------

    struct Reservation
    {
        void *base;             // as returned by mmap, therefore page-aligned
        std::size_t length;     // page-rounded, exactly the length passed to mmap
        Reservation *next;
    };

    pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
    Reservation *s_reservations = nullptr;
    std::uint32_t s_reservationCount = 0;

    // True when [begin, end) lies strictly inside one reservation. Bounds are
    // compared as 64-bit values so a caller's range can never "fit" by virtue
    // of having wrapped.
    bool Contains(const Reservation &reservation, std::uint64_t begin, std::uint64_t end)
    {
        const std::uint64_t base = reinterpret_cast<std::uintptr_t>(reservation.base);
        const std::uint64_t past = base + static_cast<std::uint64_t>(reservation.length);
        return begin >= base && end <= past && begin < end;
    }

    // The three helpers below require s_lock to be held.

    Reservation *FindContaining(std::uint64_t begin, std::uint64_t end)
    {
        for (Reservation *entry = s_reservations; entry != nullptr; entry = entry->next)
        {
            if (Contains(*entry, begin, end))
                return entry;
        }
        return nullptr;
    }

    void Attach(Reservation *record)
    {
        record->next = s_reservations;
        s_reservations = record;
        ++s_reservationCount;
    }

    // Unlinks the record for `base` and hands it to the caller to free.
    Reservation *Detach(void *base)
    {
        Reservation **link = &s_reservations;
        while (*link != nullptr)
        {
            if ((*link)->base == base)
            {
                Reservation *found = *link;
                *link = found->next;
                found->next = nullptr;
                --s_reservationCount;
                return found;
            }
            link = &(*link)->next;
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Error reporting
    // -----------------------------------------------------------------------

    // Publishes the Win32 code for the system call that just failed. errno
    // keeps the raw cause, which is what makes a backend failure diagnosable.
    void PublishErrno(DWORD fallback)
    {
        DWORD error;
        switch (errno)
        {
        case ENOMEM:
            error = ERROR_NOT_ENOUGH_MEMORY;
            break;
        case EINVAL:
            error = ERROR_INVALID_PARAMETER;
            break;
        case EACCES:
        case EPERM:
            error = ERROR_ACCESS_DENIED;
            break;
        default:
            error = fallback;
            break;
        }
        SetLastError(error);
    }

    // Protection actually requested of the kernel. PAGE_NOACCESS is accepted
    // because callers pass the reserve protection through on the commit call;
    // it maps to a commit that leaves the pages inaccessible.
    int ProtFor(DWORD protect)
    {
        return (protect == PAGE_READWRITE) ? (PROT_READ | PROT_WRITE) : PROT_NONE;
    }

    bool ProtectSupported(DWORD protect)
    {
        return protect == PAGE_READWRITE || protect == PAGE_NOACCESS;
    }

    // Translates a caller's (address, size) pair into the page-aligned range
    // Windows would operate on. Rejects a zero size, a sum that overflows, and a
    // rounded range that does not fit the pointer width.
    bool ResolveRange(const void *address, SIZE_T size, std::uint64_t *begin, std::uint64_t *end)
    {
        if (size == 0)
            return false;

        const std::uint64_t start = reinterpret_cast<std::uintptr_t>(address);
        std::uint64_t last = 0;
        if (!AddChecked(start, static_cast<std::uint64_t>(size), &last) || !Representable(last))
            return false;

        std::uint64_t limit = 0;
        if (!PageCeilChecked(last, &limit) || !Representable(limit))
            return false;

        *begin = PageFloor(start);
        *end = limit;
        return true;
    }

    // Reserves fresh address space for VirtualAlloc(NULL, ...).
    LPVOID ReserveNew(SIZE_T size, DWORD allocationType, DWORD protect)
    {
        std::uint64_t length = 0;
        if (!PageCeilChecked(static_cast<std::uint64_t>(size), &length) || !Representable(length) || length == 0)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }

        // MEM_COMMIT with a null address is reserve-and-commit in one step,
        // which is how a caller that does not need the reserved-then-committed
        // split asks for memory.
        const int prot = ((allocationType & MEM_COMMIT) != 0) ? ProtFor(protect) : PROT_NONE;

        void *base = mmap(nullptr, static_cast<std::size_t>(length), prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED)
        {
            PublishErrno(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }

        Reservation *record = static_cast<Reservation *>(std::calloc(1, sizeof(Reservation)));
        if (record == nullptr)
        {
            munmap(base, static_cast<std::size_t>(length));
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        record->base = base;
        record->length = static_cast<std::size_t>(length);

        pthread_mutex_lock(&s_lock);
        Attach(record);
        pthread_mutex_unlock(&s_lock);

        return base;
    }

    // Makes an existing reservation's pages writable (VirtualAlloc(ptr, ...)).
    LPVOID CommitExisting(void *address, SIZE_T size, DWORD allocationType, DWORD protect)
    {
        // Reserving at a caller-chosen address would have to replace whatever
        // is already mapped there, so it is refused instead of done with
        // MAP_FIXED. No caller in the engine needs it.
        if ((allocationType & MEM_RESERVE) != 0)
        {
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
        if (!ProtectSupported(protect))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }

        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        if (!ResolveRange(address, size, &begin, &end))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }

        LPVOID result = nullptr;
        pthread_mutex_lock(&s_lock);
        // The lock is held across mprotect so a concurrent release cannot unmap
        // the range between the lookup and the call.
        if (FindContaining(begin, end) == nullptr)
        {
            SetLastError(ERROR_INVALID_ADDRESS);
        }
        else if (mprotect(reinterpret_cast<void *>(static_cast<std::uintptr_t>(begin)),
                          static_cast<std::size_t>(end - begin), ProtFor(protect)) != 0)
        {
            PublishErrno(ERROR_INVALID_ADDRESS);
        }
        else
        {
            // Windows returns the base of the committed region; every caller in
            // the engine only tests for non-null, and handing back the address
            // the caller passed keeps `ptr` usable as the same pointer it
            // always was.
            result = address;
        }
        pthread_mutex_unlock(&s_lock);
        return result;
    }

    // MEM_DECOMMIT: discard the contents and take the pages away again.
    BOOL Decommit(void *address, SIZE_T size)
    {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        if (!ResolveRange(address, size, &begin, &end))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        const std::size_t length = static_cast<std::size_t>(end - begin);
        void *const start = reinterpret_cast<void *>(static_cast<std::uintptr_t>(begin));

        BOOL ok = FALSE;
        pthread_mutex_lock(&s_lock);
        if (FindContaining(begin, end) == nullptr)
        {
            // Also covers an interior pointer whose page rounds outside the
            // reservation, and any address from an unrelated mapping.
            SetLastError(ERROR_INVALID_ADDRESS);
        }
        else if (madvise(start, length, MADV_DONTNEED) != 0)
        {
            // The ranges the engine decommits are anonymous private mappings,
            // for which MADV_DONTNEED is always supported and leaves the next
            // access zero-filled. A failure here is reported rather than
            // papered over.
            PublishErrno(ERROR_INVALID_ADDRESS);
        }
        else if (mprotect(start, length, PROT_NONE) != 0)
        {
            PublishErrno(ERROR_INVALID_ADDRESS);
        }
        else
        {
            ok = TRUE;
        }
        pthread_mutex_unlock(&s_lock);
        return ok;
    }

    // MEM_RELEASE: unmap exactly the region that was reserved.
    BOOL Release(void *address, SIZE_T size)
    {
        // Windows requires a zero size with MEM_RELEASE: the length is the
        // reservation's, not the caller's, and accepting a caller-supplied one
        // would allow releasing a part of a region.
        if (size != 0)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        BOOL ok = FALSE;
        pthread_mutex_lock(&s_lock);
        Reservation *record = Detach(address);
        if (record == nullptr)
        {
            // An interior address, a repeated release, or a pointer this module
            // never handed out. Nothing is unmapped in that case.
            SetLastError(ERROR_INVALID_ADDRESS);
        }
        else if (munmap(record->base, record->length) != 0)
        {
            // Extremely unlikely for a mapping this module owns; put the record
            // back so the table keeps describing the address space as it is.
            PublishErrno(ERROR_NOT_ENOUGH_MEMORY);
            Attach(record);
        }
        else
        {
            std::free(record);
            ok = TRUE;
        }
        pthread_mutex_unlock(&s_lock);
        return ok;
    }
} // namespace

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect)
{
    const DWORD supportedTypes = MEM_COMMIT | MEM_RESERVE;
    if ((allocationType & ~supportedTypes) != 0 || (allocationType & supportedTypes) == 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!ProtectSupported(protect))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (address == nullptr)
        return ReserveNew(size, allocationType, protect);

    // A non-null address can only be committed into: reserving there would have
    // to replace an existing mapping.
    return CommitExisting(address, size, allocationType, protect);
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD freeType)
{
    const DWORD supportedTypes = MEM_DECOMMIT | MEM_RELEASE;
    if ((freeType & ~supportedTypes) != 0 || (freeType & supportedTypes) == 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((freeType & MEM_DECOMMIT) != 0 && (freeType & MEM_RELEASE) != 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (address == nullptr)
    {
        SetLastError(ERROR_INVALID_ADDRESS);
        return FALSE;
    }

    if ((freeType & MEM_RELEASE) != 0)
        return Release(address, size);
    return Decommit(address, size);
}

uint32_t Sys_VirtualMemReservationCount()
{
    pthread_mutex_lock(&s_lock);
    const std::uint32_t count = s_reservationCount;
    pthread_mutex_unlock(&s_lock);
    return count;
}

// ---------------------------------------------------------------------------
// Selftest
//
// Opt-in only: the engine build never defines KISAK_SYS_VIRTUALMEM_SELFTEST, so
// none of this is compiled into the dedicated server. Build the test with
//
//   clang++ -m32 -std=gnu++20 -fms-extensions -D_GNU_SOURCE \
//           -DKISAK_SYS_VIRTUALMEM_SELFTEST -pthread \
//           -I<repo>/src src/linux/sys_virtualmem.cpp -o /tmp/sys_virtualmem_test
//
// The fault assertions need a process that is allowed to die, so each one runs
// in a forked child: a child that touches a page it should not be able to touch
// must be killed by SIGSEGV, and a child that survives is a failure.
// ---------------------------------------------------------------------------
#ifdef KISAK_SYS_VIRTUALMEM_SELFTEST

#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#include <cstdio>

#include <fcntl.h>

namespace
{
    int s_failures = 0;

    void ReportFailure(int line, const char *expression)
    {
        std::printf("FAIL line %d: %s\n", line, expression);
        ++s_failures;
    }

    // Rounds a size up the way the module does, so the test does not assume the
    // host page size.
    std::size_t RoundUp(std::size_t bytes)
    {
        const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        return ((bytes + page - 1) / page) * page;
    }

    bool DiedOfFault(int status)
    {
        if (!WIFSIGNALED(status))
            return false;
        const int signal = WTERMSIG(status);
        return signal == SIGSEGV || signal == SIGBUS;
    }

    // Runs `touch` in a child and reports whether the child died of a fault.
    bool FaultsWhen(bool (*touch)())
    {
        const pid_t child = fork();
        if (child < 0)
            return false;
        if (child == 0)
        {
            // The expected fault would otherwise print a page of diagnostics
            // for every case.
            const int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0)
                dup2(devnull, STDERR_FILENO);
            // Die from the fault itself. A sanitizer that installs its own
            // SIGSEGV handler would otherwise report the access and exit with
            // its own status code, which says nothing about the fault.
            signal(SIGSEGV, SIG_DFL);
            signal(SIGBUS, SIG_DFL);
            touch();
            _exit(0);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child)
            return false;
        return DiedOfFault(status);
    }

    volatile unsigned char *g_touchTarget = nullptr;

    bool TouchRead()
    {
        // The value is consumed through a volatile store so the compiler cannot
        // drop the load that is supposed to fault.
        volatile unsigned char sink = *g_touchTarget;
        (void)sink;
        return true;
    }

    bool TouchWrite()
    {
        *g_touchTarget = 0x5A;
        return true;
    }

    bool FaultsOnRead(void *address)
    {
        g_touchTarget = static_cast<volatile unsigned char *>(address);
        return FaultsWhen(TouchRead);
    }

    bool FaultsOnWrite(void *address)
    {
        g_touchTarget = static_cast<volatile unsigned char *>(address);
        return FaultsWhen(TouchWrite);
    }

    void Check(bool condition, int line, const char *expression)
    {
        if (!condition)
            ReportFailure(line, expression);
    }
} // namespace

#define CHECK(expression) Check((expression), __LINE__, #expression)

namespace
{
    struct ThreadArgs
    {
        std::uint32_t iterations;
    };

    // Reserve, commit, fill, decommit, recommit, verify zeroes, release. Runs
    // concurrently with its siblings; the point is that the reservation table
    // stays consistent under overlapping traffic, not throughput.
    //
    // A failing thread returns the message through its join value rather than
    // reporting from inside the thread, so the counters stay single-threaded.
    void *ChurnThread(void *raw)
    {
        ThreadArgs *args = static_cast<ThreadArgs *>(raw);
        const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        const std::size_t length = page * 4;
        const char *failure = nullptr;

        for (std::uint32_t i = 0; i < args->iterations; ++i)
        {
            unsigned char *base = static_cast<unsigned char *>(
                VirtualAlloc(nullptr, static_cast<SIZE_T>(length), MEM_RESERVE, PAGE_READWRITE));
            if (base == nullptr)
            {
                failure = "reserve failed in a churn thread";
                break;
            }
            if (VirtualAlloc(base, static_cast<SIZE_T>(length), MEM_COMMIT, PAGE_READWRITE) == nullptr)
            {
                failure = "commit failed in a churn thread";
                break;
            }
            memset(base, 0xA5, length);

            if (!VirtualFree(base, static_cast<SIZE_T>(page * 2), MEM_DECOMMIT))
                failure = "decommit failed in a churn thread";
            if (VirtualAlloc(base, static_cast<SIZE_T>(page * 2), MEM_COMMIT, PAGE_READWRITE) == nullptr)
                failure = "recommit failed in a churn thread";
            if (failure == nullptr)
            {
                for (std::size_t offset = 0; offset < page * 2; ++offset)
                {
                    if (base[offset] != 0)
                    {
                        failure = "a recommitted page was not zeroed in a churn thread";
                        break;
                    }
                }
            }
            if (!VirtualFree(base, 0, MEM_RELEASE))
                failure = "release failed in a churn thread";
            if (failure != nullptr)
                break;
        }
        return const_cast<char *>(failure);
    }
} // namespace

int main()
{
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const std::uint32_t baseline = Sys_VirtualMemReservationCount();

    std::printf("page size %zu, baseline reservations %u\n", page, baseline);

    // --- A reservation is address space only: every page faults -------------
    {
        unsigned char *base = static_cast<unsigned char *>(
            VirtualAlloc(nullptr, static_cast<SIZE_T>(page * 3), MEM_RESERVE, PAGE_READWRITE));
        CHECK(base != nullptr);
        CHECK((reinterpret_cast<std::uintptr_t>(base) & (page - 1)) == 0);
        CHECK(Sys_VirtualMemReservationCount() == baseline + 1);

        CHECK(FaultsOnRead(base));
        CHECK(FaultsOnRead(base + page));
        CHECK(FaultsOnWrite(base + page * 2));

        // --- Commit makes exactly the pages it covers writable --------------
        // An unaligned request that spans a page boundary covers both pages,
        // and leaves the next one alone.
        CHECK(VirtualAlloc(base + page - 1, 2, MEM_COMMIT, PAGE_READWRITE) == static_cast<LPVOID>(base + page - 1));

        volatile unsigned char *writable = base;
        writable[0] = 1;
        writable[page - 1] = 2;
        writable[page] = 3;
        writable[page * 2 - 1] = 4;
        CHECK(writable[0] == 1 && writable[page - 1] == 2 && writable[page] == 3 && writable[page * 2 - 1] == 4);
        CHECK(!FaultsOnRead(base));
        CHECK(!FaultsOnWrite(base + page));
        // The third page was outside the request and is still reserved.
        CHECK(FaultsOnWrite(base + page * 2));

        // --- Decommit takes the pages away again ----------------------------
        CHECK(VirtualFree(base, static_cast<SIZE_T>(page * 2), MEM_DECOMMIT) == TRUE);
        CHECK(FaultsOnRead(base));
        CHECK(FaultsOnWrite(base + page));
        CHECK(FaultsOnRead(base + page * 2 - 1));

        // --- Recommitting yields zeroes, not the old contents ---------------
        CHECK(VirtualAlloc(base, static_cast<SIZE_T>(page * 2), MEM_COMMIT, PAGE_READWRITE) == static_cast<LPVOID>(base));
        int nonzero = 0;
        for (std::size_t offset = 0; offset < page * 2; ++offset)
        {
            if (base[offset] != 0)
                ++nonzero;
        }
        CHECK(nonzero == 0);

        // Committing an already-committed range keeps its contents, as on
        // Windows: it is a protection call, not an allocation.
        base[7] = 0x11;
        CHECK(VirtualAlloc(base, static_cast<SIZE_T>(page), MEM_COMMIT, PAGE_READWRITE) == static_cast<LPVOID>(base));
        CHECK(base[7] == 0x11);

        CHECK(VirtualFree(base, 0, MEM_RELEASE) == TRUE);
        CHECK(Sys_VirtualMemReservationCount() == baseline);
        // The address is unmapped now, so touching it faults.
        CHECK(FaultsOnRead(base));
    }

    // --- Releasing must not disturb a neighbour -----------------------------
    {
        const std::size_t length = page * 2;
        unsigned char *first = static_cast<unsigned char *>(
            VirtualAlloc(nullptr, static_cast<SIZE_T>(length), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        unsigned char *second = static_cast<unsigned char *>(
            VirtualAlloc(nullptr, static_cast<SIZE_T>(length), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        CHECK(first != nullptr && second != nullptr);
        memset(first, 0x22, length);
        memset(second, 0x33, length);
        CHECK(Sys_VirtualMemReservationCount() == baseline + 2);

        // A commit that would run past the end of the first reservation is
        // refused, and the second reservation keeps its contents.
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(first + length - page, static_cast<SIZE_T>(page * 2), MEM_COMMIT, PAGE_READWRITE) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_ADDRESS);
        CHECK(second[0] == 0x33 && second[length - 1] == 0x33);

        CHECK(VirtualFree(first, 0, MEM_RELEASE) == TRUE);
        CHECK(second[0] == 0x33 && second[length - 1] == 0x33);
        CHECK(Sys_VirtualMemReservationCount() == baseline + 1);
        CHECK(VirtualFree(second, 0, MEM_RELEASE) == TRUE);
        CHECK(Sys_VirtualMemReservationCount() == baseline);
    }

    // --- Invalid arguments ------------------------------------------------
    {
        unsigned char *base = static_cast<unsigned char *>(
            VirtualAlloc(nullptr, static_cast<SIZE_T>(page * 2), MEM_RESERVE, PAGE_READWRITE));
        CHECK(base != nullptr);
        CHECK(Sys_VirtualMemReservationCount() == baseline + 1);

        // Zero sizes.
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(nullptr, 0, MEM_RESERVE, PAGE_READWRITE) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(base, 0, MEM_COMMIT, PAGE_READWRITE) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualFree(base, 0, MEM_DECOMMIT) == FALSE);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);

        // Interior and repeated release, and a pointer this module never issued.
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualFree(base + page, 0, MEM_RELEASE) == FALSE);
        CHECK(GetLastError() == ERROR_INVALID_ADDRESS);
        int stackValue = 0;
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualFree(&stackValue, 0, MEM_RELEASE) == FALSE);
        CHECK(GetLastError() == ERROR_INVALID_ADDRESS);
        CHECK(Sys_VirtualMemReservationCount() == baseline + 1);

        // Release with a non-zero size, and mutually exclusive free types.
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualFree(base, static_cast<SIZE_T>(page), MEM_RELEASE) == FALSE);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualFree(base, 0, MEM_DECOMMIT | MEM_RELEASE) == FALSE);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);

        // Commit outside the reservation, before it and after it.
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(base - page, static_cast<SIZE_T>(page), MEM_COMMIT, PAGE_READWRITE) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_ADDRESS);
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(base + page * 2, static_cast<SIZE_T>(page), MEM_COMMIT, PAGE_READWRITE) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_ADDRESS);
        // Decommit of a range that leaves the reservation.
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualFree(base + page, static_cast<SIZE_T>(page * 2), MEM_DECOMMIT) == FALSE);
        CHECK(GetLastError() == ERROR_INVALID_ADDRESS);

        // Reserving at a fixed address is refused rather than done with
        // MAP_FIXED.
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(base, static_cast<SIZE_T>(page), MEM_RESERVE, PAGE_READWRITE) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_ADDRESS);

        // Unknown flag and protection values.
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(nullptr, static_cast<SIZE_T>(page), 0x80000u /* MEM_RESET */, PAGE_READWRITE) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(base, static_cast<SIZE_T>(page), MEM_COMMIT, PAGE_READONLY) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);

        // Overflow: a size that makes start + size wrap the 32-bit address
        // space, and a rounded end that no longer fits a pointer.
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(reinterpret_cast<void *>(static_cast<std::uintptr_t>(0xFFFFF000u)),
                           static_cast<SIZE_T>(0x2000u), MEM_COMMIT, PAGE_READWRITE) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(base, static_cast<SIZE_T>(0xFFFFFFFFu), MEM_COMMIT, PAGE_READWRITE) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualFree(reinterpret_cast<void *>(static_cast<std::uintptr_t>(0xFFFFF000u)),
                          static_cast<SIZE_T>(0x2000u), MEM_DECOMMIT) == FALSE);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
        SetLastError(ERROR_SUCCESS);
        CHECK(VirtualAlloc(nullptr, static_cast<SIZE_T>(0xFFFFFFFFu), MEM_RESERVE, PAGE_READWRITE) == nullptr);
        CHECK(GetLastError() == ERROR_INVALID_PARAMETER);

        // Nothing above disturbed the live reservation.
        CHECK(Sys_VirtualMemReservationCount() == baseline + 1);
        CHECK(VirtualAlloc(base, static_cast<SIZE_T>(page), MEM_COMMIT, PAGE_READWRITE) == static_cast<LPVOID>(base));
        base[0] = 0x44;
        CHECK(base[0] == 0x44);
        CHECK(VirtualFree(base, 0, MEM_RELEASE) == TRUE);
        CHECK(Sys_VirtualMemReservationCount() == baseline);
    }

    // --- Unaligned commit/decommit boundaries ------------------------------
    {
        const std::size_t length = RoundUp(4 * page);
        unsigned char *base = static_cast<unsigned char *>(
            VirtualAlloc(nullptr, static_cast<SIZE_T>(length), MEM_RESERVE, PAGE_READWRITE));
        CHECK(base != nullptr);

        // Commit one byte straddling the first page boundary: both pages must
        // become writable, the third must not.
        CHECK(VirtualAlloc(base + page - 1, 2, MEM_COMMIT, PAGE_READWRITE) == static_cast<LPVOID>(base + page - 1));
        volatile unsigned char *writable = base;
        writable[0] = 1;
        writable[page] = 1;
        CHECK(!FaultsOnWrite(base));
        CHECK(!FaultsOnWrite(base + page));
        CHECK(FaultsOnWrite(base + page * 2));

        // Decommit one byte straddling the boundary: both pages go back.
        CHECK(VirtualFree(base + page - 1, 2, MEM_DECOMMIT) == TRUE);
        CHECK(FaultsOnRead(base));
        CHECK(FaultsOnRead(base + page));
        CHECK(FaultsOnRead(base + page * 2));

        // A single-byte commit at the last byte of the reservation must not
        // round past its end: its own page becomes writable and the page
        // before it stays reserved.
        CHECK(VirtualAlloc(base + length - 1, 1, MEM_COMMIT, PAGE_READWRITE) == static_cast<LPVOID>(base + length - 1));
        writable[length - 1] = 9;
        CHECK(!FaultsOnWrite(base + length - 1));
        CHECK(FaultsOnRead(base + length - page - 1));
        CHECK(VirtualFree(base + length - 1, 1, MEM_DECOMMIT) == TRUE);
        CHECK(FaultsOnRead(base + length - 1));

        // Decommit of pages that were never committed is not an error.
        CHECK(VirtualFree(base, static_cast<SIZE_T>(length), MEM_DECOMMIT) == TRUE);
        CHECK(VirtualFree(base, 0, MEM_RELEASE) == TRUE);
    }

    // --- Reserve and commit in one call ------------------------------------
    {
        unsigned char *base = static_cast<unsigned char *>(
            VirtualAlloc(nullptr, static_cast<SIZE_T>(page), MEM_COMMIT, PAGE_READWRITE));
        CHECK(base != nullptr);
        memset(base, 0x77, page);
        CHECK(base[page - 1] == 0x77);
        CHECK(VirtualFree(base, 0, MEM_RELEASE) == TRUE);

        unsigned char *both = static_cast<unsigned char *>(
            VirtualAlloc(nullptr, static_cast<SIZE_T>(page), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        CHECK(both != nullptr);
        memset(both, 0x78, page);
        CHECK(both[0] == 0x78);
        CHECK(VirtualFree(both, 0, MEM_RELEASE) == TRUE);
    }

    // --- Concurrent alloc/free ---------------------------------------------
    {
        enum { kThreads = 4 };
        pthread_t threads[kThreads];
        ThreadArgs args;
        args.iterations = 200;
        int created = 0;
        for (int i = 0; i < kThreads; ++i)
        {
            if (pthread_create(&threads[i], nullptr, ChurnThread, &args) == 0)
                ++created;
            else
                break;
        }
        for (int i = 0; i < created; ++i)
        {
            void *outcome = nullptr;
            pthread_join(threads[i], &outcome);
            if (outcome != nullptr)
                Check(false, __LINE__, static_cast<const char *>(outcome));
        }
        CHECK(created == kThreads);
        CHECK(Sys_VirtualMemReservationCount() == baseline);
    }

    if (s_failures == 0)
        std::printf("all virtual memory checks passed\n");
    else
        std::printf("%d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}

#endif // KISAK_SYS_VIRTUALMEM_SELFTEST
