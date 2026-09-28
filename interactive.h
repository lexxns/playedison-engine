// interactive.h — the "human side" of the JSON play protocol.
//
// This file is #included into duel_runner_edo.cpp and implements everything a
// HUMAN player needs to see or do in a live game (vs-AI or PvP):
//
//   * serializing the board into the {"ev":"state", ...} snapshot the browser
//     renders (see serialize_state_json),
//   * building the per-seat game log ("You summon X", "P1 activates Y"...),
//   * turning every core "decision request" (MSG_SELECT_*, MSG_ANNOUNCE_*)
//     into a {"ev":"prompt", ...} line and parsing the browser's
//     {"act": {...}} reply back into the core's binary response format,
//   * emitting {"ev":"anim", ...} hints for the frontend's animations.
//
// The opponent (player 1) is auto-played by the deck policies in policy.h;
// the human is always player 0, unless --multiplayer is set (both seats human).
//
// ---------------------------------------------------------------------------
// How a duel is driven (the message loop lives in duel_runner_edo.cpp)
// ---------------------------------------------------------------------------
//   1. Each OCG_DuelProcess() call returns a buffer of length-prefixed
//      messages:  [u32 len][u8 type][payload bytes] [u32 len][...] ...
//   2. For every message the loop calls update_field(), log_game_event() and
//      anim_game_event() with a Msg cursor positioned at the payload.
//   3. SELECT / ANNOUNCE messages are routed to human_handle_select(), which
//      emits a prompt line and blocks on read_action_line() until the server
//      writes an {"act": {...}} line back.
//   4. set_response() / set_response_i() encode the semantic answer into the
//      binary SELECT response the core expects.
//
// ---------------------------------------------------------------------------
// The Msg cursor (defined in duel_runner_edo.cpp)
// ---------------------------------------------------------------------------
//   Msg is a tiny little-endian reader over one message's payload bytes. It
//   knows nothing about the message — the CALLER decides the layout by the
//   order of reads. The payload starts AFTER the [u8 type] byte (the loop
//   advances the cursor past it first).
//
//     msg.u8()      read one byte            (uint8_t)
//     msg.u16()     read two bytes, LE       (uint16_t)
//     msg.u32()     read four bytes, LE      (uint32_t)
//     msg.u64()     read eight bytes, LE     (uint64_t)
//     msg.skip(n)   advance n bytes without reading
//
//   A "loc_info" appears in many messages and is always 10 bytes:
//   [controller u8][location u8][sequence u32][position u32].
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "policy.h"

// ---------------------------------------------------------------------------
// minimal JSON helpers
// ---------------------------------------------------------------------------
namespace json {
    inline std::string esc(const std::string &s) {
        std::string o;
        for (const char c: s) {
            switch (c) {
                case '"': o += "\\\"";
                    break;
                case '\\': o += "\\\\";
                    break;
                case '\n': o += "\\n";
                    break;
                case '\r': break;
                default: o += c;
            }
        }
        return o;
    }

    inline std::string str(const char *s) { return "\"" + esc(s) + "\""; }
    inline std::string str(const std::string &s) { return "\"" + esc(s) + "\""; }
    inline std::string num(const int64_t v) { return std::to_string(v); }
}

// ---- shared state: declared/defined in duel_runner_edo.cpp ----------------
// The AI policy's model of the field (zones, hands, LP, targets). Used here
// for log narration (e.g. looking up an attacker's code) — NOT for the board
// snapshot, which is queried from the core directly.
extern FieldState g_field;
// Opaque handle to the running OCG_Duel (needed for OCG_DuelQuery* calls).
extern intptr_t g_duel;
// The last MSG_HINT(SELECTMSG) payload — tells the UI what a SELECT is about.
extern uint64_t g_last_hint_selectmsg;
// Pack a card location (controler, location, sequence) into one key: 8 bits of
// each, which is far more than any zone or hand needs.
static inline uint32_t loc_key(uint8_t ctrl, uint8_t loc, uint32_t seq) {
    return (static_cast<uint32_t>(ctrl) << 24) | (static_cast<uint32_t>(loc) << 16)
            | (seq & 0xFFFF);
}
// Current turn number (incremented on MSG_NEW_TURN) and total processed steps.
extern int g_turns;
extern int g_steps;
// --verbose: extra stderr diagnostics from the runner.
extern bool g_verbose;

// Resolve a card passcode to its display name (queries cards.cdb).
static std::string card_name_ui(uint32_t code);
// Resolve an effect-description Stringid to its card text (defined below).
static std::string card_desc_string(uint64_t stringid);

// ---- module state (interactive.h only) ------------------------------------
// Per-view game log: g_seat_logs[0]/[1] hold each seat's narrative lines, so a
// player never sees the opponent's hidden information; g_seat_logs[2] is the
// *watcher* view — every line as a third party may read it (a face-down card is
// "a card", a revealed one keeps its name), which is what a spectator and the
// replay's spectator view get instead of one player's private log.
static std::vector<std::string> g_seat_logs[3];
// Current phase (PHASE_* bit value), kept fresh from MSG_NEW_PHASE.
static uint32_t g_current_phase = 0x04;
// Last known monster passcode per (player, zone-sequence) — a small cache used
// for naming cards in logs when the message stream doesn't carry the code.
static uint32_t g_last_mzone_code[2][8] = {{0}};
// True when running with --multiplayer (both seats are human players).
static bool g_multiplayer = false;
// The player (0 or 1) whose SELECT prompt is currently being handled; the
// server uses it to route the prompt to the right browser.
static uint8_t g_active_prompt_player = 0;

// Chain-link bookkeeping for the negate animation: MSG_CHAINING reports the
// card that activated each link, but MSG_CHAIN_NEGATED / MSG_CHAIN_DISABLED
// only carry the link number.  Keyed by the core's 1-based chain count.
struct ChainLinkAnim {
    uint32_t code = 0;
    uint8_t ctrl = 0;
    uint8_t loc = 0;
    uint32_t seq = 0;
};
static std::map<uint32_t, ChainLinkAnim> g_chain_links;

// Cards that were just Summoned, keyed "ctrl|loc|seq".  Many older card
// scripts never call SetDescription, so an on-summon trigger prompt would
// otherwise read as a bare "Activate <card>?" with no way to tell it apart
// from the card's ignition effect.  The window closes on the next Main Phase
// command or phase change.
static std::vector<std::string> g_just_summoned;
static std::string summon_key(const uint8_t ctrl, const uint8_t loc, const uint32_t seq) {
    return std::to_string(ctrl) + "|" + std::to_string(loc) + "|" + std::to_string(seq);
}
static void mark_just_summoned(const uint8_t ctrl, const uint8_t loc, const uint32_t seq) {
    const std::string k = summon_key(ctrl, loc, seq);
    for (const auto &e : g_just_summoned)
        if (e == k) return;
    g_just_summoned.push_back(k);
    if (g_just_summoned.size() > 12) g_just_summoned.erase(g_just_summoned.begin());
}
static bool just_summoned(const uint8_t ctrl, const uint8_t loc, const uint32_t seq) {
    const std::string k = summon_key(ctrl, loc, seq);
    for (const auto &e : g_just_summoned)
        if (e == k) return true;
    return false;
}
static void clear_just_summoned() { g_just_summoned.clear(); }

// Auto-pass support.  `g_recent_action` marks that something worth responding
// to happened (a card was activated, a monster summoned/set, an attack
// declared); phase and turn changes clear it, because merely moving through
// phases is exactly the noise the auto-pass toggle is meant to skip.
// `g_action_reported` makes the flag one-shot per player so the first prompt
// after the action says "action":true and later prompts in the same phase do
// not (otherwise every window would look worth stopping for).
// `g_chain_size` is the current chain link count (0 = no chain yet).
static bool g_recent_action = false;
static bool g_action_reported[2] = {false, false};
static uint32_t g_chain_size = 0;
static void note_significant_action() {
    g_recent_action = true;
    g_action_reported[0] = false;
    g_action_reported[1] = false;
}
static void clear_significant_action() {
    g_recent_action = false;
    g_action_reported[0] = false;
    g_action_reported[1] = false;
}

// Reset all per-game state (logs, phase, code cache) for a fresh duel.
static void log_reset() {
    for (auto & p : g_seat_logs) p.clear();
    g_current_phase = 0x04;
    for (auto & p : g_last_mzone_code) for (unsigned int & s : p) s = 0;
    g_chain_links.clear();
    g_just_summoned.clear();
    g_chain_size = 0;
    clear_significant_action();
}

// Display name of the card at (controller, location, sequence). Tries the AI
// field model first, then the monster-code cache; "?" when unknown. Used by the
// log narration (not by the board snapshot, which queries the core directly).
static std::string zone_card_name(const uint8_t loc, const uint8_t ctrl, const uint32_t seq) {
    // try the field model first, then the code cache
    for (const auto &kv: g_field.zone[ctrl])
        if (kv.first.first == loc && kv.first.second == seq)
            return card_name_ui(kv.second.code);
    if (loc == 0x04 && ctrl < 2 && seq < 8 && g_last_mzone_code[ctrl][seq])
        return card_name_ui(g_last_mzone_code[ctrl][seq]);
    return "?";
}

// like zone_card_name, but also reports whether the card is public (face-up).
// A face-down card's name must be shown only to its controller.
static std::string zone_card_name_secret(const uint8_t loc, const uint8_t ctrl, const uint32_t seq, bool *reveal) {
    *reveal = true;
    for (const auto &kv: g_field.zone[ctrl])
        if (kv.first.first == loc && kv.first.second == seq) {
            *reveal = kv.second.faceup;
            return card_name_ui(kv.second.code);
        }
    if (loc == 0x04 && ctrl < 2 && seq < 8 && g_last_mzone_code[ctrl][seq])
        return card_name_ui(g_last_mzone_code[ctrl][seq]);
    return "?";
}

// Human-readable name for a PHASE_* bit value (e.g. 0x04 -> "Main Phase 1").
static const char *phase_name(const uint32_t ph) {
    switch (ph) {
        case 0x01: return "Draw Phase";
        case 0x02: return "Standby Phase";
        case 0x04: return "Main Phase 1";
        case 0x08: return "Battle Phase";
        case 0x10: return "Battle Step";
        case 0x20: return "Damage Step";
        case 0x40: return "Damage Calculation";
        case 0x80: return "End of Battle Step";
        case 0x100: return "Main Phase 2";
        case 0x200: return "End Phase";
        default: return "?";
    }
}

// how seat `viewer` refers to the player `p` who performed an action
static const char *log_who_v(const uint8_t viewer, const uint8_t p) {
    if (viewer == 2) return p == 0 ? "P1" : "P2";   // the watcher is neither seat
    if (g_multiplayer) return p == viewer ? "You" : (p == 0 ? "P1" : "P2");
    return p == 0 ? "You" : "AI";
}

// append a line to one seat's log (capped at 400 lines)
static void log_add_v(const uint8_t viewer, const std::string &line) {
    g_seat_logs[viewer].push_back(line);
    if (g_seat_logs[viewer].size() > 400)
        g_seat_logs[viewer].erase(g_seat_logs[viewer].begin());
}

// append a neutral line (phase markers, damage markers, targeting arrows)
static void log_add(const std::string &what) {
    log_add_v(0, what);
    log_add_v(1, what);
}

// pluralize the *last* word ("special summon" -> "special summons", "set" -> "sets")
static std::string plural_verb(const std::string &verb) {
    const size_t sp = verb.rfind(' ');
    if (sp == std::string::npos) return verb + "s";
    return verb.substr(0, sp + 1) + verb.substr(sp + 1) + "s";
}

// a card action by `ctrl`.  The card's name is shown to the other seat only
// when `reveal` is true (the card is public); otherwise the opponent reads
// "a card" while its controller sees the real name.  `suffix` trails the name
// (e.g. " face-down").
static void log_card_event(const uint8_t ctrl, const std::string &verb, const uint32_t code,
                           const bool reveal, const std::string &suffix = "") {
    const std::string name = card_name_ui(code);
    for (int v = 0; v < 3; ++v) {
        const bool mine = (ctrl == v);
        const std::string what = (reveal || mine) ? name : std::string("a card");
        log_add_v(v, std::string(log_who_v(v, ctrl)) + " " +
                     (mine ? verb : plural_verb(verb)) + " " + what + suffix);
    }
}

// a player action without a card name (damage, LP, direct attack, draws)
static void log_action(const uint8_t ctrl, const std::string &verb, const std::string &rest) {
    for (int v = 0; v < 3; ++v) {
        const bool mine = (ctrl == v);
        log_add_v(v, std::string(log_who_v(v, ctrl)) + " " +
                     (mine ? verb : plural_verb(verb)) + rest);
    }
}

// a labelled choice by `ctrl` (a SELECT_OPTION answer).  Announced to both
// seats: "activates Elemental HERO Stratos: Activate 1 of these effects" tells
// the opponent nothing about which one was picked, because the choice happens
// inside the effect's target as a Duel.SelectOption, and the announcement has
// to come from here.  `code` names the card the choice belongs to when the
// option was a description Stringid.
static void log_choice(const uint8_t ctrl, const uint32_t code, const std::string &what) {
    if (what.empty()) return;
    for (int v = 0; v < 3; ++v) {
        const bool mine = (ctrl == v);
        std::string line = std::string(log_who_v(v, ctrl)) + (mine ? " choose" : " chooses");
        if (code >= 100000) line += " for " + card_name_ui(code);
        log_add_v(v, line + ": " + what);
    }
}

// the name of a card in a zone as seat `viewer` may see it: the real name if
// it is face-up (public) or belongs to the viewer, otherwise "a face-down card"
static std::string zone_card_name_for(const uint8_t viewer, const uint8_t loc, const uint8_t ctrl, const uint32_t seq) {
    bool reveal = false;
    std::string nm = zone_card_name_secret(loc, ctrl, seq, &reveal);
    if (reveal || ctrl == viewer) return nm;
    return "a face-down card";
}

// a targeting arrow line; the target's name is private unless it is face-up
static void log_target_line(const std::string &prefix, const uint8_t tl, const uint8_t tc, const uint32_t tseq) {
    for (int v = 0; v < 3; ++v)
        log_add_v(v, prefix + zone_card_name_for(v, tl, tc, tseq));
}

// ---------------------------------------------------------------------------
// state serialization (board snapshot for the UI)
//
// The board snapshot is produced by querying the core's actual game state
// with OCG_DuelQueryLocation — the "state of the world" — instead of
// replaying the message stream.  This means the UI can never drift out of
// sync with the engine (the old message-replay model forgot to remove cards
// from the hand list when they moved to the field, so set/summoned cards
// showed up in both places).  Only LP/turn/phase/targets/log, which the
// query API does not expose, come from the runner's own globals.
// ---------------------------------------------------------------------------
struct QCard {
    uint32_t code = 0;
    uint32_t pos = 0;
    uint32_t seq = 0;
    int32_t atk = 0;
    int32_t def = 0;
    bool is_public = false;
    int counters = 0;   // total counter count on this card (all types)
    // Relationships this card maintains, as packed locations (loc_key): what it
    // is equipped to (QUERY_EQUIP_CARD) and what it keeps targeted
    // (QUERY_TARGET_CARD — how Call of the Haunted and Il Blud-style cards record
    // "this monster dies with me").  Both are visible on the table, so both
    // players get them.
    uint32_t equip = 0;                  // 0 = none
    std::vector<uint32_t> targets;       // maintained card targets
};

// Parse the OCG_DuelQueryLocation buffer for one location into plain structs.
// The buffer is [u32 data_size][card data...], where each card is a run of
// (u16 size, u32 flag, value...) tuples ending in QUERY_END, and each empty
// mzone/szone slot is a bare u16 0.  Values are copied out immediately so the
// core's reused query buffer may be invalidated by the next call.
static std::vector<QCard> query_location(const OCG_Duel duel, const uint8_t player, const uint32_t loc) {
    std::vector<QCard> out;
    OCG_QueryInfo info{};
    info.flags = QUERY_CODE | QUERY_POSITION | QUERY_ATTACK | QUERY_DEFENSE | QUERY_IS_PUBLIC | QUERY_COUNTERS
            | QUERY_EQUIP_CARD | QUERY_TARGET_CARD;
    info.con = player;
    info.loc = loc;
    uint32_t len = 0;
    const auto *buf = static_cast<const uint8_t *>(OCG_DuelQueryLocation(duel, &len, &info));
    if (!buf || len < 4)
        return out;
    const uint8_t *d = buf + 4;
    const uint8_t *dend = buf + len;
    const bool slotted = (loc == LOCATION_MZONE || loc == LOCATION_SZONE);
    uint32_t seq = 0;
    while (d < dend) {
        if (dend - d < 2)
            break;
        uint16_t sz = 0;
        std::memcpy(&sz, d, 2);
        if (sz == 0) {
            // empty mzone/szone slot marker
            d += 2;
            if (slotted) ++seq;
            continue;
        }
        if (sz < 4 || d + 2 + sz > dend)
            break;
        QCard qc;
        const uint8_t *e = d;
        bool ended = false;
        while (e + 2 <= dend) {
            uint16_t fsz = 0;
            std::memcpy(&fsz, e, 2);
            if (fsz < 4 || e + 2 + fsz > dend)
                break;
            uint32_t flag = 0;
            std::memcpy(&flag, e + 2, 4);
            const uint8_t *v = e + 6;
            const uint32_t vsz = fsz - 4;
            if (flag == QUERY_CODE && vsz >= 4) std::memcpy(&qc.code, v, 4);
            else if (flag == QUERY_POSITION && vsz >= 4) std::memcpy(&qc.pos, v, 4);
            else if (flag == QUERY_ATTACK && vsz >= 4) {
                int32_t x = 0;
                std::memcpy(&x, v, 4);
                qc.atk = x;
            } else if (flag == QUERY_DEFENSE && vsz >= 4) {
                int32_t x = 0;
                std::memcpy(&x, v, 4);
                qc.def = x;
            } else if (flag == QUERY_IS_PUBLIC && vsz >= 1) qc.is_public = (*v != 0);
            else if (flag == QUERY_EQUIP_CARD && vsz >= 10) {
                // one location, or ten zero bytes for "attached to nothing"
                // (loc 0 is not a real location, so it is the absent marker)
                const uint8_t ctrl = v[0], lo = v[1];
                uint32_t sq = 0;
                std::memcpy(&sq, v + 2, 4);
                if (lo) qc.equip = loc_key(ctrl, lo, sq);
            } else if (flag == QUERY_TARGET_CARD && vsz >= 4) {
                // [count u32][count x location]
                uint32_t n = 0;
                std::memcpy(&n, v, 4);
                for (uint32_t i = 0; i < n && 4 + i * 10 + 10 <= vsz; ++i) {
                    const uint8_t *t = v + 4 + i * 10;
                    const uint8_t ctrl = t[0], lo = t[1];
                    uint32_t sq = 0;
                    std::memcpy(&sq, t + 2, 4);
                    if (lo) qc.targets.push_back(loc_key(ctrl, lo, sq));
                }
            }
            else if (flag == QUERY_COUNTERS && vsz >= 4) {
                // value: [count u32][(type + total<<16) u32] per counter type
                uint32_t nc = 0;
                std::memcpy(&nc, v, 4);
                qc.counters = 0;
                for (uint32_t i = 0; i < nc && 4 + i * 4 + 4 <= vsz; ++i) {
                    uint32_t cc = 0;
                    std::memcpy(&cc, v + 4 + i * 4, 4);
                    qc.counters += (int)(cc >> 16);
                }
            }
            e += 2 + fsz;
            if (flag == QUERY_END) {
                ended = true;
                break;
            }
        }
        if (!ended)
            break;
        qc.seq = seq;
        if (slotted) ++seq;
        out.push_back(qc);
        d = e;
    }
    return out;
}

static std::string serialize_state_json(const uint8_t viewer) {
    const auto duel = reinterpret_cast<OCG_Duel>(g_duel);
    const uint8_t opp = 1 - viewer;
    // "what is this card tied to?" — the monster an equip sits on, and the cards
    // a maintained effect keeps targeted (Call of the Haunted's revived monster,
    // Il Blud's summoned Zombie, ...).  Both are visible on the table.  The core
    // reports live card locations, so there is no stale list to validate against:
    // if the target left the field it simply is not here.
    auto links_json = [&](const QCard &qc) {
        std::vector<uint32_t> keys;
        if (qc.equip)
            keys.push_back(qc.equip);
        for (uint32_t k : qc.targets)
            if (k != qc.equip)
                keys.push_back(k);
        if (keys.empty())
            return std::string();
        std::ostringstream out;
        out << ",\"links\":[";
        for (size_t i = 0; i < keys.size(); ++i) {
            const uint8_t c = static_cast<uint8_t>((keys[i] >> 24) & 0xFF);
            const uint8_t l = static_cast<uint8_t>((keys[i] >> 16) & 0xFF);
            const uint32_t sq = keys[i] & 0xFFFF;
            out << (i ? "," : "") << "{\"p\":" << static_cast<int>(c) << ",\"loc\":" << static_cast<int>(l)
                    << ",\"seq\":" << sq << "}";
        }
        out << "]";
        return out.str();
    };

    // zone (mz/sz) object list, hiding the opponent's facedown codes
    auto zone_json = [&](const uint8_t player, const uint32_t loc) {
        std::ostringstream out;
        out << "[";
        bool first = true;
        for (const auto &qc: query_location(duel, player, loc)) {
            const bool faceup = (qc.pos & 0x5) != 0; // POS_FACEUP_ATTACK | POS_FACEUP_DEFENSE
            const bool hidden = (player == opp && !faceup); // opp facedown = private
            out << (first ? "" : ",");
            out << "{\"code\":" << (hidden ? 0 : qc.code)
                    << ",\"pos\":" << qc.pos
                    // a set monster's stats identify it as well as its name does,
                    // so they are private on exactly the same terms as the code
                    << ",\"atk\":" << (hidden ? 0 : qc.atk)
                    << ",\"def\":" << (hidden ? 0 : qc.def)
                    << ",\"faceup\":" << (faceup ? "true" : "false")
                    << ",\"known\":" << (hidden ? "false" : "true")
                    << ",\"p\":" << static_cast<int>(player) << ",\"loc\":" << static_cast<int>(loc)
                    << ",\"seq\":" << qc.seq
                    << ",\"counters\":" << qc.counters
                    << (hidden ? std::string() : links_json(qc))
                    << "}";
            first = false;
        }
        out << "]";
        return out.str();
    };
    auto codes_json = [](const std::vector<uint32_t> &v) {
        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < v.size(); ++i) out << (i ? "," : "") << v[i];
        out << "]";
        return out.str();
    };
    // full code lists for hands / graveyards / banished / extra
    auto code_list = [&](const uint8_t player, const uint32_t loc) {
        std::vector<uint32_t> v;
        for (auto &qc: query_location(duel, player, loc))
            if (qc.code) v.push_back(qc.code);
        return v;
    };
    const std::vector<uint32_t> hand0 = code_list(viewer, LOCATION_HAND);
    // opponent hand: full codes are private unless a reveal effect (e.g. Mind
    // on Air's EFFECT_PUBLIC) makes them public. Emit the codes only when public.
    const auto opp_hand_q = query_location(duel, opp, LOCATION_HAND);
    std::vector<uint32_t> hand1;
    for (auto &qc: opp_hand_q)
        if (qc.is_public && qc.code) hand1.push_back(qc.code);
    const std::vector<uint32_t> g0 = code_list(viewer, LOCATION_GRAVE);
    const std::vector<uint32_t> g1 = code_list(opp, LOCATION_GRAVE);
    const std::vector<uint32_t> ban0 = code_list(viewer, LOCATION_REMOVED);
    const std::vector<uint32_t> ban1 = code_list(opp, LOCATION_REMOVED);
    const std::vector<uint32_t> extra0 = code_list(viewer, LOCATION_EXTRA);
    const int extra1_count = static_cast<int>(query_location(duel, opp, LOCATION_EXTRA).size());
    const int deck0_count = static_cast<int>(query_location(duel, viewer, LOCATION_DECK).size());
    const int deck1_count = static_cast<int>(query_location(duel, opp, LOCATION_DECK).size());

    std::ostringstream out;
    out << R"({"ev":"state","player":)" << static_cast<int>(viewer)
            << ",\"turn\":" << g_turns << ",\"steps\":" << g_steps
            << ",\"phase\":" << g_current_phase
            << ",\"phase_name\":" << json::str(phase_name(g_current_phase))
            << ",\"lp\":[" << g_field.lp[viewer] << "," << g_field.lp[opp] << "]"
            << ",\"hand0\":" << codes_json(hand0)
            << ",\"hand1_count\":" << opp_hand_q.size()
            << ",\"hand1\":" << codes_json(hand1)
            << ",\"deck0_count\":" << deck0_count
            << ",\"deck1_count\":" << deck1_count
            << ",\"mz0\":" << zone_json(viewer, LOCATION_MZONE)
            << ",\"mz1\":" << zone_json(opp, LOCATION_MZONE)
            << ",\"sz0\":" << zone_json(viewer, LOCATION_SZONE)
            << ",\"sz1\":" << zone_json(opp, LOCATION_SZONE)
            << ",\"g0\":" << codes_json(g0)
            << ",\"g1\":" << codes_json(g1)
            << ",\"ban0\":" << codes_json(ban0)
            << ",\"ban1\":" << codes_json(ban1)
            << ",\"extra0\":" << codes_json(extra0)
            << ",\"extra1_count\":" << extra1_count
            // no "hint" here: it was the last SELECT prompt's question string id,
            // shared by both viewers (so it told the opponent what was being
            // asked) and unused by the client.  Remove the extra comma?  It is
            // followed by "targets", so nothing else changes.
            << ",\"targets\":[";
    for (size_t i = 0; i < g_field.targets.size(); ++i) {
        if (i) out << ",";
        out << "[" << static_cast<int>(std::get<0>(g_field.targets[i])) << ","
                << static_cast<int>(std::get<1>(g_field.targets[i])) << ","
                << std::get<2>(g_field.targets[i]) << "]";
    }
    out << "]"
            << ",\"log\":[";
    for (size_t i = 0; i < g_seat_logs[viewer].size(); ++i) {
        if (i) out << ",";
        out << json::str(g_seat_logs[viewer][i]);
    }
    // the watcher log rides along with every snapshot: the room keeps it for
    // spectators and drops it before pushing to a player (see server.py)
    out << "],\"log_watch\":[";
    for (size_t i = 0; i < g_seat_logs[2].size(); ++i) {
        if (i) out << ",";
        out << json::str(g_seat_logs[2][i]);
    }
    out << "]}";
    return out.str();
}

// translate core messages into human-readable log lines ("You summon X",
// "AI activates Y").  `msg` points at the payload (type already consumed).
static void log_game_event(uint8_t type, Msg &msg) {
    switch (type) {
        case MSG_SUMMONING:
        case MSG_SPSUMMONING:
        case MSG_FLIPSUMMONING: {
            uint32_t code = msg.u32();
            uint8_t ctrl = msg.u8();
            msg.u8();
            msg.u32();
            msg.u32();
            const char *verb = (type == MSG_SUMMONING)
                                   ? "summon"
                                   : (type == MSG_SPSUMMONING)
                                         ? "special summon"
                                         : "flip summon";
            log_card_event(ctrl, verb, code, /*reveal=*/true);
            return;
        }
        case MSG_SET: {
            uint32_t code = msg.u32();
            uint8_t ctrl = msg.u8();
            msg.u8();
            msg.u32();
            msg.u32();
            // face-down: the name is private to the controller
            log_card_event(ctrl, "set", code, /*reveal=*/false, " face-down");
            return;
        }
        case MSG_CHAINING: {
            uint32_t code = msg.u32();
            uint8_t ctrl = msg.u8();
            msg.u8();
            msg.u32();
            msg.u32();
            msg.u8();
            msg.u8();
            msg.u32();
            const uint64_t desc = msg.u64();   // effect description Stringid
            msg.u32();
            // name which of the card's effects was activated, when the script
            // provides a description (keeps the log in step with the prompt)
            const std::string dtext = card_desc_string(desc);
            log_card_event(ctrl, "activate", code, /*reveal=*/true,
                           dtext.empty() ? "" : ": " + dtext);
            return;
        }
        case MSG_ATTACK: {
            uint8_t ac = msg.u8();
            uint8_t al = msg.u8();
            uint32_t aseq = msg.u32();
            msg.u32();
            uint8_t tc = msg.u8();
            uint8_t tl = msg.u8();
            uint32_t tseq = msg.u32();
            msg.u32();
            // attacker code is not in MSG_ATTACK; look it up from the field model,
            // restricted to the attacker's own side (a defender at the same seq on
            // the opposite side must never be mistaken for the attacker)
            std::string attacker = "?";
            for (auto &kv: g_field.zone[ac])
                if (kv.first.first == al && kv.first.second == aseq) {
                    attacker = card_name_ui(kv.second.code);
                    break;
                }
            if (tl == 0) {
                log_action(ac, "declare", " a direct attack with " + attacker);
            } else {
                // the target may be face-down: its name is private to its controller
                for (int v = 0; v < 3; ++v) {
                    std::string target = zone_card_name_for(v, tl, tc, tseq);
                    log_add_v(v, std::string(log_who_v(v, ac)) + " " +
                                 (ac == v ? "declare" : "declares") +
                                 " an attack with " + attacker + " targeting " + target);
                }
            }
            return;
        }
        case MSG_POS_CHANGE: {
            uint32_t code = msg.u32();
            uint8_t ctrl = msg.u8();
            uint8_t loc = msg.u8();
            msg.u8();
            uint8_t prev = msg.u8();
            uint8_t cur = msg.u8();
            if (loc == 0x08) {
                // SZONE: reveal / set face-down
                if ((cur & 0x1) && !(prev & 0x1))
                    log_card_event(ctrl, "reveal", code, /*reveal=*/true);
                else
                    log_card_event(ctrl, "set", code, /*reveal=*/false, " face-down");
            } else {
                // MZONE monster: position change
                const char *pos = (cur & 0x1) ? "attack position" : (cur & 0x4) ? "face-up defense" : "face-down";
                const bool faceup_now = (cur & 0x1) || (cur & 0x4);
                log_card_event(ctrl, "change", code, faceup_now, std::string(" to ") + pos);
            }
            return;
        }
        case MSG_CARD_TARGET: {
            // effect card loc_info, target loc_info
            uint8_t ec = msg.u8();
            uint8_t el = msg.u8();
            uint32_t eseq = msg.u32();
            msg.u32();
            uint8_t tc = msg.u8();
            uint8_t tl = msg.u8();
            uint32_t tseq = msg.u32();
            msg.u32();
            (void) ec;
            (void) el;
            (void) eseq;
            log_target_line("  → targets ", tl, tc, tseq);
            bool dup = false;
            for (auto &t: g_field.targets)
                if (std::get<0>(t) == tc && std::get<1>(t) == tl && std::get<2>(t) == tseq) dup = true;
            if (!dup)
                g_field.targets.emplace_back(tc, tl, tseq);
            return;
        }
        case MSG_CANCEL_TARGET: {
            uint8_t ec = msg.u8();
            uint8_t el = msg.u8();
            uint32_t eseq = msg.u32();
            msg.u32();
            uint8_t tc = msg.u8();
            uint8_t tl = msg.u8();
            uint32_t tseq = msg.u32();
            msg.u32();
            (void) ec;
            (void) el;
            (void) eseq;
            log_target_line("  → no longer targets ", tl, tc, tseq);
            for (auto it = g_field.targets.begin(); it != g_field.targets.end(); ++it)
                if (std::get<0>(*it) == tc && std::get<1>(*it) == tl && std::get<2>(*it) == tseq) {
                    g_field.targets.erase(it);
                    break;
                }
            return;
        }
        case MSG_BECOME_TARGET: {
            uint32_t n = msg.u32();
            std::vector<std::tuple<uint8_t, uint8_t, uint32_t> > added;
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t tc = msg.u8();
                uint8_t tl = msg.u8();
                uint32_t tseq = msg.u32();
                msg.u32();
                added.emplace_back(tc, tl, tseq);
            }
            if (n == 1) {
                auto &t = added[0];
                log_target_line("  → targets ", std::get<1>(t), std::get<0>(t), std::get<2>(t));
            }
            for (auto &t: added) {
                bool dup = false;
                for (auto &e: g_field.targets)
                    if (e == t) dup = true;
                if (!dup) g_field.targets.push_back(t);
            }
            return;
        }
        case MSG_BATTLE: {
            // attacker loc_info, a_atk, a_def, a_flag, target loc_info, t_atk, t_def, t_flag
            uint8_t ac = msg.u8();
            uint8_t al = msg.u8();
            uint32_t aseq = msg.u32();
            msg.u32();
            uint32_t a_atk = msg.u32();
            uint32_t a_def = msg.u32();
            uint8_t a_flag = msg.u8();
            uint8_t tc = msg.u8();
            uint8_t tl = msg.u8();
            uint32_t tseq = msg.u32();
            uint32_t tpos = msg.u32();
            uint32_t t_atk = msg.u32();
            uint32_t t_def = msg.u32();
            uint8_t t_flag = msg.u8();
            const bool t_defending = (tpos & 0x4) || (tpos & 0x8); // face-up/down defense
            std::string attacker = zone_card_name(al, ac, aseq);
            std::string what = attacker + " (ATK " + std::to_string(a_atk) + ")";
            if (tl == 0) {
                what += " attacks directly";
            } else {
                std::string target = zone_card_name(tl, tc, tseq);
                what += t_defending
                            ? " vs " + target + " (DEF " + std::to_string(t_def) + ")"
                            : " vs " + target + " (ATK " + std::to_string(t_atk) + ")";
            }
            log_add("— Damage Calculation —");
            log_action(ac, "battle", ": " + what);
            (void) a_def;
            (void) a_flag;
            (void) tc;
            (void) t_flag;
            return;
        }
        case MSG_DAMAGE_STEP_START: {
            if (g_current_phase != 0x20) {
                g_current_phase = 0x20;
                log_add("— Damage Step —");
            }
            return;
        }
        case MSG_DAMAGE_STEP_END: {
            if (g_current_phase != 0x10) {
                g_current_phase = 0x10;
                log_add("— End of Damage Step —");
            }
            return;
        }
        case MSG_DAMAGE: {
            uint8_t p = msg.u8();
            uint32_t amt = msg.u32();
            log_action(p, "take", " " + std::to_string(amt) + " damage");
            return;
        }
        case MSG_RECOVER: {
            uint8_t p = msg.u8();
            uint32_t amt = msg.u32();
            log_action(p, "recover", " " + std::to_string(amt) + " LP");
            return;
        }
        case MSG_DRAW: {
            uint8_t p = msg.u8();
            int n = static_cast<int>(msg.u32());
            std::vector<uint32_t> codes(n);
            for (int i = 0; i < n; ++i) {
                codes[i] = msg.u32();
                msg.u32();
            }
            // opening hand: 5 (or 5+1 with 1st-turn draw) before any prompts
            static bool opening_done[2] = {false, false};
            const bool opening = (n == 5 && !opening_done[p]);
            if (opening) opening_done[p] = true;
            // a drawn card's name is revealed only to the player who drew it
            for (int v = 0; v < 3; ++v) {
                const bool mine = (p == v);
                std::string what;
                if (opening) {
                    what = "an opening hand";
                } else {
                    what = std::to_string(n) + " card" + (n == 1 ? "" : "s");
                    if (n == 1 && mine)
                        what += " (" + card_name_ui(codes[0]) + ")";
                }
                log_add_v(v, std::string(log_who_v(v, p)) + " " + (mine ? "draw" : "draws") + " " + what);
            }
            return;
        }
        case MSG_NEW_PHASE: {
            uint32_t ph = msg.u16();
            if (ph != g_current_phase) {
                g_current_phase = ph;
                log_add("— " + std::string(phase_name(ph)) + " —");
                // resolved chains no longer hold targets; clear the highlight
                // so it doesn't linger into the next phase
                g_field.targets.clear();
            }
            return;
        }
        case MSG_CHAIN_END: {
            // keep the highlight visible while the chain's effects resolve; it
            // clears when the next phase starts (or the next chain re-targets)
            return;
        }
        case MSG_CHAIN_NEGATED:
        case MSG_CHAIN_DISABLED: {
            // payload: [chaincount u8] — the card is resolved via g_chain_links
            const uint32_t link = msg.u8();
            auto it = g_chain_links.find(link);
            const bool disabled = (type == MSG_CHAIN_DISABLED);
            const char *what = disabled ? "disabled" : "negated";
            const std::string suffix = std::string(" (chain link ") + std::to_string(link) + ")";
            if (it != g_chain_links.end()) {
                const uint32_t code = it->second.code;
                const uint8_t ctrl = it->second.ctrl;
                const std::string name = card_name_ui(code);
                for (int v = 0; v < 3; ++v) {
                    const bool mine = (ctrl == v);
                    const std::string who = mine ? "Your " : (std::string(log_who_v(v, ctrl)) + "'s ");
                    log_add_v(v, who + name + " was " + what + suffix);
                }
            } else {
                log_add(std::string("Chain link ") + std::to_string(link) + " was " + what);
            }
            return;
        }
        case MSG_TOSS_COIN: {
            const uint8_t player = msg.u8();
            const int count = msg.u8();
            std::string res;
            for (int i = 0; i < count; ++i) {
                const bool heads = msg.u8() != 0;
                if (i) res += ", ";
                res += heads ? "Heads" : "Tails";
            }
            log_action(player, "toss", std::string(" a coin — ") + res);
            return;
        }
        case MSG_TOSS_DICE: {
            const uint8_t player = msg.u8();
            const int count = msg.u8();
            std::string res;
            for (int i = 0; i < count; ++i) {
                if (i) res += ", ";
                res += std::to_string(static_cast<int>(msg.u8()));
            }
            log_action(player, "roll", std::string(count == 1 ? " a die — " : " the dice — ") + res);
            return;
        }
        case MSG_MISSED_EFFECT: {
            // payload: loc_info (ctrl u8, loc u8, seq u32, pos u32) + code u32
            const uint8_t ctrl = msg.u8();
            const uint8_t loc = msg.u8();
            msg.u32();
            msg.u32();
            const uint32_t code = msg.u32();
            // a hand card's identity stays private even when its timing is missed
            const bool reveal = (loc != 0x02);
            const std::string name = reveal ? card_name_ui(code) : std::string("a card");
            for (int v = 0; v < 3; ++v) {
                const bool mine = (ctrl == v);
                const std::string who = mine ? "Your " : (std::string(log_who_v(v, ctrl)) + "'s ");
                log_add_v(v, who + name + " missed the timing");
            }
            return;
        }
        default:
            return;
    }
}

// resolve a description Stringid (aux.Stringid(code,index) =
// (index & 0xfffff) | (code << 20)) to the card's str[index+1] text
// (str1..str16). Returns "" when unavailable (system strings <= 0x7ff are the
// generic "activate this effect?" prompts, not card text) so the UI falls back
// to a plain label.
static std::string card_desc_string(const uint64_t stringid) {
    if (!stringid || stringid <= 0x7ff) return "";
    const auto code = static_cast<uint32_t>(stringid >> 20);
    const auto idx = static_cast<uint32_t>(stringid & 0xfffff);
    if (idx > 15) return "";
    static sqlite3_stmt *st = nullptr;
    if (!st) {
        sqlite3_prepare_v2(g_db,
                           "SELECT str1,str2,str3,str4,str5,str6,str7,str8,"
                           "str9,str10,str11,str12,str13,str14,str15,str16 "
                           "FROM texts WHERE id = ?", -1, &st, nullptr);
    }
    sqlite3_reset(st);
    sqlite3_bind_int(st, 1, static_cast<int>(code));
    if (sqlite3_step(st) == SQLITE_ROW) {
        const auto s = reinterpret_cast<const char *>(sqlite3_column_text(st, static_cast<int>(idx)));
        if (s) return s;
    }
    return "";
}

// card option helper: code + name + position for the UI
static std::string card_opt_json(const uint32_t code, const uint32_t pos = 0) {
    std::ostringstream out;
    out << "{\"code\":" << code << ",\"name\":" << json::str(card_name_ui(code))
            << ",\"pos\":" << pos << "}";
    return out.str();
}

// human-readable zone name for a card option (so deck/field/hand targets can
// be told apart in the button list)
static const char *loc_name(const uint8_t loc) {
    switch (loc) {
        case 0x01: return "Deck";
        case 0x02: return "Hand";
        case 0x04: return "Field";
        case 0x08: return "Spell/Trap";
        case 0x10: return "Graveyard";
        case 0x20: return "Banished";
        case 0x40: return "Extra Deck";
        case 0x80: return "Overlay";
        default: return "";
    }
}

// card option for a SELECT prompt, hiding an opponent's face-down field card
// (the core reports the real code in MSG_SELECT_CARD, so we must mask it here).
// Non-field cards (hand/grave/extra) are left as-is — their visibility is
// decided by the effect that put them into the selection.
static std::string card_opt_json_secret(const uint32_t code, const uint32_t pos, const uint8_t ctrl,
                                        const uint8_t loc, const uint32_t seq, const uint8_t viewer) {
    const bool on_field = (loc == 0x04 || loc == 0x08); // LOCATION_MZONE / SZONE
    const bool faceup = (pos & 0x5) != 0; // POS_FACEUP_ATTACK | POS_FACEUP_DEFENSE
    const bool hidden = (ctrl != viewer && on_field && !faceup);
    std::ostringstream out;
    out << "{\"code\":" << (hidden ? 0 : code)
            << ",\"name\":" << json::str(hidden ? std::string("a face-down card") : card_name_ui(code))
            << ",\"pos\":" << pos
            << ",\"p\":" << static_cast<int>(ctrl)
            << ",\"loc\":" << static_cast<int>(loc)
            << ",\"locname\":" << json::str(loc_name(loc))
            << ",\"seq\":" << seq
            << ",\"hidden\":" << (hidden ? "true" : "false") << "}";
    return out.str();
}

// effect option (chain / activation) — includes which of a card's effects this
// is (the description Stringid encodes the effect index, 0-based -> report
// 1-based) and where the card is.  effect==0 means the script set no
// description, so the UI should not label it "effect N".
static std::string effect_opt_json(const uint32_t code, const uint8_t ctrl, const uint8_t loc,
                                   const uint32_t seq, const uint64_t desc, const bool is_trigger = false) {
    const uint32_t effect = (desc && desc > 0x7ff) ? static_cast<uint32_t>((desc & 0xfffff) + 1) : 0;
    const std::string dtext = card_desc_string(desc);
    // no card description: fall back to naming the trigger window so the player
    // can still tell an on-summon trigger from the card's ignition effect
    const char *trigger = (!dtext.empty() || !just_summoned(ctrl, loc, seq)) ? "" : "summon";
    std::ostringstream out;
    out << "{\"code\":" << code
            << ",\"name\":" << json::str(card_name_ui(code))
            << ",\"p\":" << static_cast<int>(ctrl)
            << ",\"loc\":" << static_cast<int>(loc)
            << ",\"locname\":" << json::str(loc_name(loc))
            << ",\"seq\":" << seq
            << ",\"effect\":" << effect
            // `trig` is the engine's own EFFECT_TYPE_TRIGGER_* flag: a "when ..."
            // effect the player may activate right now.  The client must never
            // auto-pass a window that offers one.
            << ",\"trig\":" << (is_trigger ? "true" : "false")
            << ",\"trigger\":" << json::str(trigger)
            << ",\"desc\":" << json::str(dtext) << "}";
    return out.str();
}

// ---------------------------------------------------------------------------
// prompt emission + action reading
// ---------------------------------------------------------------------------
static bool g_interactive = false; // set by --interactive
static bool g_assess = false; // assess mode: stop at AI's first search
static std::string g_scenario; // scenario .lua to load instead of decks (set in main)
static std::string g_scenario_state; // scenario initial-state JSON for the field model
static FILE *g_in = nullptr; // stdin (line protocol)

// ---- recording and replay (--record / --replay / --verify) -----------------
// A recording is the runner's own JSONL output with the answers interleaved, in
// order: everything a client saw, plus everything the players decided.  The same
// seed, the same decks and those answers reproduce the duel exactly (see
// .smoke/replay_determinism_test.py), which is what makes a stored game both a
// thing to watch and a thing to re-run.  Event lines and answers are told apart
// by their key: an answer is the server's `{"act": ...}` line, an event is
// `{"ev": ...}`.
static FILE *g_record = nullptr;             // --record: transcript being written
static bool g_replay = false;                // --replay: answers come from a file
static bool g_verify = false;                // --verify: compare events as they come
static std::vector<std::string> g_replay_acts;    // recorded answers, in order
static std::vector<std::string> g_replay_events;  // recorded events, in order
static size_t g_replay_acts_at = 0;
static size_t g_replay_events_at = 0;
static size_t g_replay_mismatches = 0;
static bool g_replay_ran_out = false;        // replay needed an answer nobody gave

// Is this recorded line one of the runner's events (as opposed to an answer)?
static bool record_line_is_event(const std::string &line) {
    return line.find("\"ev\":") != std::string::npos;
}

// Load a transcript: header + events + answers.  Returns false if it cannot be
// read.  The header (`{"ev":"replay",...}`) is read for the seed, so replaying a
// file needs no flags repeated.
static bool load_replay(const std::string &path, uint64_t *seed_out) {
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[65536];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        if (line.find("\"ev\":\"replay\"") != std::string::npos) {
            if (seed_out) {
                const size_t k = line.find("\"seed\":");
                if (k != std::string::npos) *seed_out = std::strtoull(line.c_str() + k + 7, nullptr, 10);
            }
            continue;                       // the header is metadata, not a step
        }
        if (record_line_is_event(line)) g_replay_events.push_back(line);
        else g_replay_acts.push_back(line);
    }
    std::fclose(f);
    return true;
}

// Open the transcript being written and put the header on the first line.
static bool start_recording(const std::string &path, const std::string &header) {
    g_record = std::fopen(path.c_str(), "w");
    if (!g_record) return false;
    std::fprintf(g_record, "%s\n", header.c_str());
    std::fflush(g_record);
    return true;
}

static void emit_line(const std::string &line) {
    if (g_record) {
        std::fprintf(g_record, "%s\n", line.c_str());
        std::fflush(g_record);
    }
    if (g_verify) {
        // compare against what was recorded, in order: the first difference is
        // where the replay stopped being the same game
        if (g_replay_events_at < g_replay_events.size()) {
            const std::string &want = g_replay_events[g_replay_events_at];
            if (want != line) {
                if (g_replay_mismatches == 0) {
                    std::fprintf(stderr, "[replay] first divergence at event %zu\n  recorded: %s\n  replayed: %s\n",
                                 g_replay_events_at, want.substr(0, 300).c_str(), line.substr(0, 300).c_str());
                }
                ++g_replay_mismatches;
            }
        } else {
            ++g_replay_mismatches;          // the replay produced an extra event
        }
        ++g_replay_events_at;
    }
    std::fprintf(stdout, "%s\n", line.c_str());
    std::fflush(stdout);
}

static void emit_state() {
    if (g_multiplayer) {
        // emit both perspectives so each client sees the game from its own seat
        emit_line(serialize_state_json(0));
        emit_line(serialize_state_json(1));
    } else {
        emit_line(serialize_state_json(0));
    }
}

// Emit a structured animation hint for the frontend.
static void anim_game_event(uint8_t type, Msg &msg) {
    switch (type) {
        case MSG_SUMMONING:
        case MSG_SPSUMMONING:
        case MSG_FLIPSUMMONING: {
            // payload: [code  u32][ctrl u8][loc u8][seq u32][pos u32]
            uint32_t card_code = msg.u32();
            uint8_t controller = msg.u8();
            uint8_t location = msg.u8();
            uint32_t sequence = msg.u32();
            uint32_t position = msg.u32();
            const char *kind = (type == MSG_SUMMONING)
                                   ? "summon"
                                   : (type == MSG_SPSUMMONING)
                                         ? "spsummon"
                                         : "flipsummon";
            std::ostringstream out;
            out << R"({"ev":"anim","kind":")" << kind << "\""
                    << ",\"p\":" << static_cast<int>(controller)
                    << ",\"loc\":" << static_cast<int>(location)
                    << ",\"seq\":" << sequence
                    << ",\"pos\":" << position
                    << ",\"code\":" << card_code
                    << ",\"name\":" << json::str(card_name_ui(card_code))
                    << ",\"turn\":" << g_turns << "}";
            emit_line(out.str());
            if (type == MSG_FLIPSUMMONING) {
                // a Flip Summon is a face-down DEF monster turning face-up in
                // Attack Position: flip the card over and straighten it at once
                std::ostringstream f;
                f << R"({"ev":"anim","kind":"flip")"
                        << ",\"p\":" << static_cast<int>(controller)
                        << ",\"loc\":" << static_cast<int>(location)
                        << ",\"seq\":" << sequence
                        << ",\"code\":" << card_code
                        << ",\"name\":" << json::str(card_name_ui(card_code))
                        << ",\"to\":\"up\""
                        << ",\"frompos\":" << 0x8
                        << ",\"topos\":" << position
                        << ",\"flipsummon\":true"
                        << ",\"turn\":" << g_turns << "}";
                emit_line(f.str());
            }
            // remember the summon so a description-less trigger prompt that
            // follows can be labelled ("on-summon effect")
            mark_just_summoned(controller, location, sequence);
            note_significant_action();
            return;
        }
        case MSG_ATTACK: {
            // payload: attacker loc_info, then target loc_info (all-zero =
            // direct attack). loc_info = [ctrl u8][loc u8][seq u32][pos u32].
            // The attacker's code is NOT part of MSG_ATTACK — resolve it from
            // the field model (same as log_game_event does).
            uint8_t attacker_controller = msg.u8();
            uint8_t attacker_location = msg.u8();
            uint32_t attacker_sequence = msg.u32();
            uint32_t attacker_position = msg.u32();
            uint8_t target_controller = msg.u8();
            uint8_t target_location = msg.u8();
            uint32_t target_sequence = msg.u32();
            uint32_t target_position = msg.u32();
            uint32_t code = 0;
            for (auto &kv: g_field.zone[attacker_controller])
                if (kv.first.first == attacker_location && kv.first.second == attacker_sequence) {
                    code = kv.second.code;
                    break;
                }
            note_significant_action();
            std::ostringstream out;
            out << R"({"ev":"anim","kind":"attack")"
                    << ",\"p\":" << static_cast<int>(attacker_controller)
                    << ",\"loc\":" << static_cast<int>(attacker_location)
                    << ",\"seq\":" << attacker_sequence
                    << ",\"pos\":" << attacker_position
                    << ",\"code\":" << code
                    << ",\"name\":" << json::str(card_name_ui(code))
                    << ",\"tp\":" << static_cast<int>(target_controller)
                    << ",\"tloc\":" << static_cast<int>(target_location)
                    << ",\"tseq\":" << target_sequence
                    << ",\"tpos\":" << target_position
                    << ",\"turn\":" << g_turns << "}";
            emit_line(out.str());
            return;
        }
        case MSG_DAMAGE: {
            uint8_t player = msg.u8();
            uint32_t amount = msg.u32();
            std::ostringstream out;
            out << R"({"ev":"anim","kind":"damage")"
                    << ",\"p\":" << static_cast<int>(player)
                    << ",\"amt\":" << static_cast<int>(amount) << "}";
            emit_line(out.str());
            return;
        }
        case MSG_RECOVER: {
            uint8_t player = msg.u8();
            uint32_t amount = msg.u32();
            std::ostringstream out;
            out << R"({"ev":"anim","kind":"recover")"
                    << ",\"p\":" << static_cast<int>(player)
                    << ",\"amt\":" << static_cast<int>(amount) << "}";
            emit_line(out.str());
            return;
        }
        case MSG_POS_CHANGE: {
            // payload: [code u32][ctrl u8][loc u8][seq u8][prevpos u8][curpos u8]
            uint32_t code = msg.u32();
            uint8_t ctrl = msg.u8();
            uint8_t loc = msg.u8();
            uint32_t seq = msg.u8();
            uint8_t prev = msg.u8();
            uint8_t cur = msg.u8();
            const bool up_before = (prev & 0x5) != 0;   // face-up attack | defense
            const bool up_after = (cur & 0x5) != 0;
            if (up_before == up_after) return;          // a pure ATK/DEF shift: no flip
            std::ostringstream out;
            out << R"({"ev":"anim","kind":"flip")"
                    << ",\"p\":" << static_cast<int>(ctrl)
                    << ",\"loc\":" << static_cast<int>(loc)
                    << ",\"seq\":" << seq
                    << ",\"code\":" << code
                    << ",\"name\":" << json::str(card_name_ui(code))
                    << ",\"to\":\"" << (up_after ? "up" : "down") << "\""
                    << ",\"frompos\":" << static_cast<int>(prev)
                    << ",\"topos\":" << static_cast<int>(cur)
                    << ",\"flipsummon\":false"
                    << ",\"turn\":" << g_turns << "}";
            emit_line(out.str());
            return;
        }
        case MSG_CHAINING: {
            // remember which card activated each link so a later
            // MSG_CHAIN_NEGATED / MSG_CHAIN_DISABLED can point at it
            uint32_t code = msg.u32();
            uint8_t ctrl = msg.u8();
            uint8_t loc = msg.u8();
            uint32_t seq = msg.u32();
            msg.u32();                 // position
            msg.u8();                  // triggering controller
            msg.u8();                  // triggering location
            msg.u32();                 // triggering sequence
            const uint64_t desc = msg.u64();   // effect description Stringid
            uint32_t link = msg.u32(); // 1-based chain count
            if (link <= 256) g_chain_links[link] = ChainLinkAnim{code, ctrl, loc, seq};
            g_chain_size = link;
            note_significant_action();
            // the activation burst the frontend plays out of the card: the
            // frame of the card decides its tint (monster = orange — every
            // monster type, including tokens/extra deck —, spell = green,
            // trap = purple).  `loc`/`seq` name the card it emerges from; a
            // hand activation is a hand card, a Graveyard effect is the GY pile.
            uint32_t type_bits = 0;
            card_type_bits(code, type_bits);
            const char *ctype = (type_bits & TYPE_SPELL) ? "spell"
                                : (type_bits & TYPE_TRAP) ? "trap"
                                                          : "monster";
            std::ostringstream ao;
            ao << R"({"ev":"anim","kind":"activate")"
                    << ",\"p\":" << static_cast<int>(ctrl)
                    << ",\"loc\":" << static_cast<int>(loc)
                    << ",\"seq\":" << seq
                    << ",\"code\":" << code
                    << ",\"ctype\":\"" << ctype << "\""
                    << ",\"chain\":" << link
                    << ",\"turn\":" << g_turns << "}";
            emit_line(ao.str());
            // An activation is the one "this card was used" fact the event stream
            // does not otherwise carry: summons, sets and attacks have anim
            // hints, while a chain link is only visible in the log text ("You
            // activate X").  Replays and per-card statistics both need it as
            // data, so it is emitted here instead of parsed back out of a
            // human-readable line later.
            std::ostringstream uo;
            uo << R"({"ev":"use","kind":"activate")"
                    << ",\"p\":" << static_cast<int>(ctrl)
                    << ",\"loc\":" << static_cast<int>(loc)
                    << ",\"seq\":" << seq
                    << ",\"code\":" << code
                    << ",\"ctype\":\"" << ctype << "\""
                    << ",\"chain\":" << link
                    << ",\"turn\":" << g_turns
                    << ",\"desc\":" << json::str(card_desc_string(desc))
                    << "}";
            emit_line(uo.str());
            return;
        }
        case MSG_CHAIN_END: {
            g_chain_links.clear();
            g_chain_size = 0;
            return;
        }
        case MSG_SET: {
            // a Set is a meaningful action (my monster hit the board face-down)
            note_significant_action();
            return;
        }
        case MSG_NEW_TURN:
        case MSG_NEW_PHASE: {
            // the summon window is over: stop labelling later prompts with it,
            // and forget the last action — moving through phases is noise
            clear_just_summoned();
            clear_significant_action();
            return;
        }
        case MSG_SELECT_IDLECMD:
        case MSG_SELECT_BATTLECMD: {
            // the summon window is over: stop labelling later prompts with it
            clear_just_summoned();
            return;
        }
        case MSG_CHAIN_NEGATED:
        case MSG_CHAIN_DISABLED: {
            const uint32_t link = msg.u8();
            ChainLinkAnim info;
            auto it = g_chain_links.find(link);
            if (it != g_chain_links.end()) info = it->second;
            std::ostringstream out;
            out << R"({"ev":"anim","kind":"negate")"
                    << ",\"p\":" << static_cast<int>(info.ctrl)
                    << ",\"loc\":" << static_cast<int>(info.loc)
                    << ",\"seq\":" << info.seq
                    << ",\"code\":" << info.code
                    << ",\"name\":" << json::str(card_name_ui(info.code))
                    << ",\"link\":" << link
                    << ",\"disabled\":" << (type == MSG_CHAIN_DISABLED ? "true" : "false")
                    << ",\"turn\":" << g_turns << "}";
            emit_line(out.str());
            return;
        }
        case MSG_TOSS_COIN: {
            uint8_t player = msg.u8();
            int count = msg.u8();
            std::ostringstream out;
            out << R"({"ev":"anim","kind":"coin")"
                    << ",\"p\":" << static_cast<int>(player)
                    << ",\"results\":[";
            for (int i = 0; i < count; ++i) {
                if (i) out << ",";
                out << (msg.u8() != 0 ? "1" : "0");
            }
            out << "],\"turn\":" << g_turns << "}";
            emit_line(out.str());
            return;
        }
        case MSG_TOSS_DICE: {
            uint8_t player = msg.u8();
            int count = msg.u8();
            std::ostringstream out;
            out << R"({"ev":"anim","kind":"dice")"
                    << ",\"p\":" << static_cast<int>(player)
                    << ",\"results\":[";
            for (int i = 0; i < count; ++i) {
                if (i) out << ",";
                out << static_cast<int>(msg.u8());
            }
            out << "],\"turn\":" << g_turns << "}";
            emit_line(out.str());
            return;
        }
        case MSG_MISSED_EFFECT: {
            // payload: loc_info (ctrl u8, loc u8, seq u32, pos u32) + code u32
            uint8_t ctrl = msg.u8();
            uint8_t loc = msg.u8();
            uint32_t seq = msg.u32();
            msg.u32();
            uint32_t code = msg.u32();
            const bool reveal = (loc != 0x02);   // hand cards stay anonymous
            std::ostringstream out;
            out << R"({"ev":"anim","kind":"missed")"
                    << ",\"p\":" << static_cast<int>(ctrl)
                    << ",\"loc\":" << static_cast<int>(loc)
                    << ",\"seq\":" << seq
                    << ",\"code\":" << (reveal ? code : 0)
                    << ",\"name\":" << json::str(reveal ? card_name_ui(code) : std::string("a card"))
                    << ",\"hidden\":" << (reveal ? "false" : "true")
                    << ",\"turn\":" << g_turns << "}";
            emit_line(out.str());
            return;
        }
        default:
            return;
    }
}


// read one "act" JSON line; returns the raw line or "" on EOF
static std::string read_action_line() {
    if (g_replay) {
        // the recorded answers, in the order they were given
        if (g_replay_acts_at >= g_replay_acts.size()) {
            g_replay_ran_out = true;        // the replay asked for a move nobody made
            return "";
        }
        return g_replay_acts[g_replay_acts_at++];
    }
    std::string line;
    int c;
    while ((c = fgetc(g_in)) != EOF) {
        if (c == '\n') break;
        line += static_cast<char>(c);
    }
    // the answer is part of the transcript: it is the half that cannot be
    // recovered from the events, and the half a re-simulation needs
    if (g_record && !line.empty()) {
        std::fprintf(g_record, "%s\n", line.c_str());
        std::fflush(g_record);
    }
    return line;
}

// tiny JSON value scanner for {"kind":..., ...} action objects
namespace actjson {
    // One parsed JSON scalar. Exactly one of the flags is set for a valid
    // value; `s` holds string tokens and `i` holds numbers.
    struct Val {
        std::string s; // the raw token when the value was a JSON string
        long long i = 0; // the numeric value when it was a JSON number
        bool is_num = false; // true if this value was a number (use `i`)
        bool is_true = false; // true if the literal `true`
        bool is_false = false; // true if the literal `false`
    };

    // parse one JSON value starting at pos; advance pos
    static Val parse(const std::string &t, size_t &pos) {
        Val value;
        while (pos < t.size() && isspace(static_cast<unsigned char>(t[pos]))) ++pos;
        if (pos >= t.size()) return value;
        const char ch = t[pos];
        if (ch == '"') {
            ++pos;
            while (pos < t.size() && t[pos] != '"') { value.s += t[pos++]; }
            if (pos < t.size()) ++pos;
        } else if (ch == 't') {
            value.is_true = true;
            pos += 4;
        } else if (ch == 'f') {
            value.is_false = true;
            pos += 5;
        } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
            const bool neg = (ch == '-');
            if (neg) ++pos;
            long long n = 0;
            while (pos < t.size() && t[pos] >= '0' && t[pos] <= '9') {
                n = n * 10 + (t[pos] - '0');
                ++pos;
            }
            value.i = neg ? -n : n;
            value.is_num = true;
        } else if (ch == '{' || ch == '[') {
            // skip the whole object/array token
            int depth = 0;
            while (pos < t.size()) {
                if (t[pos] == '{' || t[pos] == '[') ++depth;
                else if (t[pos] == '}' || t[pos] == ']') {
                    --depth;
                    if (depth == 0) {
                        ++pos;
                        break;
                    }
                }
                pos++;
            }
            value.is_false = true; // placeholder: not used for objects
        }
        return value;
    }

    static Val field(const std::string &t, const std::string &key) {
        // naive key search: "\"key\"" followed by ':'
        const size_t k = t.find("\"" + key + "\"");
        if (k == std::string::npos) return Val{};
        size_t p = k + key.size() + 2;
        while (p < t.size() && isspace(static_cast<unsigned char>(t[p]))) ++p;
        if (p < t.size() && t[p] == ':') {
            ++p;
            return parse(t, p);
        }
        return Val{};
    }

    // parse any "[a,b,...]" numeric array under `key`
    static std::vector<int> array_of(const std::string &t, const std::string &key) {
        std::vector<int> out;
        const size_t k = t.find("\"" + key + "\"");
        if (k == std::string::npos) return out;
        size_t p = t.find('[', k);
        if (p == std::string::npos) return out;
        ++p;
        while (p < t.size() && t[p] != ']') {
            while (p < t.size() && (t[p] == ',' || isspace(static_cast<unsigned char>(t[p])))) ++p;
            if (p >= t.size() || t[p] == ']') break;
            const Val value = parse(t, p);
            if (value.is_num) out.push_back(static_cast<int>(value.i));
        }
        return out;
    }

    // parse "indices":[a,b,...]
    static std::vector<int> indices(const std::string &t) {
        return array_of(t, "indices");
    }

    // parse "zones":[[p,loc,seq],...]
    static std::vector<std::tuple<int, int, int> > zones(const std::string &t) {
        std::vector<std::tuple<int, int, int> > out;
        const size_t k = t.find("\"zones\"");
        if (k == std::string::npos) return out;
        size_t p = t.find('[', k);
        if (p == std::string::npos) return out;
        ++p;
        while (p < t.size() && t[p] != ']') {
            if (t[p] != '[') {
                ++p;
                continue;
            }
            ++p;
            const Val a = parse(t, p);
            while (p < t.size() && t[p] != ',') ++p;
            if (p < t.size()) ++p;
            const Val b = parse(t, p);
            while (p < t.size() && t[p] != ',') ++p;
            if (p < t.size()) ++p;
            const Val c = parse(t, p);
            while (p < t.size() && t[p] != ']') ++p;
            if (p < t.size()) ++p;
            if (a.is_num && b.is_num && c.is_num)
                out.emplace_back(static_cast<int>(a.i), static_cast<int>(b.i), static_cast<int>(c.i));
        }
        return out;
    }
} // namespace actjson

// forward decl for the binary-response helper used by both paths
static void set_response(intptr_t duel_handle, const std::vector<uint8_t> &b);

static void set_response_i(intptr_t duel_handle, int32_t v);

// ---------------------------------------------------------------------------
// human prompt handling — one function per SELECT message type.
//
// Every handler follows the same four steps:
//   1. parse the message payload with the Msg cursor (field order matches the
//      core's MSG_* layout — see the protocol docs / handle_select),
//   2. emit_state()  -> send the fresh board snapshot,
//   3. emit_line()   -> send {"ev":"prompt","type":"...", ...},
//   4. read_action_line() + set_response()/set_response_i() -> encode the
//      browser's {"act": {...}} answer into the core's binary response.
//
// Return value: false means "handled, response already set" (the caller does
// not need to fall back to the AI). The AI responder is only a fallback for
// message types this file does not know about.
// ---------------------------------------------------------------------------
static bool human_yesno(const intptr_t duel_handle, Msg &msg, const char *kind) {
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":")" << kind << R"(","player":)" << static_cast<int>(g_active_prompt_player) << "}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const actjson::Val value = actjson::field(line, "value");
    set_response_i(duel_handle, (value.is_num && value.i != 0) ? 1 : 0);
    return false;
}

// Optional-trigger "Yes / No" prompt: a card effect is offering to activate
// (e.g. Swap Frog). The card is identified by code + loc_info; the description
// Stringid resolves to the effect text so the UI can say what is activating.
static bool human_effectyn(const intptr_t duel_handle, Msg &msg) {
    msg.u8();
    const uint32_t code = msg.u32();
    const uint8_t ctrl = msg.u8();
    const uint8_t loc = msg.u8();
    const uint32_t seq = msg.u32();
    msg.u32();
    const uint64_t desc = msg.u64();
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"effectyn","player":)" << static_cast<int>(g_active_prompt_player) << ",\"card\":"
            << effect_opt_json(code, ctrl, loc, seq, desc) << "}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const actjson::Val value = actjson::field(line, "value");
    set_response_i(duel_handle, (value.is_num && value.i != 0) ? 1 : 0);
    return false;
}

// SelectOption prompt: pick one of several labelled choices (e.g. Gravirose's
// End Phase options). Option values are usually description Stringids, so each
// is decoded to (card code, effect number, resolved text) for the UI.
static bool human_option(const intptr_t duel_handle, Msg &msg) {
    msg.u8();
    const int n = msg.u8();
    std::vector<uint64_t> opts(n);
    for (int i = 0; i < n; ++i) opts[i] = msg.u64();
    std::vector<uint32_t> codes(n, 0);
    std::vector<std::string> descs(n);
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"option","player":)" << static_cast<int>(g_active_prompt_player) << ",\"count\":" << n
            << ",\"options\":[";
    for (int i = 0; i < n; ++i) {
        if (i) out << ",";
        // option values are usually effect description Stringids: decode which
        // card + which effect so the UI can label them (0-based -> 1-based)
        const auto code = static_cast<uint32_t>(opts[i] >> 20);
        const uint32_t effect = (opts[i] && opts[i] > 0x7ff) ? static_cast<uint32_t>((opts[i] & 0xfffff) + 1) : 0;
        codes[i] = code;
        descs[i] = card_desc_string(opts[i]);
        out << "{\"value\":" << opts[i] << ",\"code\":" << code << ",\"effect\":" << effect
                << ",\"desc\":" << json::str(descs[i]) << "}";
    }
    out << "]}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const actjson::Val value = actjson::field(line, "index");
    int idx = static_cast<int>(value.is_num ? value.i : 0);
    if (idx < 0 || idx >= n) idx = 0;
    // tell the opponent which option this was, so "Activate 1 of these effects"
    // never stands on its own
    log_choice(g_active_prompt_player, codes[idx],
               descs[idx].empty() ? ("option " + std::to_string(idx + 1)) : descs[idx]);
    set_response_i(duel_handle, idx);
    return false;
}

// SelectCard / SelectTribute prompt: choose min..max of the listed cards.
// `is_tribute` handles MSG_SELECT_TRIBUTE's slightly different payload
// (no position; tribute targets are always on-field monsters).
static bool human_select_card(const intptr_t duel_handle, Msg &msg, const bool is_tribute) {
    msg.u8(); // player
    const int cancelable = msg.u8();
    const int min = static_cast<int>(msg.u32());
    const int max = static_cast<int>(msg.u32());
    const int n = static_cast<int>(msg.u32());
    std::vector<std::string> opts(n);
    for (int i = 0; i < n; ++i) {
        const uint32_t code = msg.u32();
        const uint8_t ctrl = msg.u8();
        uint8_t loc = msg.u8();
        uint32_t seq = msg.u32();
        uint32_t pos;
        if (is_tribute) {
            // MSG_SELECT_TRIBUTE has release_param (1 byte) and NO position.
            // Recover the position from the field model (tribute targets are
            // on-field monsters).
            msg.u8(); // release_param
            pos = 0x1; // default face-up attack
            auto it = g_field.zone[ctrl].find({loc, seq});
            if (it != g_field.zone[ctrl].end()) pos = it->second.pos;
        } else {
            pos = msg.u32();
        }
        // hide the opponent's face-down field cards (attack targets, backrow)
        opts[i] = card_opt_json_secret(code, pos, ctrl, loc, seq, g_active_prompt_player);
    }
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":")" << (is_tribute ? "tribute" : "card")
            << R"(","player":)" << static_cast<int>(g_active_prompt_player) << ",\"min\":" << min << ",\"max\":" << max
            << ",\"cancelable\":" << (cancelable ? "true" : "false")
            << ",\"cards\":[";
    for (int i = 0; i < n; ++i) {
        if (i) out << ",";
        out << opts[i];
    }
    out << "]}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const actjson::Val cv = actjson::field(line, "cancel");
    if (cv.is_true && cancelable && min == 0) {
        set_response_i(duel_handle, -1);
        return false;
    }
    std::vector<int> idx = actjson::indices(line);
    if (idx.size() < static_cast<size_t>(min)) idx.clear(); // invalid -> cancel if allowed
    std::vector<uint8_t> b(8 + idx.size());
    constexpr int32_t t = 2;
    std::memcpy(b.data(), &t, 4);
    const auto c = static_cast<uint32_t>(idx.size());
    std::memcpy(b.data() + 4, &c, 4);
    for (size_t i = 0; i < idx.size(); ++i) b[8 + i] = static_cast<uint8_t>(idx[i]);
    set_response(duel_handle, b);
    return false;
}

// MSG_SELECT_SUM: pick cards whose combined value reaches `acc` (e.g. Machina
// Fortress's discard of Machine monsters whose total Level is 8+).  Must-select
// cards are auto-counted; the player chooses from the free selection list.
// SelectSum prompt: pick cards whose combined value (usually Level) reaches
// the accumulator (e.g. Machina Fortress discarding Machines totalling 8+).
// Mandatory cards are auto-counted; only the free picks are presented.
static bool human_sum(intptr_t duel_handle, Msg &msg) {
    msg.u8(); // player
    int mode = msg.u8();
    int acc = static_cast<int>(msg.u32() & 0xffff);
    int min = static_cast<int>(msg.u32()), max = static_cast<int>(msg.u32());
    int mcount = static_cast<int>(msg.u32());
    int must_sum = 0;
    for (int i = 0; i < mcount; ++i) {
        msg.u32();
        uint8_t ctrl = msg.u8();
        uint8_t loc = msg.u8();
        msg.u32();
        msg.u32();
        auto lv = static_cast<int32_t>(msg.u32());
        (void) ctrl;
        (void) loc;
        must_sum += lv;
    }
    int n = static_cast<int>(msg.u32());
    std::vector<std::string> opts(n);
    for (int i = 0; i < n; ++i) {
        uint32_t code = msg.u32();
        uint8_t ctrl = msg.u8();
        uint8_t loc = msg.u8();
        uint32_t seq = msg.u32();
        msg.u32();
        auto lv = static_cast<int32_t>(msg.u32());
        std::ostringstream out;
        out << "{\"code\":" << code << ",\"name\":" << json::str(card_name_ui(code))
                << ",\"p\":" << static_cast<int>(ctrl) << ",\"loc\":" << static_cast<int>(loc)
                << ",\"locname\":" << json::str(loc_name(loc))
                << ",\"seq\":" << seq << ",\"level\":" << lv << "}";
        opts[i] = out.str();
    }
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"sum","player":)" << static_cast<int>(g_active_prompt_player)
            << ",\"acc\":" << (acc - must_sum)
            << ",\"min\":" << min << ",\"max\":" << max
            << ",\"mode\":" << mode << ",\"cards\":[";
    for (int i = 0; i < n; ++i) {
        if (i) out << ",";
        out << opts[i];
    }
    out << "]}";
    emit_line(out.str());
    std::string line = read_action_line();
    std::vector<int> idx = actjson::indices(line);
    std::vector<uint8_t> b(8 + idx.size());
    int32_t t = 2;
    std::memcpy(b.data(), &t, 4);
    auto c = static_cast<uint32_t>(idx.size());
    std::memcpy(b.data() + 4, &c, 4);
    for (size_t i = 0; i < idx.size(); ++i) b[8 + i] = static_cast<uint8_t>(idx[i]);
    set_response(duel_handle, b);
    return false;
}

// SelectChain prompt: chain an effect to the current chain, or pass.
// If `forced` is set, the chain response is mandatory (no pass).
static bool human_chain(const intptr_t duel_handle, Msg &msg) {
    msg.u8();
    msg.u8();
    const int forced = msg.u8();
    msg.skip(8);
    const int n = static_cast<int>(msg.u32());
    std::vector<std::string> opts(n);
    for (int i = 0; i < n; ++i) {
        const uint32_t code = msg.u32();
        const uint8_t ctrl = msg.u8();
        const uint8_t loc = msg.u8();
        const uint32_t seq = msg.u32();
        msg.u32();
        const uint64_t desc = msg.u64();
        msg.u8();                       // client_mode
        const uint8_t trig = msg.u8();  // effect type: optional/mandatory trigger
        opts[i] = effect_opt_json(code, ctrl, loc, seq, desc, trig != 0);
    }
    emit_state();
    std::ostringstream out;
    const uint8_t me = g_active_prompt_player;
    // `action` tells the client whether anything worth responding to has
    // happened since it was last asked (an activation, summon, set or attack —
    // as opposed to simply moving through phases).  The auto-pass toggle uses
    // it to skip pure phase/timing windows.  One-shot per player: the first
    // prompt after the action reports it, later ones in the same phase do not.
    const bool acted = g_recent_action && !g_action_reported[me < 2 ? me : 0];
    if (me < 2) g_action_reported[me] = true;
    out << R"({"ev":"prompt","type":"chain","player":)" << static_cast<int>(me) << ",\"forced\":"
            << (forced ? "true" : "false")
            << ",\"chain\":" << g_chain_size
            << ",\"action\":" << (acted ? "true" : "false") << ",\"options\":[";
    for (int i = 0; i < n; ++i) {
        if (i) out << ",";
        out << opts[i];
    }
    out << "]}";
    emit_line(out.str());
    const std::string line = read_action_line();
    if (forced) {
        set_response_i(duel_handle, 0);
        return false;
    }
    const actjson::Val pv = actjson::field(line, "pass");
    if (pv.is_true) {
        set_response_i(duel_handle, -1);
        return false;
    }
    const actjson::Val iv = actjson::field(line, "index");
    int idx = static_cast<int>(iv.is_num ? iv.i : -1);
    if (idx < 0 || idx >= n) idx = -1;
    set_response_i(duel_handle, idx);
    return false;
}

// SelectPosition prompt: choose a battle position for a card.
// Payload: [player u8][card code u32][positions u8].  `positions` is a
// bitmask of the allowed POS_* values.  The card code names the monster being
// positioned (several monsters summoned together each get their own prompt,
// so the UI can say which one it is asking about).
static bool human_position(const intptr_t duel_handle, Msg &msg) {
    msg.u8();
    const uint32_t code = msg.u32();
    const int positions = msg.u8();
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"position","player":)" << static_cast<int>(g_active_prompt_player)
        << ",\"code\":" << code
        << ",\"name\":" << json::str(code ? card_name_ui(code) : "")
        << ",\"positions\":" << positions << "}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const actjson::Val value = actjson::field(line, "pos");
    const int pos = static_cast<int>(value.is_num ? value.i : 1);
    set_response_i(duel_handle, pos);
    return false;
}

// SelectPlace / SelectDisfield prompt: choose free zones for one or more
// cards (e.g. Pendulum scale placement). If the UI sends no zones, the
// runner falls back to picking free zones itself.
static bool human_place(const intptr_t duel_handle, Msg &msg) {
    const int player = msg.u8();
    const int count = msg.u8();
    const uint32_t flag = msg.u32();
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"place","player":)" << static_cast<int>(g_active_prompt_player) << ",\"count\":" << count
            << ",\"flag\":" << flag << "}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const auto zs = actjson::zones(line);
    std::vector<uint8_t> b;
    for (size_t i = 0; i < zs.size() && i < static_cast<size_t>(count); ++i) {
        b.push_back(static_cast<uint8_t>(std::get<0>(zs[i])));
        b.push_back(static_cast<uint8_t>(std::get<1>(zs[i])));
        b.push_back(static_cast<uint8_t>(std::get<2>(zs[i])));
    }
    if (b.empty() || b.size() < static_cast<size_t>(count) * 3) {
        // fall back to picking free zones (same logic as the AI responder)
        std::vector<std::tuple<uint8_t, uint8_t, uint8_t> > zones;
        for (int p = 0; p < 2; ++p)
            for (const int l: {0x04, 0x08}) {
                const int max_s = (l == 0x04) ? 6 : 7;
                for (int s = 0; s <= max_s; ++s) {
                    const uint32_t bit = 1u << (s + (p == player ? 0 : 16) + (l == 0x04 ? 0 : 8));
                    if (!(bit & flag))
                        zones.emplace_back(static_cast<uint8_t>(p), static_cast<uint8_t>(l), static_cast<uint8_t>(s));
                }
            }
        std::vector<uint8_t> b2;
        for (size_t i = 0; i < zones.size() && i < static_cast<size_t>(count); ++i) {
            b2.push_back(std::get<0>(zones[i]));
            b2.push_back(std::get<1>(zones[i]));
            b2.push_back(std::get<2>(zones[i]));
        }
        if (b2.empty()) {
            set_response_i(duel_handle, 0);
            return false;
        }
        set_response(duel_handle, b2);
        return false;
    }
    set_response(duel_handle, b);
    return false;
}

// generic: parse (player,count,[(code,ctrl,loc,seq),...]) groups.
// The location is kept: the client lights up the pile the offer belongs to (a
// Graveyard summon procedure like Machina Fortress lives in the GY tray, an
// Extra Deck summon in the Extra tray) and hands back the index the engine
// listed, so a code alone is not enough — two copies of the same card can be in
// two different piles.
static bool human_idlecmd(intptr_t duel_handle, Msg &msg) {
    msg.u8();
    auto group = [&msg]() {
        const int n = static_cast<int>(msg.u32());
        std::vector<std::string> opts(n);
        for (int i = 0; i < n; ++i) {
            const uint32_t code = msg.u32();
            const uint8_t ctrl = msg.u8();
            const uint8_t loc = msg.u8();
            const uint32_t seq = msg.u32();
            opts[i] = card_opt_json_secret(code, 0, ctrl, loc, seq, g_active_prompt_player);
        }
        return opts;
    };
    std::vector<std::string> summon = group();
    std::vector<std::string> spsummon = group();
    // repo: repositionable field monsters (code + ctrl + loc + u8 seq)
    int n_repo = static_cast<int>(msg.u32());
    std::vector<std::string> repo_opts(n_repo);
    for (int i = 0; i < n_repo; ++i) {
        uint32_t code = msg.u32();
        uint8_t ctrl = msg.u8();
        uint8_t loc = msg.u8();
        uint8_t seq = msg.u8();
        std::ostringstream ro;
        ro << "{\"code\":" << code << ",\"name\":" << json::str(card_name_ui(code))
                << ",\"p\":" << static_cast<int>(ctrl) << ",\"loc\":" << static_cast<int>(loc) << ",\"seq\":" << static_cast<int>(seq) << "}";
        repo_opts[i] = ro.str();
    }
    std::vector<std::string> mset = group();
    std::vector<std::string> sset = group();
    // activate list carries a description Stringid (which effect) + location,
    // so it is parsed separately from the plain code groups above
    int n_act = static_cast<int>(msg.u32());
    std::vector<std::string> act_opts(n_act);
    for (int i = 0; i < n_act; ++i) {
        uint32_t code = msg.u32();
        uint8_t ctrl = msg.u8();
        uint8_t loc = msg.u8();
        uint32_t seq = msg.u32();
        uint64_t desc = msg.u64();
        msg.u8();
        act_opts[i] = effect_opt_json(code, ctrl, loc, seq, desc);
    }
    int to_bp = msg.u8(), to_ep = msg.u8(), can_shuffle = msg.u8();
    emit_state();
    auto list = [](const char *name, const std::vector<std::string> &v) {
        std::ostringstream out;
        out << ",\"" << name << "\":[";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out << ",";
            out << v[i];
        }
        out << "]";
        return out.str();
    };
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"idle","player":)" << static_cast<int>(g_active_prompt_player) << "";
    out << list("summon", summon) << list("spsummon", spsummon)
            << list("mset", mset) << list("sset", sset);
    out << ",\"repo\":[";
    for (size_t i = 0; i < repo_opts.size(); ++i) {
        if (i) out << ",";
        out << repo_opts[i];
    }
    out << "]";
    out << ",\"act\":[";
    for (size_t i = 0; i < act_opts.size(); ++i) {
        if (i) out << ",";
        out << act_opts[i];
    }
    out << "]";
    out << ",\"to_bp\":" << (to_bp ? "true" : "false")
            << ",\"to_ep\":" << (to_ep ? "true" : "false")
            << ",\"shuffle\":" << (can_shuffle ? "true" : "false") << "}";
    emit_line(out.str());
    std::string line = read_action_line();
    std::string kind = actjson::field(line, "action").s;
    actjson::Val iv = actjson::field(line, "index");
    int idx = static_cast<int>(iv.is_num ? iv.i : 0);
    int t = 7, s = 0; // default: end phase
    if (kind == "summon") {
        t = 0;
        s = idx;
    } else if (kind == "spsummon") {
        t = 1;
        s = idx;
    } else if (kind == "repo") {
        t = 2;
        s = idx;
    } else if (kind == "mset") {
        t = 3;
        s = idx;
    } else if (kind == "sset") {
        t = 4;
        s = idx;
    } else if (kind == "act") {
        t = 5;
        s = idx;
    } else if (kind == "to_bp") { t = 6; } else if (kind == "to_ep") { t = 7; } else if (kind == "shuffle") { t = 8; }
    set_response_i(duel_handle, t | (s << 16));
    return false;
}

// SelectBattleCmd prompt: the battle-phase menu — attack with a monster,
// activate a quick effect, or move to Main Phase 2 / End Phase.
static bool human_battlecmd(intptr_t duel_handle, Msg &msg) {
    msg.u8();
    int act = static_cast<int>(msg.u32());
    std::vector<std::string> act_opts(act);
    for (int i = 0; i < act; ++i) {
        uint32_t code = msg.u32();
        uint8_t ctrl = msg.u8();
        uint8_t loc = msg.u8();
        uint32_t seq = msg.u32();
        uint64_t desc = msg.u64();
        msg.u8();
        act_opts[i] = effect_opt_json(code, ctrl, loc, seq, desc);
    }
    int atk = static_cast<int>(msg.u32());
    std::vector<std::string> atk_opts(atk);
    for (int i = 0; i < atk; ++i) {
        uint32_t code = msg.u32();
        uint8_t ctrl = msg.u8();
        uint8_t loc = msg.u8();
        uint8_t seq = msg.u8();
        msg.u8();
        std::ostringstream ao;
        ao << "{\"code\":" << code << ",\"name\":" << json::str(card_name_ui(code))
                << ",\"p\":" << static_cast<int>(ctrl) << ",\"loc\":" << static_cast<int>(loc) << ",\"seq\":" << static_cast<int>(seq) << "}";
        atk_opts[i] = ao.str();
    }
    int to_m2 = msg.u8(), to_ep = msg.u8();
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"battle","player":)" << static_cast<int>(g_active_prompt_player) << "";
    out << ",\"act\":[";
    for (size_t i = 0; i < act_opts.size(); ++i) {
        if (i) out << ",";
        out << act_opts[i];
    }
    out << "]";
    out << ",\"attack\":[";
    for (size_t i = 0; i < atk_opts.size(); ++i) {
        if (i) out << ",";
        out << atk_opts[i];
    }
    out << "]";
    out << ",\"to_m2\":" << (to_m2 ? "true" : "false")
            << ",\"to_ep\":" << (to_ep ? "true" : "false") << "}";
    emit_line(out.str());
    std::string line = read_action_line();
    std::string kind = actjson::field(line, "action").s;
    actjson::Val iv = actjson::field(line, "index");
    int idx = static_cast<int>(iv.is_num ? iv.i : 0);
    int t = 2, s = 0;
    if (kind == "act") {
        t = 0;
        s = idx;
    } else if (kind == "attack") {
        t = 1;
        s = idx;
    } else if (kind == "to_m2") { t = 2; } else if (kind == "to_ep") { t = 3; }
    set_response_i(duel_handle, t | (s << 16));
    return false;
}

// RPS and announcements: auto-answer (no UI needed for the first version)
static bool human_auto(const intptr_t duel_handle, const uint8_t type, const Msg &msg, const std::mt19937 &rng) {
    // reuse the policy path for these rare prompts (they only need a number)
    (void) duel_handle;
    (void) type;
    (void) msg;
    (void) rng;
    return false; // caller falls back to the AI responder
}

// MSG_SELECT_UNSELECT_CARD: pick/toggle cards one at a time (used by synchro /
// xyz material selection).  Payload: player, finishable, cancelable, min, max,
// [select cards], [unselect cards].  Response: [int32 1][int32 index] to pick
// a card (0..n-1 = select list, n..n+n2-1 = unselect list), or [int32 -1] to
// finish/cancel when allowed.
static bool human_unselect_card(intptr_t duel_handle, Msg &msg) {
    msg.u8(); // player
    int finishable = msg.u8();
    int cancelable = msg.u8();
    int min = static_cast<int>(msg.u32()), max = static_cast<int>(msg.u32());
    int n = static_cast<int>(msg.u32());
    std::vector<std::string> sel(n);
    for (int i = 0; i < n; ++i) {
        uint32_t code = msg.u32();
        uint8_t ctrl = msg.u8();
        uint8_t loc = msg.u8();
        uint32_t seq = msg.u32();
        uint32_t pos = msg.u32();
        sel[i] = card_opt_json_secret(code, pos, ctrl, loc, seq, g_active_prompt_player);
    }
    int n2 = static_cast<int>(msg.u32());
    std::vector<std::string> unsel(n2);
    for (int i = 0; i < n2; ++i) {
        uint32_t code = msg.u32();
        uint8_t ctrl = msg.u8();
        uint8_t loc = msg.u8();
        uint32_t seq = msg.u32();
        uint32_t pos = msg.u32();
        unsel[i] = card_opt_json_secret(code, pos, ctrl, loc, seq, g_active_prompt_player);
    }
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"unselect","player":)" << static_cast<int>(g_active_prompt_player) << ""
            << ",\"min\":" << min << ",\"max\":" << max
            << ",\"finishable\":" << (finishable ? "true" : "false")
            << ",\"cancelable\":" << (cancelable ? "true" : "false")
            << ",\"select\":[";
    for (size_t i = 0; i < sel.size(); ++i) out << (i ? "," : "") << sel[i];
    out << "],\"unselect\":[";
    for (size_t i = 0; i < unsel.size(); ++i) out << (i ? "," : "") << unsel[i];
    out << "]}";
    emit_line(out.str());
    std::string line = read_action_line();
    actjson::Val fin = actjson::field(line, "finish");
    actjson::Val can = actjson::field(line, "cancel");
    if ((fin.is_true || can.is_true) && (finishable || cancelable)) {
        set_response_i(duel_handle, -1);
        return false;
    }
    actjson::Val iv = actjson::field(line, "index");
    int idx = static_cast<int>(iv.is_num ? iv.i : 0);
    int total = n + n2;
    if (idx < 0 || idx >= total) idx = 0;
    std::vector<uint8_t> b(8);
    int32_t one = 1;
    std::memcpy(b.data(), &one, 4);
    int32_t ii = idx;
    std::memcpy(b.data() + 4, &ii, 4);
    set_response(duel_handle, b);
    return false;
}

// MSG_ANNOUNCE_NUMBER: pick one of a set of numbers (e.g. Card Trooper's
// "send 1/2/3 from the top of the Deck").  Payload: [player u8][count u8]
// [count x u64 options]; the response is the INDEX of the chosen option.
// The SELECTMSG hint (if any) carries the question text ("Send how many
// cards to Graveyard?") and is forwarded so the UI can label the prompt.
static bool human_announce_number(const intptr_t duel_handle, Msg &msg) {
    msg.u8();                    // player
    const int n = msg.u8();
    std::vector<uint64_t> opts(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) opts[static_cast<size_t>(i)] = msg.u64();
    emit_state();
    std::ostringstream out;
    out << "{\"ev\":\"prompt\",\"type\":\"number\",\"player\":" << static_cast<int>(g_active_prompt_player)
        << ",\"count\":" << n << ",\"options\":[";
    for (int i = 0; i < n; ++i) {
        if (i) out << ",";
        out << opts[static_cast<size_t>(i)];
    }
    out << "],\"desc\":" << json::str(card_desc_string(g_last_hint_selectmsg)) << "}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const actjson::Val value = actjson::field(line, "index");
    int idx = static_cast<int>(value.is_num ? value.i : -1);
    if (idx < 0 || idx >= n) idx = n - 1;   // no answer / bad answer: max by default
    set_response_i(duel_handle, idx);
    return false;
}

// MSG_ROCK_PAPER_SCISSORS: choose 1/2/3 (rock/paper/scissors) for the
// start-of-match roll.  Payload: [player u8].  Response: 1..3; the outcome
// arrives separately as MSG_HAND_RES.
static bool human_rps(const intptr_t duel_handle, Msg &msg) {
    msg.u8();                    // player
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"rps","player":)" << static_cast<int>(g_active_prompt_player) << "}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const actjson::Val value = actjson::field(line, "value");
    int pick = static_cast<int>(value.is_num ? value.i : 1);
    if (pick < 1 || pick > 3) pick = 1;   // never leave the core retrying
    set_response_i(duel_handle, pick);
    return false;
}

// MSG_ANNOUNCE_RACE: declare `count` monster races out of the available mask.
// Payload: [player u8][count u8][available u64].  Response: the chosen u64
// bitmask (8 bytes little-endian).
static bool human_announce_race(const intptr_t duel_handle, Msg &msg) {
    msg.u8();                    // player
    const int count = msg.u8();
    const uint64_t available = msg.u64();
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"race","player":)" << static_cast<int>(g_active_prompt_player)
        << ",\"count\":" << count << ",\"options\":[";
    bool first = true;
    for (int b = 0; b < 64; ++b) {
        const uint64_t bit = 1ull << b;
        if (!(available & bit)) continue;
        if (!first) out << ",";
        first = false;
        out << bit;
    }
    out << "]}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const actjson::Val value = actjson::field(line, "mask");
    uint64_t mask = value.is_num ? static_cast<uint64_t>(value.i) : 0;
    mask &= available;
    if (__builtin_popcountll(mask) != count) {
        // bad answer: take the first `count` available races
        mask = 0;
        int left = count;
        for (int b = 0; b < 64 && left > 0; ++b) {
            const uint64_t bit = 1ull << b;
            if (available & bit) { mask |= bit; --left; }
        }
    }
    std::vector<uint8_t> b(8);
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>((mask >> (8 * i)) & 0xff);
    set_response(duel_handle, b);
    return false;
}

// MSG_ANNOUNCE_ATTRIB: same for attributes.  Payload:
// [player u8][count u8][available u32]; response is a u32 bitmask (4 bytes LE).
static bool human_announce_attrib(const intptr_t duel_handle, Msg &msg) {
    msg.u8();                    // player
    const int count = msg.u8();
    const uint32_t available = msg.u32();
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"attrib","player":)" << static_cast<int>(g_active_prompt_player)
        << ",\"count\":" << count << ",\"options\":[";
    bool first = true;
    for (int b = 0; b < 32; ++b) {
        const uint32_t bit = 1u << b;
        if (!(available & bit)) continue;
        if (!first) out << ",";
        first = false;
        out << bit;
    }
    out << "]}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const actjson::Val value = actjson::field(line, "mask");
    uint32_t mask = value.is_num ? static_cast<uint32_t>(value.i) : 0;
    mask &= available;
    if (__builtin_popcount(mask) != count) {
        mask = 0;
        int left = count;
        for (int b = 0; b < 32 && left > 0; ++b) {
            const uint32_t bit = 1u << b;
            if (available & bit) { mask |= bit; --left; }
        }
    }
    std::vector<uint8_t> b(4);
    for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>((mask >> (8 * i)) & 0xff);
    set_response(duel_handle, b);
    return false;
}

// MSG_SELECT_COUNTER: distribute `count` counters across the listed cards
// (remove/place).  Payload: [player u8][counterType u16][count u16][n u32]
// then n x (code u32, ctrl u8, loc u8, seq u8, held u16).  Response: one
// little-endian int16 per listed card (their sum must equal `count`).
static bool human_select_counter(const intptr_t duel_handle, Msg &msg) {
    msg.u8();                    // player
    const uint16_t counter_type = msg.u16();
    const int count = msg.u16();
    const uint32_t n = msg.u32();
    struct CounterCard { uint32_t code; uint8_t ctrl; uint8_t loc; uint8_t seq; uint16_t held; };
    std::vector<CounterCard> cards(n);
    for (uint32_t i = 0; i < n; ++i) {
        cards[i].code = msg.u32();
        cards[i].ctrl = msg.u8();
        cards[i].loc = msg.u8();
        cards[i].seq = msg.u8();
        cards[i].held = msg.u16();
    }
    emit_state();
    std::ostringstream out;
    out << R"({"ev":"prompt","type":"counter","player":)" << static_cast<int>(g_active_prompt_player)
        << ",\"count\":" << count << ",\"counterType\":" << counter_type << ",\"cards\":[";
    for (uint32_t i = 0; i < n; ++i) {
        if (i) out << ",";
        out << "{\"code\":" << cards[i].code
            << ",\"name\":" << json::str(card_name_ui(cards[i].code))
            << ",\"p\":" << static_cast<int>(cards[i].ctrl)
            << ",\"loc\":" << static_cast<int>(cards[i].loc)
            << ",\"locname\":" << json::str(loc_name(cards[i].loc))
            << ",\"seq\":" << static_cast<int>(cards[i].seq)
            << ",\"held\":" << cards[i].held << "}";
    }
    out << "]}";
    emit_line(out.str());
    const std::string line = read_action_line();
    std::vector<int> alloc = actjson::array_of(line, "alloc");
    int sum = 0;
    bool valid = (alloc.size() == n);
    if (valid) {
        for (uint32_t i = 0; i < n; ++i) {
            if (alloc[i] < 0 || alloc[i] > cards[i].held) { valid = false; break; }
            sum += alloc[i];
        }
        if (sum != count) valid = false;
    }
    if (!valid) {
        // greedy default: take as many as possible from the listed cards in order
        alloc.assign(n, 0);
        int left = count;
        for (uint32_t i = 0; i < n && left > 0; ++i) {
            const int take = std::min<int>(left, cards[i].held);
            alloc[i] = take;
            left -= take;
        }
    }
    std::vector<uint8_t> b(static_cast<size_t>(n) * 2);
    for (uint32_t i = 0; i < n; ++i) {
        const uint16_t v = static_cast<uint16_t>(alloc[i]);
        b[i * 2] = static_cast<uint8_t>(v & 0xff);
        b[i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
    }
    set_response(duel_handle, b);
    return false;
}

// MSG_ANNOUNCE_CARD: the player declares a card name (e.g. Mind Crush).
// Payload: [player u8][count u8][count x u64 select-options]. The response is
// the declared card's passcode (int32). The options may be raw card codes or
// opcodes (e.g. OPCODE_ISTYPE + a type) — the core validates the answer, so we
// just relay the chosen code.
static bool human_announce_card(const intptr_t duel_handle, Msg &msg) {
    msg.u8();                    // player
    const int n = msg.u8();
    std::vector<uint64_t> opts(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) opts[static_cast<size_t>(i)] = msg.u64();
    emit_state();
    std::ostringstream out;
    out << "{\"ev\":\"prompt\",\"type\":\"announce_card\",\"player\":" << static_cast<int>(g_active_prompt_player)
      << ",\"count\":" << n << ",\"options\":[";
    for (int i = 0; i < n; ++i) {
        if (i) out << ",";
        out << opts[static_cast<size_t>(i)];
    }
    out << "]}";
    emit_line(out.str());
    const std::string line = read_action_line();
    const actjson::Val value = actjson::field(line, "code");
    set_response_i(duel_handle, static_cast<int32_t>(value.is_num ? value.i : 0));
    return false;
}

// entry point: return true when the human path handled this SELECT (and set a
// response); false means "unknown type" so the caller falls back to the AI
// responder instead of spinning on MSG_RETRY.
static bool human_handle_select(const intptr_t duel_handle, const uint8_t type, const uint8_t *pbuf,
                                const int len, std::mt19937 &rng) {
    Msg msg(pbuf);
    msg.u8();
    // the first payload byte of every SELECT is the acting player
    g_active_prompt_player = (len > 1) ? pbuf[1] : 0;
    switch (type) {
        case MSG_SELECT_YESNO: human_yesno(duel_handle, msg, "yesno");
            return true;
        case MSG_SELECT_EFFECTYN: human_effectyn(duel_handle, msg);
            return true;
        case MSG_SELECT_OPTION: human_option(duel_handle, msg);
            return true;
        case MSG_SELECT_CARD: human_select_card(duel_handle, msg, false);
            return true;
        case MSG_SELECT_TRIBUTE: human_select_card(duel_handle, msg, true);
            return true;
        case MSG_SELECT_CHAIN: human_chain(duel_handle, msg);
            return true;
        case MSG_SELECT_POSITION: human_position(duel_handle, msg);
            return true;
        case MSG_SELECT_PLACE:
        case MSG_SELECT_DISFIELD: human_place(duel_handle, msg);
            return true;
        case MSG_SELECT_IDLECMD: human_idlecmd(duel_handle, msg);
            return true;
        case MSG_SELECT_BATTLECMD: human_battlecmd(duel_handle, msg);
            return true;
        case MSG_SELECT_UNSELECT_CARD: human_unselect_card(duel_handle, msg);
            return true;
        case MSG_SELECT_SUM: human_sum(duel_handle, msg);
            return true;
        case MSG_ANNOUNCE_CARD: human_announce_card(duel_handle, msg);
            return true;
        case MSG_ANNOUNCE_NUMBER: human_announce_number(duel_handle, msg);
            return true;
        case MSG_ANNOUNCE_RACE: human_announce_race(duel_handle, msg);
            return true;
        case MSG_ANNOUNCE_ATTRIB: human_announce_attrib(duel_handle, msg);
            return true;
        case MSG_SELECT_COUNTER: human_select_counter(duel_handle, msg);
            return true;
        case MSG_ROCK_PAPER_SCISSORS: human_rps(duel_handle, msg);
            return true;
        default: return false; // not handled -> AI responder handles it
    }
}

// ---------------------------------------------------------------------------
// scenario initial-state injection: load the field model from a JSON state
// file produced by scenario.py (the runner's own field model mirrors it; the
// core's actual board is set via the Debug.* Lua script loaded separately).
// ---------------------------------------------------------------------------
static bool load_scenario_state(const std::string &path) {
    std::string text;
    {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            text.append(buf, n);
        std::fclose(f);
    }
    g_field.clear();
    // minimal JSON scan: find "mz0":[...], "sz0":[...], etc. arrays of objects
    // {code, pos, atk, def, faceup, p, loc, seq}
    auto find_array = [&](const char *key) -> std::vector<std::string> {
        std::vector<std::string> objs;
        const std::string needle = std::string("\"") + key + "\":";
        const size_t k = text.find(needle);
        if (k == std::string::npos) return objs;
        const size_t open = text.find('[', k);
        if (open == std::string::npos) return objs;
        size_t i = open + 1;
        while (i < text.size()) {
            while (i < text.size() && (isspace(static_cast<unsigned char>(text[i])) || text[i] == ',')) ++i;
            if (i >= text.size() || text[i] == ']') break;
            const size_t start = i;
            int depth = 0;
            while (i < text.size()) {
                if (text[i] == '{') depth++;
                else if (text[i] == '}') {
                    depth--;
                    if (depth == 0) {
                        ++i;
                        break;
                    }
                } else if (text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') {
                        if (text[i] == '\\') ++i;
                        ++i;
                    }
                }
                i++;
            }
            objs.push_back(text.substr(start, i - start));
        }
        return objs;
    };
    auto int_field = [](const std::string &o, const char *key) -> long long {
        const std::string needle = std::string("\"") + key + "\":";
        size_t k = o.find(needle);
        if (k == std::string::npos) return 0;
        k += needle.size();
        while (k < o.size() && isspace(static_cast<unsigned char>(o[k]))) ++k;
        long long v = 0;
        bool neg = false;
        if (k < o.size() && o[k] == '-') {
            neg = true;
            ++k;
        }
        while (k < o.size() && isdigit(static_cast<unsigned char>(o[k]))) {
            v = v * 10 + (o[k] - '0');
            ++k;
        }
        return neg ? -v : v;
    };
    auto bool_field = [](const std::string &o, const char *key, const bool dflt) {
        const std::string needle = std::string("\"") + key + "\":";
        size_t k = o.find(needle);
        if (k == std::string::npos) return dflt;
        k += needle.size();
        while (k < o.size() && isspace(static_cast<unsigned char>(o[k]))) ++k;
        if (k < o.size() && (o[k] == 't' || o[k] == 'T')) return true;
        if (k < o.size() && (o[k] == 'f' || o[k] == 'F')) return false;
        if (k < o.size() && o[k] == '1') return true;
        return dflt;
    };
    auto load_zone = [&](const char *key, const uint8_t p) {
        for (auto &o: find_array(key)) {
            FCard c;
            c.code = static_cast<uint32_t>(int_field(o, "code"));
            c.pos = static_cast<uint32_t>(int_field(o, "pos"));
            c.atk = static_cast<int32_t>(int_field(o, "atk"));
            c.def = static_cast<int32_t>(int_field(o, "def"));
            c.ctrl = p;
            c.loc = static_cast<uint8_t>(int_field(o, "loc"));
            c.seq = static_cast<uint32_t>(int_field(o, "seq"));
            c.faceup = (c.pos & 0x1) != 0;
            c.known = bool_field(o, "known", c.faceup);
            if (c.code) {
                g_field.zone[p][{c.loc, c.seq}] = c;
                if (c.loc == 0x04 && c.seq < 8) g_last_mzone_code[p][c.seq] = c.code;
            }
        }
    };
    load_zone("mz0", 0);
    load_zone("mz1", 1);
    load_zone("sz0", 0);
    load_zone("sz1", 1);
    // hand0 / hand1_count / g0 / g1 / lp
    for (auto &o: find_array("hand0")) {
        const long long code = int_field(o, "code");
        if (code) g_field.hand[0].push_back(static_cast<uint32_t>(code));
    }
    for (auto &o: find_array("g0")) {
        const long long code = int_field(o, "code");
        if (code) g_field.grave[0].push_back(static_cast<uint32_t>(code));
    }
    for (auto &o: find_array("g1")) {
        const long long code = int_field(o, "code");
        if (code) g_field.grave[1].push_back(static_cast<uint32_t>(code));
    }
    // hand arrays are bare numbers in our state JSON, not objects; handle
    // that with a simpler scan
    auto codes_array = [&](const char *key) {
        std::vector<uint32_t> out;
        const std::string needle = std::string("\"") + key + "\":[";
        size_t k = text.find(needle);
        if (k == std::string::npos) return out;
        k += needle.size();
        while (k < text.size() && text[k] != ']') {
            while (k < text.size() && (isspace(static_cast<unsigned char>(text[k])) || text[k] == ',')) ++k;
            if (k >= text.size() || text[k] == ']') break;
            long long v = 0;
            while (k < text.size() && isdigit(static_cast<unsigned char>(text[k]))) {
                v = v * 10 + (text[k] - '0');
                ++k;
            }
            if (v) out.push_back(static_cast<uint32_t>(v));
        }
        return out;
    };
    g_field.hand[0] = codes_array("hand0");
    g_field.grave[0] = codes_array("g0");
    g_field.grave[1] = codes_array("g1");
    auto lp_field = [&](const char *key) {
        const std::string needle = std::string("\"") + key + "\":[";
        size_t k = text.find(needle);
        if (k == std::string::npos) return 8000;
        k += needle.size();
        long long v = 0;
        while (k < text.size() && isdigit(static_cast<unsigned char>(text[k]))) {
            v = v * 10 + (text[k] - '0');
            ++k;
        }
        return static_cast<int>(v);
    };
    g_field.lp[0] = lp_field("lp");
    g_field.lp[1] = 8000; // second LP value parsed below
    // parse lp array second element
    {
        const std::string needle = "\"lp\":[";
        size_t k = text.find(needle);
        if (k != std::string::npos) {
            k += needle.size();
            while (k < text.size() && text[k] != ',') ++k;
            ++k;
            long long v = 0;
            while (k < text.size() && isdigit(static_cast<unsigned char>(text[k]))) {
                v = v * 10 + (text[k] - '0');
                ++k;
            }
            if (v) g_field.lp[1] = static_cast<int>(v);
        }
    }
    g_field.hand_count[0] = static_cast<int>(g_field.hand[0].size());
    g_field.hand_count[1] = static_cast<int>(g_field.hand[1].size());
    return true;
}
