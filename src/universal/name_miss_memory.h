#pragma once

// Names that a lookup did not find.
//
// A lookup by name that finds nothing has walked everything it could have
// matched, and a caller that asks again each frame (a menu expression reading
// a dvar that was never registered, or asking whether a menu of another
// context is open) pays for that walk every time. This remembers "this text
// was not found", together with a stamp the owner makes from whatever would
// have to change before the answer could (a count of registrations, of list
// changes). A remembered miss holds only for the same stamp and the same
// text, byte for byte: the text is kept here, because the caller's pointer
// may hold another name by now. Names of NAME_MISS_TEXT_BYTES or more are
// not remembered.
//
// Any thread may ask and remember at once. An entry is read between two
// readings of its sequence number and dropped if a writer was in it, and one
// writer at a time claims an entry; a writer that cannot claim it skips.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

inline constexpr size_t NAME_MISS_TEXT_WORDS = 11;
inline constexpr size_t NAME_MISS_TEXT_BYTES = NAME_MISS_TEXT_WORDS * sizeof(uint32_t);

template <size_t SLOT_BITS>
class NameMissMemory
{
public:
    // True when this text was remembered as not found under this stamp.
    bool Has(const char *name, uint32_t stamp) const
    {
        uint32_t text[NAME_MISS_TEXT_WORDS];
        if (!Pack(name, text))
            return false;
        const Entry &entry = entries_[Slot(name)];
        const uint32_t before = entry.sequence.load(std::memory_order_acquire);
        if (before & 1)
            return false;
        bool same = entry.stamp.load(std::memory_order_relaxed) == stamp;
        for (size_t word = 0; word < NAME_MISS_TEXT_WORDS; ++word)
            same &= entry.text[word].load(std::memory_order_relaxed) == text[word];
        // The second reading of the sequence number must come after the reads
        // above on every processor, not only in the program's order.
        std::atomic_thread_fence(std::memory_order_acquire);
        return same && entry.sequence.load(std::memory_order_relaxed) == before;
    }

    void Remember(const char *name, uint32_t stamp)
    {
        uint32_t text[NAME_MISS_TEXT_WORDS];
        if (!Pack(name, text))
            return;
        Entry &entry = entries_[Slot(name)];
        uint32_t sequence = entry.sequence.load();
        if ((sequence & 1) || !entry.sequence.compare_exchange_strong(sequence, sequence + 1))
            return;
        entry.stamp.store(stamp, std::memory_order_relaxed);
        for (size_t word = 0; word < NAME_MISS_TEXT_WORDS; ++word)
            entry.text[word].store(text[word], std::memory_order_relaxed);
        entry.sequence.store(sequence + 2);
    }

    // How many entries have ever been written; for checks.
    size_t Used() const
    {
        size_t used = 0;
        for (const Entry &entry : entries_)
            used += entry.sequence.load() != 0;
        return used;
    }

private:
    struct Entry
    {
        std::atomic<uint32_t> sequence; // odd while a writer is in the entry
        std::atomic<uint32_t> stamp;
        std::atomic<uint32_t> text[NAME_MISS_TEXT_WORDS];
    };

    // The text with its terminator, the rest zero, so that equal words mean
    // equal text. False for a null name or one that does not fit.
    static bool Pack(const char *name, uint32_t (&text)[NAME_MISS_TEXT_WORDS])
    {
        if (!name)
            return false;
        char bytes[NAME_MISS_TEXT_BYTES] = {};
        size_t length = 0;
        while (name[length])
        {
            if (length + 1 >= NAME_MISS_TEXT_BYTES)
                return false;
            bytes[length] = name[length];
            ++length;
        }
        memcpy(text, bytes, sizeof(bytes));
        return true;
    }

    static size_t Slot(const char *name)
    {
        return (size_t)(((uint32_t)(uintptr_t)name * 2654435761u) >> (32 - SLOT_BITS));
    }

    Entry entries_[(size_t)1 << SLOT_BITS] = {};
};
