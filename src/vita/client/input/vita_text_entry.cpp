// See vita_text_entry.h.

#include "vita_text_entry.h"

namespace kisak::vita_text {

namespace {

// Printable ASCII and printable Latin-1: what the engine's one-byte text holds
// and Unicode has at the same numbers.
constexpr bool EngineCharacter(unsigned int character)
{
    return (character >= 0x20 && character <= 0x7E) || (character >= 0xA0 && character <= 0xFF);
}

} // namespace

int FromKeyboard(const uint16_t *text, char *out, int outSize)
{
    int count = 0;
    for (; text && *text && count < outSize - 1; ++text)
    {
        if (EngineCharacter(*text))
            out[count++] = static_cast<char>(static_cast<unsigned char>(*text));
    }
    out[count] = '\0';
    return count;
}

int ToKeyboard(const char *text, uint16_t *out, int outSize)
{
    int count = 0;
    for (; text && *text && count < outSize - 1; ++text)
    {
        const unsigned char character = static_cast<unsigned char>(*text);
        if (EngineCharacter(character))
            out[count++] = character;
    }
    out[count] = 0;
    return count;
}

void Entry::AskForCommand()
{
    askedForCommand_ = true;
}

bool Entry::Orphaned(const Waiting &waiting) const
{
    return state_ == State::Up && target_ != Target::Console
           && (waiting.target != target_ || waiting.field != field_);
}

void Entry::KeyboardClosed(bool accepted, const uint16_t *text)
{
    if (state_ != State::Up)
        return;
    accepted_ = accepted;
    length_ = accepted ? FromKeyboard(text, text_, sizeof(text_)) : 0;
    fed_ = 0;
    cleared_ = false;
    state_ = State::Feeding;
}

// The keyboard's text goes to the game. False: the game no longer waits for
// it, and nothing was done this frame.
bool Entry::Feed(const Waiting &waiting, Sink &sink)
{
    if (target_ == Target::Console)
    {
        if (accepted_ && length_)
            sink.Command(text_);
        state_ = State::Idle;
        return true;
    }
    // The game stopped waiting while the keyboard was up (a menu closed under
    // it, the match ended): the text has nowhere to go. Once the field is
    // emptied the characters follow wherever the game takes them, as a PC's
    // would when a full field hands on to the next.
    if (waiting.target != target_ || (!cleared_ && waiting.field != field_))
    {
        state_ = State::Idle;
        return false;
    }
    if (!accepted_)
    {
        // Closed without Enter: the edit ends and the field keeps its text.
        sink.Key(kKeyEscape);
        state_ = State::Idle;
        return true;
    }
    if (!cleared_)
    {
        sink.ClearField(target_);
        cleared_ = true;
    }
    for (int sent = 0; fed_ < length_ && sent < kCharsPerFrame; ++sent)
        sink.Char(static_cast<unsigned char>(text_[fed_++]));
    if (fed_ == length_)
    {
        sink.Key(kKeyEnter);
        state_ = State::Idle;
    }
    return true;
}

void Entry::Frame(const Waiting &waiting, Sink &sink)
{
    if (state_ == State::Up)
        return;
    if (state_ == State::Feeding && Feed(waiting, sink))
        return;

    // A wait that ended is forgotten, so the same field opens the keyboard
    // the next time it is edited.
    if (waiting.target == Target::None)
        handled_ = Waiting{};
    Target target = waiting.target;
    const void *field = waiting.field;
    if (askedForCommand_)
    {
        askedForCommand_ = false;
        target = Target::Console;
        field = nullptr;
    }
    else
    {
        // One keyboard for each wait: after it closed, or did not open, the
        // game has to stop waiting or wait for something else first.
        if (target == Target::None || (target == handled_.target && field == handled_.field))
            return;
        handled_ = waiting;
    }
    failed_ = !sink.OpenKeyboard(target);
    if (failed_)
        return;
    target_ = target;
    field_ = field;
    state_ = State::Up;
}

} // namespace kisak::vita_text
