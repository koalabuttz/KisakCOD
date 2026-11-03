#pragma once

// The map's own effects, as its scripts place them: the effect table
// (maps/mp/<map>_fx.gsc, level._effect["<id>"] = loadfx("<path>")) and the
// createfx list (maps/createfx/<map>_fx.gsc), where each entry is made by one
// create call and then given its values:
//
//     ent = maps\mp\_utility::createOneshotEffect( "<id>" );
//     ent.v[ "origin" ] = ( x, y, z );
//     ent.v[ "angles" ] = ( pitch, yaw, roll );
//     ent.v[ "fxid" ] = "<id>";
//     ent.v[ "delay" ] = -15;
//
// Both files are script source in the map's zone (raw files). This reads only
// those shapes: a statement it does not know is reported, never guessed at. It
// runs no script and includes no engine header, so an asset-free test can check
// it (tests/linux_asset_export/map_effects_test.cpp) on text it writes itself.
//
// Script identifiers and function names are compared without case, as the
// script compiler does; string contents (effect ids, paths, value keys) are
// compared exactly.

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace kisak_map_effects
{

// ---- tokens -----------------------------------------------------------------

enum class TokenKind
{
    End,
    Name,    // identifier, with '\' path separators kept (maps\mp\_utility)
    String,  // "..." without the quotes, escapes resolved
    Number,  // optional '-', digits, optional fraction
    Punct,   // one of ( ) { } [ ] ; , . = or the two-character ::
    Other,   // any other character
};

struct Token
{
    TokenKind kind = TokenKind::End;
    std::string text;
    int line = 1;
};

inline bool SameName(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// Splits script source into tokens; comments (// and /* */) and white space are
// dropped. An unterminated string or comment ends the list with a problem.
inline std::vector<Token> Tokenize(std::string_view text, std::vector<std::string> &problems)
{
    std::vector<Token> tokens;
    int line = 1;
    size_t i = 0;
    const auto isNameStart = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; };
    const auto isNameChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\\';
    };
    while (i < text.size())
    {
        const char c = text[i];
        if (c == '\n')
        {
            ++line;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
        {
            const int startLine = line;
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/'))
            {
                if (text[i] == '\n')
                    ++line;
                ++i;
            }
            if (i + 1 >= text.size())
            {
                problems.push_back("line " + std::to_string(startLine) + ": a comment is not closed");
                return tokens;
            }
            i += 2;
            continue;
        }
        Token token;
        token.line = line;
        if (c == '"')
        {
            ++i;
            bool closed = false;
            while (i < text.size())
            {
                const char s = text[i++];
                if (s == '"')
                {
                    closed = true;
                    break;
                }
                if (s == '\n')
                    break;
                if (s == '\\' && i < text.size())
                {
                    const char e = text[i++];
                    token.text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    continue;
                }
                token.text += s;
            }
            if (!closed)
            {
                problems.push_back("line " + std::to_string(line) + ": a string is not closed");
                return tokens;
            }
            token.kind = TokenKind::String;
            tokens.push_back(token);
            continue;
        }
        const bool negative = c == '-' && i + 1 < text.size() &&
                              (std::isdigit(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '.');
        if (std::isdigit(static_cast<unsigned char>(c)) || negative ||
            (c == '.' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))))
        {
            size_t j = i + (negative ? 1 : 0);
            bool digits = false;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])))
                ++j, digits = true;
            if (j < text.size() && text[j] == '.')
            {
                ++j;
                while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])))
                    ++j, digits = true;
            }
            if (digits)
            {
                token.kind = TokenKind::Number;
                token.text = std::string(text.substr(i, j - i));
                tokens.push_back(token);
                i = j;
                continue;
            }
        }
        if (isNameStart(c))
        {
            size_t j = i;
            while (j < text.size() && isNameChar(text[j]))
                ++j;
            token.kind = TokenKind::Name;
            token.text = std::string(text.substr(i, j - i));
            tokens.push_back(token);
            i = j;
            continue;
        }
        if (c == ':' && i + 1 < text.size() && text[i + 1] == ':')
        {
            token.kind = TokenKind::Punct;
            token.text = "::";
            tokens.push_back(token);
            i += 2;
            continue;
        }
        token.kind = std::string_view("(){}[];,.=").find(c) != std::string_view::npos ? TokenKind::Punct
                                                                                        : TokenKind::Other;
        token.text = std::string(1, c);
        tokens.push_back(token);
        ++i;
    }
    return tokens;
}

// A cursor over the tokens with the few matches the two shapes need.
class Cursor
{
public:
    explicit Cursor(const std::vector<Token> &tokens) : tokens_(tokens) {}
    const Token &Peek(size_t ahead = 0) const
    {
        static const Token kEnd{};
        return pos_ + ahead < tokens_.size() ? tokens_[pos_ + ahead] : kEnd;
    }
    bool AtEnd() const { return pos_ >= tokens_.size(); }
    size_t Position() const { return pos_; }
    void Seek(size_t pos) { pos_ = pos; }
    const Token &Next()
    {
        const Token &token = Peek();
        if (!AtEnd())
            ++pos_;
        return token;
    }
    bool Punct(const char *text)
    {
        if (Peek().kind == TokenKind::Punct && Peek().text == text)
        {
            ++pos_;
            return true;
        }
        return false;
    }
    bool Name(const char *name)
    {
        if (Peek().kind == TokenKind::Name && SameName(Peek().text, name))
        {
            ++pos_;
            return true;
        }
        return false;
    }
    bool Take(TokenKind kind, std::string &text)
    {
        if (Peek().kind != kind)
            return false;
        text = Next().text;
        return true;
    }
    // Moves past one statement: up to and including the next ';' at this brace
    // depth, or the '}' that closes a block the statement opened. Stops before a
    // '}' that would close the enclosing block.
    void SkipStatement()
    {
        int depth = 0;
        while (!AtEnd())
        {
            const Token &token = Peek();
            if (token.kind == TokenKind::Punct)
            {
                if (token.text == "{")
                    ++depth;
                else if (token.text == "}")
                {
                    if (depth == 0)
                        return;
                    ++pos_;
                    if (--depth == 0)
                        return;
                    continue;
                }
                else if (token.text == ";" && depth == 0)
                {
                    ++pos_;
                    return;
                }
            }
            ++pos_;
        }
    }

private:
    const std::vector<Token> &tokens_;
    size_t pos_ = 0;
};

// ---- the effect table -------------------------------------------------------

struct EffectTable
{
    std::map<std::string, std::string> paths;  // effect id -> effect path (loadfx's argument)
    unsigned otherLoadFx = 0;  // loadfx calls not of the level._effect[...] = shape
    std::vector<std::string> problems;
};

// Every `level._effect[ "<id>" ] = loadfx( "<path>" );` in the file, wherever it
// is. An id given two different paths is a problem; the same pair twice is not.
inline EffectTable ParseEffectTable(std::string_view text)
{
    EffectTable table;
    const std::vector<Token> tokens = Tokenize(text, table.problems);
    Cursor at(tokens);
    unsigned loadFxCalls = 0, matched = 0;
    for (const Token &token : tokens)
        loadFxCalls += token.kind == TokenKind::Name && SameName(token.text, "loadfx");
    while (!at.AtEnd())
    {
        const size_t start = at.Position();
        std::string id, path;
        const int line = at.Peek().line;
        if (at.Name("level") && at.Punct(".") && at.Name("_effect") && at.Punct("[") &&
            at.Take(TokenKind::String, id) && at.Punct("]") && at.Punct("=") && at.Name("loadfx") &&
            at.Punct("(") && at.Take(TokenKind::String, path) && at.Punct(")") && at.Punct(";"))
        {
            ++matched;
            const auto found = table.paths.find(id);
            if (found != table.paths.end() && found->second != path)
                table.problems.push_back("line " + std::to_string(line) + ": effect '" + id + "' is loaded from '" +
                                         path + "' after '" + found->second + "'");
            else
                table.paths[id] = path;
            continue;
        }
        at.Seek(start + 1);
    }
    table.otherLoadFx = loadFxCalls - matched;
    return table;
}

// ---- the createfx list ------------------------------------------------------

enum class EntryKind
{
    Oneshot,    // createOneshotEffect: spawnFx, then triggerFx at its delay
    Loop,       // createLoopEffect: playLoopedFx, repeating every delay seconds
    Exploder,   // createExploder: played only when the game fires its exploder
    LoopSound,  // createLoopSound: a sound, no effect
    Other,      // another create function
};

inline const char *EntryKindName(EntryKind kind)
{
    switch (kind)
    {
    case EntryKind::Oneshot:
        return "oneshot";
    case EntryKind::Loop:
        return "loop";
    case EntryKind::Exploder:
        return "exploder";
    case EntryKind::LoopSound:
        return "loop_sound";
    case EntryKind::Other:
        return "other";
    }
    return "?";
}

struct Entry
{
    EntryKind kind = EntryKind::Other;
    std::string create;   // the create function's name, as written
    std::string createId; // its string argument, when it has one
    int line = 0;
    std::string fxid;
    float origin[3] = {};
    float angles[3] = {};
    float delay = 0;
    bool hasFxid = false, hasOrigin = false, hasAngles = false, hasDelay = false;
    unsigned otherValues = 0;  // values this reader does not use (soundalias, exploder, ...)
};

struct CreateFxList
{
    std::vector<Entry> entries;
    std::vector<std::string> problems;
    unsigned Count(EntryKind kind) const
    {
        unsigned count = 0;
        for (const Entry &entry : entries)
            count += entry.kind == kind;
        return count;
    }
};

inline EntryKind KindOfCreate(std::string_view function)
{
    if (SameName(function, "createOneshotEffect"))
        return EntryKind::Oneshot;
    if (SameName(function, "createLoopEffect"))
        return EntryKind::Loop;
    if (SameName(function, "createExploder"))
        return EntryKind::Exploder;
    if (SameName(function, "createLoopSound"))
        return EntryKind::LoopSound;
    return EntryKind::Other;
}

// The body of `main()`: entries, each made by `<var> = <path>::create...( ... );`
// and given values by `<var>.v[ "<key>" ] = <value>;`, where a value is a
// string, a number or a vector `( n, n, n )`. Any other statement is a problem.
// Checks afterwards that every oneshot and loop entry has its fxid (the same as
// its create call's argument when it has one), origin, angles and delay.
inline CreateFxList ParseCreateFx(std::string_view text)
{
    CreateFxList list;
    const std::vector<Token> tokens = Tokenize(text, list.problems);
    Cursor at(tokens);
    // Find `main ( ) {`.
    bool inMain = false;
    while (!at.AtEnd())
    {
        if (at.Name("main") && at.Punct("(") && at.Punct(")") && at.Punct("{"))
        {
            inMain = true;
            break;
        }
        at.Next();
    }
    if (!inMain)
    {
        list.problems.push_back("no main() { ... } in the file");
        return list;
    }
    std::string variable;
    bool closed = false;
    while (!at.AtEnd())
    {
        if (at.Punct("}"))
        {
            closed = true;
            break;
        }
        const size_t start = at.Position();
        const int line = at.Peek().line;
        std::string name, path, function, key, argument;
        // <var> = <path> :: <function> ( ["<id>"] ) ;
        if (at.Take(TokenKind::Name, name) && at.Punct("=") && at.Take(TokenKind::Name, path) && at.Punct("::") &&
            at.Take(TokenKind::Name, function) && at.Punct("("))
        {
            const bool hasArgument = at.Take(TokenKind::String, argument);
            if (at.Punct(")") && at.Punct(";"))
            {
                Entry entry;
                entry.kind = KindOfCreate(function);
                entry.create = function;
                entry.createId = hasArgument ? argument : std::string();
                entry.line = line;
                list.entries.push_back(entry);
                variable = name;
                continue;
            }
        }
        at.Seek(start);
        // <var> . v [ "<key>" ] = <value> ;
        if (at.Take(TokenKind::Name, name) && at.Punct(".") && at.Name("v") && at.Punct("[") &&
            at.Take(TokenKind::String, key) && at.Punct("]") && at.Punct("="))
        {
            std::string word;
            float vector[3] = {};
            int valueKind = 0; // 1 string, 2 number, 3 vector
            if (at.Take(TokenKind::String, word))
                valueKind = 1;
            else if (at.Take(TokenKind::Number, word))
                valueKind = 2;
            else if (at.Punct("("))
            {
                std::string x, y, z;
                if (at.Take(TokenKind::Number, x) && at.Punct(",") && at.Take(TokenKind::Number, y) &&
                    at.Punct(",") && at.Take(TokenKind::Number, z) && at.Punct(")"))
                {
                    vector[0] = std::strtof(x.c_str(), nullptr);
                    vector[1] = std::strtof(y.c_str(), nullptr);
                    vector[2] = std::strtof(z.c_str(), nullptr);
                    valueKind = 3;
                }
            }
            if (valueKind != 0 && at.Punct(";"))
            {
                if (list.entries.empty() || !SameName(name, variable))
                {
                    list.problems.push_back("line " + std::to_string(line) + ": a value for '" + name +
                                            "' before any entry was made for it");
                    continue;
                }
                Entry &entry = list.entries.back();
                const auto wrongType = [&](const char *want) {
                    list.problems.push_back("line " + std::to_string(line) + ": '" + key + "' is not " + want);
                };
                if (key == "origin" || key == "angles")
                {
                    if (valueKind != 3)
                        wrongType("a vector");
                    else
                    {
                        float *into = key == "origin" ? entry.origin : entry.angles;
                        for (int axis = 0; axis < 3; ++axis)
                            into[axis] = vector[axis];
                        (key == "origin" ? entry.hasOrigin : entry.hasAngles) = true;
                    }
                }
                else if (key == "fxid")
                {
                    if (valueKind != 1)
                        wrongType("a string");
                    else
                        entry.fxid = word, entry.hasFxid = true;
                }
                else if (key == "delay")
                {
                    if (valueKind != 2)
                        wrongType("a number");
                    else
                        entry.delay = std::strtof(word.c_str(), nullptr), entry.hasDelay = true;
                }
                else
                    ++entry.otherValues;
                continue;
            }
        }
        at.Seek(start);
        list.problems.push_back("line " + std::to_string(line) + ": a statement this reader does not know");
        at.SkipStatement();
        if (at.Position() == start)
            at.Next();
    }
    if (!closed)
        list.problems.push_back("main() is not closed");
    for (const Entry &entry : list.entries)
    {
        if (entry.kind != EntryKind::Oneshot && entry.kind != EntryKind::Loop)
            continue;
        const std::string where = "line " + std::to_string(entry.line) + ": the " + EntryKindName(entry.kind) +
                                  " entry";
        if (!entry.hasFxid)
            list.problems.push_back(where + " has no fxid");
        else if (!entry.createId.empty() && entry.createId != entry.fxid)
            list.problems.push_back(where + " was made for '" + entry.createId + "' but plays '" + entry.fxid + "'");
        if (!entry.hasOrigin)
            list.problems.push_back(where + " has no origin");
        if (!entry.hasAngles)
            list.problems.push_back(where + " has no angles");
        if (!entry.hasDelay)
            list.problems.push_back(where + " has no delay");
        if (entry.kind == EntryKind::Loop && entry.hasDelay && !(entry.delay * 1000.0f >= 1.0f))
            list.problems.push_back(where + " repeats every " + std::to_string(entry.delay) +
                                    " s, which playLoopedFx refuses");
    }
    return list;
}

// ---- timing -----------------------------------------------------------------

// Seconds to milliseconds as triggerFx and playLoopedFx convert them
// (SnapFloatToInt(seconds * 1000.0f)): to the nearest, halves to even, as the
// default floating-point rounding does.
inline int32_t ScriptMilliseconds(float seconds)
{
    return static_cast<int32_t>(std::lrintf(seconds * 1000.0f));
}

// ---- the plan ---------------------------------------------------------------

// One effect the map places and the free camera plays: a oneshot starts at
// `milliseconds` (its delay; triggerFx), a loop repeats every `milliseconds`
// (its delay; playLoopedFx).
struct Placement
{
    EntryKind kind = EntryKind::Oneshot;
    std::string fxid;
    std::string path;   // the effect's name in the zone (loadfx's argument)
    float origin[3] = {};
    float angles[3] = {};
    int32_t milliseconds = 0;
    int line = 0;
};

struct Plan
{
    std::vector<Placement> placements;  // the oneshot and loop entries, in file order
    unsigned exploders = 0, loopSounds = 0, others = 0;  // entries not played here
    std::vector<std::string> paths;     // the distinct effects the placements play
    std::vector<std::string> problems;
};

// The oneshot and loop entries of the createfx list, each with its effect from
// the table. isPackaged(path) says whether the map's zone holds that effect.
// Every problem the two files had is the plan's too, and so is a map that places
// no effect, an entry whose fxid the table does not load, and an effect the zone
// does not hold.
template <class IsPackaged>
Plan PlanEffects(const CreateFxList &list, const EffectTable &table, IsPackaged isPackaged)
{
    Plan plan;
    for (const std::string &problem : table.problems)
        plan.problems.push_back("effect table: " + problem);
    for (const std::string &problem : list.problems)
        plan.problems.push_back("createfx: " + problem);
    std::map<std::string, bool> checked;
    for (const Entry &entry : list.entries)
    {
        switch (entry.kind)
        {
        case EntryKind::Exploder:
            ++plan.exploders;
            continue;
        case EntryKind::LoopSound:
            ++plan.loopSounds;
            continue;
        case EntryKind::Other:
            ++plan.others;
            continue;
        case EntryKind::Oneshot:
        case EntryKind::Loop:
            break;
        }
        if (!entry.hasFxid || !entry.hasDelay || !entry.hasOrigin || !entry.hasAngles)
            continue; // already a problem of the list
        const auto found = table.paths.find(entry.fxid);
        if (found == table.paths.end())
        {
            plan.problems.push_back("createfx: line " + std::to_string(entry.line) + ": effect '" + entry.fxid +
                                    "' is not in the effect table");
            continue;
        }
        const auto known = checked.find(found->second);
        bool packaged;
        if (known == checked.end())
        {
            packaged = isPackaged(found->second);
            checked[found->second] = packaged;
            if (packaged)
                plan.paths.push_back(found->second);
            else
                plan.problems.push_back("effect '" + found->second + "' ('" + entry.fxid +
                                        "') is not in the map's zone");
        }
        else
            packaged = known->second;
        if (!packaged)
            continue;
        Placement placement;
        placement.kind = entry.kind;
        placement.fxid = entry.fxid;
        placement.path = found->second;
        for (int axis = 0; axis < 3; ++axis)
            placement.origin[axis] = entry.origin[axis], placement.angles[axis] = entry.angles[axis];
        placement.milliseconds = ScriptMilliseconds(entry.delay);
        placement.line = entry.line;
        plan.placements.push_back(placement);
    }
    if (plan.placements.empty() && plan.problems.empty())
        plan.problems.push_back("the map places no oneshot or loop effect");
    return plan;
}

} // namespace kisak_map_effects
