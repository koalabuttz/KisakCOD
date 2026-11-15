// The spots of a map, and which of them see each other (docs/bots.md, phase
// 4b): what a player learns of a map by playing it a few times, worked out
// once per map.
//
// The spots are places on the navigation mesh, spread out over the map. For
// every pair the table holds whether a player standing at one sees the head
// of a player standing at the other, through the map's solid geometry. That
// is a few hundred thousand lines, so the table is filled a few hundred lines
// a server frame (Step), and the bots use it once it is ready:
//
//   cover      spots near the bot that the spot nearest the threat does not
//              see (CoverFrom): far better odds than trying places at random;
//   overwatch  spots that see a place (an objective, where the team saw
//              enemies) and that few others see (Overwatch): where an anchor
//              or a defender holds.
//
// A spot stands for the area around it, so a bot checks the place it picked
// with a real trace before it relies on it.
//
// No engine call and no engine type: scripts/tests/test_bot_team.py runs it
// on the host.

#pragma once

#include "bot_mind.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace kisak::bots
{

class SpotTable
{
public:
    static constexpr int kMaxSpots = 512;
    // The eye of a standing player above their feet.
    static constexpr float kEye = 60.0f;
    // A threat or a target further than this from every spot is off the table.
    static constexpr float kNearReach = 320.0f;
    // A place to watch from is not on top of what it watches.
    static constexpr float kTooClose = 200.0f;

    void Clear()
    {
        m_count = 0;
        m_a = 0;
        m_b = 1;
        m_ready = false;
        m_lines = 0;
    }

    // Starts the table on `count` places (feet on the mesh), at most kMaxSpots.
    void Begin(const float (*places)[3], int count)
    {
        Clear();
        if (count > kMaxSpots)
            count = kMaxSpots;
        m_count = count > 0 ? count : 0;
        std::memcpy(m_places, places, sizeof(float) * 3 * static_cast<size_t>(m_count));
        std::memset(m_sees, 0, sizeof(m_sees));
        std::memset(m_seen, 0, sizeof(m_seen));
        m_ready = m_count < 2;
    }

    // Traces up to `budget` more pairs. True once the table is complete.
    bool Step(ClearLine clear, void *context, int budget)
    {
        while (!m_ready && budget-- > 0)
        {
            const float from[3] = {m_places[m_a][0], m_places[m_a][1], m_places[m_a][2] + kEye};
            const float to[3] = {m_places[m_b][0], m_places[m_b][1], m_places[m_b][2] + kEye};
            ++m_lines;
            if (clear(from, to, context))
            {
                Mark(m_a, m_b);
                Mark(m_b, m_a);
                ++m_seen[m_a];
                ++m_seen[m_b];
            }
            if (++m_b >= m_count)
            {
                ++m_a;
                m_b = m_a + 1;
                if (m_b >= m_count)
                    m_ready = true;
            }
        }
        return m_ready;
    }

    bool Ready() const { return m_ready && m_count > 0; }
    int Count() const { return m_count; }
    int Lines() const { return m_lines; }
    const float *Place(int spot) const { return m_places[spot]; }

    bool Sees(int a, int b) const
    {
        return a == b || (m_sees[a][b >> 5] >> (b & 31)) & 1u;
    }

    // How many other spots see this one.
    int Seen(int spot) const { return m_seen[spot]; }

    // The spot nearest `feet` within `reach`, or -1.
    int Nearest(const float *feet, float reach = kNearReach) const
    {
        int best = -1;
        float bestDistance = reach;
        for (int i = 0; i < m_count; ++i)
        {
            const float distance = Distance3(m_places[i], feet);
            if (distance < bestDistance)
            {
                best = i;
                bestDistance = distance;
            }
        }
        return best;
    }

    // Spots within `radius` of `from` (feet) that the spot nearest `threat`
    // (an eye) does not see, nearest first. Returns how many, 0 when the table
    // is not ready or the threat is off it.
    int CoverFrom(const float *from, const float *threat, float radius, int *out, int max) const
    {
        if (!Ready())
            return 0;
        const float threatFeet[3] = {threat[0], threat[1], threat[2] - kEye};
        const int seer = Nearest(threatFeet);
        if (seer < 0)
            return 0;
        int found = 0;
        float distances[kMaxSpots];
        for (int i = 0; i < m_count; ++i)
        {
            if (Sees(seer, i))
                continue;
            const float distance = Distance2(m_places[i], from);
            if (distance > radius || std::fabs(m_places[i][2] - from[2]) > radius * 0.5f)
                continue;
            found = Insert(out, distances, found, max, i, distance);
        }
        return found;
    }

    // Spots within `radius` of `near` (feet) that see the spot nearest
    // `target` (feet), the least seen first (the most sheltered places to watch
    // from), those on top of the target last. Returns how many.
    int Overwatch(const float *target, const float *near, float radius, int *out, int max) const
    {
        if (!Ready())
            return 0;
        const int watched = Nearest(target);
        if (watched < 0)
            return 0;
        int found = 0;
        float scores[kMaxSpots];
        for (int i = 0; i < m_count; ++i)
        {
            if (i == watched || !Sees(i, watched))
                continue;
            const float distance = Distance2(m_places[i], near);
            if (distance > radius)
                continue;
            // Few lines on it, and not on top of the target.
            const bool onTop = Distance2(m_places[i], m_places[watched]) < kTooClose;
            const float score = static_cast<float>(m_seen[i] + (onTop ? kMaxSpots : 0));
            found = Insert(out, scores, found, max, i, score);
        }
        return found;
    }

private:
    void Mark(int a, int b)
    {
        m_sees[a][b >> 5] |= 1u << (b & 31);
    }

    static float Distance2(const float *a, const float *b)
    {
        return std::hypot(a[0] - b[0], a[1] - b[1]);
    }

    static float Distance3(const float *a, const float *b)
    {
        const float dz = a[2] - b[2];
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + dz * dz);
    }

    // Keeps the `max` lowest keys in order; returns the new count.
    static int Insert(int *out, float *keys, int count, int max, int spot, float key)
    {
        if (max <= 0)
            return 0;
        if (count == max && key >= keys[count - 1])
            return count;
        int j = count < max ? count++ : count - 1;
        while (j > 0 && keys[j - 1] > key)
        {
            out[j] = out[j - 1];
            keys[j] = keys[j - 1];
            --j;
        }
        out[j] = spot;
        keys[j] = key;
        return count;
    }

    float m_places[kMaxSpots][3];
    uint32_t m_sees[kMaxSpots][kMaxSpots / 32];
    int m_seen[kMaxSpots];
    int m_count = 0;
    int m_a = 0;
    int m_b = 1;
    int m_lines = 0;
    bool m_ready = false;
};

// Whether `place` is at least `spacing` (across) from each of `kept`'s
// places, for spreading the spots over a map.
inline bool FarFromAll(const float *place, const float (*kept)[3], int count, float spacing)
{
    for (int i = 0; i < count; ++i)
        if (std::hypot(place[0] - kept[i][0], place[1] - kept[i][1]) < spacing
            && std::fabs(place[2] - kept[i][2]) < 96.0f)
            return false;
    return true;
}

} // namespace kisak::bots
