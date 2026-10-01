// duel_runner_edo.cpp — headless duel runner on the EDOPro ocgcore fork
// (edo9300/ygopro-core) with the Edison-rulings duel flags enabled.
//
// Same role as duel_runner.cpp but against the modern OCG API
// (OCG_CreateDuel/OCG_DuelProcess/OCG_DuelSetResponse) and its updated
// message formats (u32 counts, 10-uint8_t loc_info, u64 descriptions, the
// parse_response_cards selection protocol, ...).
//
// Links the ocgcore static library, drives random-vs-random duels between two
// .ydk decks, and prints one JSON result per game. Phase 1 goal: prove the
// engine duels end-to-end headlessly (summons, battle, LP, effects where
// scripts exist). The AI is a random policy over legal actions; later phases
// swap it for per-deck policies and expose step-wise control.
//
// Build:
//   g++ -std=c++14 -O2 -I ygopro-core -I /usr/include/lua5.4 \
//       duel_runner.cpp ygopro-core/build_obj/libocgcore.a \
//       -lsqlite3 -llua5.4 -o duel_runner
// Run (cwd must contain ./script/):
//   ./duel_runner --deck1 A.ydk --deck2 B.ydk --seed 1 --games 10
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <csignal>
#include <ctime>
#include <unistd.h>
#include <sys/wait.h>
#include "ocgapi.h"
#include "ocgapi_types.h"
#include "ocgapi_constants.h"
#include "policy.h"

// ---------------------------------------------------------------------------
// card reader: cards.cdb (YGOPro format)
// ---------------------------------------------------------------------------
static sqlite3* g_db = nullptr;
static bool g_verbose = false;      // declared early: used by card reader

static uint16_t g_setcodes[16];
static void card_reader_cb(void* payload, const uint32_t code, OCG_CardData* data) {
    static sqlite3_stmt* st = nullptr;
    if (!st) {
        sqlite3_prepare_v2(g_db,
            "SELECT alias, setcode, type, atk, def, level, race, attribute "
            "FROM datas WHERE id = ?", -1, &st, nullptr);
    }
    sqlite3_reset(st);
    sqlite3_bind_int(st, 1, static_cast<int>(code));
    if (sqlite3_step(st) != SQLITE_ROW) {
        std::memset(data, 0, sizeof *data);
        data->setcodes = g_setcodes;
        return;
    }
    std::memset(data, 0, sizeof *data);
    data->code = code;
    data->alias = static_cast<uint32_t>(sqlite3_column_int(st, 0));
    uint64_t setcode = (uint64_t)sqlite3_column_int64(st, 1);
    // zero the shared buffer first: a card with no archetype (setcode 0) must
    // not inherit the previous card's setcodes
    std::memset(g_setcodes, 0, sizeof g_setcodes);
    int n = 0;
    while (setcode && n < 16) { g_setcodes[n++] = static_cast<uint16_t>(setcode & 0xffff); setcode >>= 16; }
    data->setcodes = g_setcodes;
    data->type = static_cast<uint32_t>(sqlite3_column_int(st, 2));
    data->attack = sqlite3_column_int(st, 3);
    data->defense = sqlite3_column_int(st, 4);
    data->level = static_cast<uint32_t>(sqlite3_column_int(st, 5));
    data->race = static_cast<uint64_t>(sqlite3_column_int(st, 6));
    data->attribute = static_cast<uint32_t>(sqlite3_column_int(st, 7));
}

// ---------------------------------------------------------------------------
// script reader: ./script/c<code>.lua (+ constant/utility/procedure)
// ---------------------------------------------------------------------------
static std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool g_bootstrapped = false;
static void load_bootstrap(OCG_Duel duel, const char* name);
static int script_reader_cb(void* payload, const OCG_Duel duel, const char* name) {
    // First request from the card-script loading context: load the bootstrap
    // files here so their globals are visible to card scripts (loading them
    // via OCG_LoadScript from the main thread does NOT propagate to the
    // coroutine context card scripts run in).
    if (!g_bootstrapped) {
        g_bootstrapped = true;   // set before nested loads to avoid recursion
        load_bootstrap(duel, "constant.lua");
        load_bootstrap(duel, "utility.lua");  // chain-loads proc_*.lua etc.
    }
    // card scripts are requested as "c<code>.lua" (no path) by this core
    std::string path = name;
    if (path.find('/') == std::string::npos)
        path = "./script/" + path;
    std::string s = read_file(path.c_str());
    if (s.empty())
        return 0;
    if (g_verbose)
        std::fprintf(stderr, "[script] %s (%zu bytes)\n", name, s.size());
    bool ok = OCG_LoadScript(duel, s.data(), static_cast<uint32_t>(s.size()), name);
    if (g_verbose)
        std::fprintf(stderr, "[script] %s -> %s\n", name, ok ? "ok" : "FAIL");
    return ok;
}

// load the era bootstrap scripts (aux etc.) the core does not provide
static void load_bootstrap(const OCG_Duel duel, const char* name) {
    std::string path = std::string("./script/") + name;
    std::string s = read_file(path.c_str());
    if (!s.empty()) {
        bool ok = OCG_LoadScript(duel, s.data(), static_cast<uint32_t>(s.size()), name);
        if (g_verbose)
            std::fprintf(stderr, "[bootstrap] %s -> %s\n", name, ok ? "ok" : "FAILED");
    } else {
        if (g_verbose)
            std::fprintf(stderr, "[bootstrap] %s -> missing file\n", name);
    }
}

static void log_handler_cb(void* payload, const char* str, const int type) {
    std::fprintf(stderr, "[lua t=%d] %s\n", type, str ? str : "(null)");
}

// ---------------------------------------------------------------------------
// Edison (TCG, March 2010) duel flags on the EDOPro core.
// MR1 base + TCG ignition priority + the retro-rules the Edison community
// cites (0-ATK battles, single chain per damage substep, TCG SEGOC trigger
// ordering, attack replays, equip-not-sent, repos, first-turn draw).
// Goat-only bits (6-step battle step, traps-in-new-chain, private-knowledge
// triggers) are intentionally excluded pending edisonrul.ing verification.
// ---------------------------------------------------------------------------
static const uint64_t EDISON_FLAGS =
    0x100ull                 // DUEL_OCG_OBSOLETE_IGNITION (in MR1 preset)
    | 0x200ull               // DUEL_1ST_TURN_DRAW
    | 0x400ull               // DUEL_1_FACEUP_FIELD
    | 0x10000ull             // DUEL_RETURN_TO_DECK_TRIGGERS
    | 0x40000ull             // DUEL_SPSUMMON_ONCE_OLD_NEGATE
    | 0x80000ull             // DUEL_CANNOT_SUMMON_OATH_OLD
    | 0x8000000ull           // DUEL_EQUIP_NOT_SENT_IF_MISSING_TARGET
    | 0x10000000ull          // DUEL_0_ATK_DESTROYED
    | 0x20000000ull          // DUEL_STORE_ATTACK_REPLAYS
    | 0x40000000ull          // DUEL_SINGLE_CHAIN_IN_DAMAGE_SUBSTEP
    | 0x80000000ull          // DUEL_CAN_REPOS_IF_NON_SUMPLAYER
    | 0x100000000ull         // DUEL_TCG_SEGOC_NONPUBLIC
    | 0x200000000ull         // DUEL_TCG_SEGOC_FIRSTTRIGGER
    | 0x400000000ull;        // DUEL_TCG_FAST_EFFECT_IGNITION (Edison priority)

namespace {
    // ---------------------------------------------------------------------------
    // message parsing
    // ---------------------------------------------------------------------------
    struct Msg {
        const uint8_t* p;
        int pos = 0;
        explicit Msg(const uint8_t* b) : p(b) {}
        uint8_t u8() { return p[pos++]; }
        uint16_t u16() {
            const auto v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8));
            pos += 2;
            return v;
        }
        uint32_t u32() {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
                v |= static_cast<uint32_t>(p[pos + i]) << (8 * i);
            pos += 4;
            return v;
        }
        uint64_t u64() {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v |= static_cast<uint64_t>(p[pos + i]) << (8 * i);
            pos += 8;
            return v;
        }
        void skip(const int n) { pos += n; }
    };
}

namespace {
    // deck parsing
    struct Deck { std::vector<uint32_t> main, extra; };
}
static bool parse_ydk(const std::string& path, Deck& deck) {
    std::ifstream f(path);
    if (!f)
        return false;
    std::string line;
    std::string section = "main";
    while (std::getline(f, line)) {
        std::string t = line;
        t.erase(t.find_last_not_of(" \t\r\n") + 1);
        if (t.empty() || t[0] == '#')
            continue;
        if (t == "!main" || t == "#main") { section = "main"; continue; }
        if (t == "!extra" || t == "#extra") { section = "extra"; continue; }
        if (t == "!side" || t == "#side") { section = "side"; continue; }
        if (t.find_first_not_of("0123456789") != std::string::npos)
            continue;
        auto code = static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 10));
        if (section == "main") deck.main.push_back(code);
        else if (section == "extra") deck.extra.push_back(code);
    }
    return !deck.main.empty();
}

// ---------------------------------------------------------------------------
// random responder state
// ---------------------------------------------------------------------------
static std::vector<uint8_t> g_last_prompt;   // full last SELECT message incl. type
static int g_retries = 0;                 // consecutive MSG_RETRY count
static int g_winner = -1;                 // 0, 1, or 2 (draw)
static int g_win_reason = 0;               // 1 = LP, 2 = deck-out (for ML rewards)
static int g_final_lp[2] = {0, 0};   // final life points (for cap rewards)
static int g_final_ca[2] = {0, 0};   // final hand+mz+sz card counts
static int g_turns = 0;
static int g_steps = 0;
static bool g_trace = false;

// harness fast-forward: scenario Lua may carry "-- harness: turn_player=.. turn=..
// phase=..".  Until the duel reaches that exact (global turn id, whose turn,
// phase) the runner plays both seats neutrally (pass / end turns, skip
// battles) with no model and no decision recording, then hands control over.
static int g_turn_player = -1;
static bool g_h_ff = false;
static int g_h_turn = 0;
static int g_h_player = -1;
static uint32_t g_h_phase = 0;
static void set_response_i(const intptr_t pduel, const int32_t v);  // defined below

// forced-move script ("-- hmove:" lines): fire once per matching prompt,
// ahead of the auto-responder / policies, so a board can drive an exact line
// (wildcards: player/turn/phase == -1/0/0)
struct HarMove {
    int player = -1;      // -1 = any
    int turn = 0;         // 0 = any (global turn id)
    uint32_t phase = 0;   // 0 = any
    std::string prompt;   // idle|battle|chain|position|*
    std::string act;      // summon|sp|repos|mset|sset|bp|ep|attack|m2|pass|first
    bool done = false;
};
static std::vector<HarMove> g_moves;

// per-player AI policies + shared field model (updated from the message stream)
// policy files live in ./policies/<deck>.json (auto-loaded when present)
static Policy load_policy_for(const std::string& deck) {
    Policy p = make_policy(deck);
    if (deck.empty())
        return p;
    std::string path = "policies/" + deck + ".json";
    std::string s = read_file(path.c_str());
    if (!s.empty()) {
        Policy over;
        if (policy_from_json(s, over)) {
            p = over;   // full replace: the tuning UI edits the whole table
            std::fprintf(stderr, "[policy] %s: loaded %s\n", deck.c_str(), path.c_str());
        } else {
            std::fprintf(stderr, "[policy] %s: ignoring malformed %s\n", deck.c_str(), path.c_str());
        }
    }
    return p;
}

static Policy g_policy[2];
static FieldState g_field;
// current duel handle (OCG_Duel) for core state queries in interactive.h.
// The UI board snapshot is produced by querying the core's "state of the
// world" directly (OCG_DuelQueryLocation) rather than replaying message
// deltas, so it can never drift out of sync with the actual game.
intptr_t g_duel = 0;

// static card stats from cards.cdb (no OCG context needed)
static bool card_stats(const uint32_t code, int32_t& atk, int32_t& def) {
    static sqlite3_stmt* st = nullptr;
    if (!st)
        sqlite3_prepare_v2(g_db, "SELECT atk, def FROM datas WHERE id = ?", -1, &st, nullptr);
    sqlite3_reset(st);
    sqlite3_bind_int(st, 1, static_cast<int>(code));
    if (sqlite3_step(st) == SQLITE_ROW) {
        atk = sqlite3_column_int(st, 0);
        def = sqlite3_column_int(st, 1);
        return true;
    }
    return false;
}

// card type bits from cards.cdb (scalable metadata: no hand-maintained lists).
// TYPE_* macros come from ocgapi_constants.h (TYPE_MONSTER 0x1, TYPE_SPELL
// 0x2, TYPE_TRAP 0x4, TYPE_QUICKPLAY 0x10000, ...).
static bool card_type_bits(const uint32_t code, uint32_t& type) {
    static sqlite3_stmt* st = nullptr;
    if (!st)
        sqlite3_prepare_v2(g_db, "SELECT type FROM datas WHERE id = ?", -1, &st, nullptr);
    sqlite3_reset(st);
    sqlite3_bind_int(st, 1, static_cast<int>(code));
    if (sqlite3_step(st) == SQLITE_ROW) {
        type = static_cast<uint32_t>(sqlite3_column_int(st, 0));
        return true;
    }
    return false;
}

// a KNOWN facedown S/T the AI should not walk into when attacking.  Rather
// than maintaining a per-card list, we use card-type metadata from cards.cdb:
// any facedown Trap or Quick-Play Spell can punish an attack declaration
// (Mirror Force, Dimensional Prison, Enemy Controller, ...).  Scalable: it
// covers the whole card pool without a hand-maintained whitelist.
static bool is_attack_reaction_s_t(const uint32_t code) {
    // Focused list of the backrow that actually punishes an attack, rather
    // than "any trap/quick-play".  Otherwise the AI refuses to attack because
    // of a random set Bottomless / Call of the Haunted, which is far too timid.
    switch (code) {
    case 44095762: return true;  // Mirror Force
    case 70342110: return true;  // Dimensional Prison
    case 56120475: return true;  // Sakuretsu Armor
    case 77754944: return true;  // Widespread Ruin
    case 62279055: return true;  // Magic Cylinder
    case 14315573: return true;  // Negate Attack
    case 22359980: return true;  // Mirror Wall
    case 36361633: return true;  // Threatening Roar
    default: return false;
    }
}

// does the opponent control a KNOWN (perfect-memory) facedown S/T that could
// punish an attack?  Used by the greedy policy so that rollouts and the AI
// play around threats they can see/remember.
static bool opponent_has_known_attack_trap(const uint8_t opp) {
    for (auto& kv : g_field.zone[opp]) {
        const FCard& c = kv.second;
        if (c.loc == 0x08 /* LOCATION_SZONE */ && !c.faceup && c.known
            && is_attack_reaction_s_t(c.code))
            return true;
    }
    return false;
}

static uint64_t g_last_hint_selectmsg = 0;   // value from last HINT_SELECTMSG
static uint32_t g_attacker_code = 0;          // attacker we declared in BATTLECMD
static uint32_t g_attacker_atk = 0;
static bool g_attack_pending = false;
static std::vector<uint32_t> g_opening[2];   // opening-hand card codes per player

// ML decision recorder: globals are declared before search.h so rollout
// children can silence them (no recursive label-search / recorder writes).
static FILE* g_ml_out = nullptr;          // decision log file (--record-decisions)
static int g_ml_labels = 0;               // label rollouts per search (--search-labels)
// neural-policy server (--neural-policy): a persistent python predictor that
// answers card/tribute/chain prompts. Rollout children silence it (below).
static FILE* g_nn_in[2] = {nullptr, nullptr};     // per-seat request pipes
static FILE* g_nn_out[2] = {nullptr, nullptr};    // per-seat response pipes
static int g_nn_pid[2] = {-1, -1};   // (pid_t; int is fine on linux)
static int g_nn_seat[2] = {-1, -1};              // seat served by slot (0/1)
static long g_nn_id = 0;
static std::string g_nn_python = "python3";              // --neural-python
static std::string g_nn_script = "ml/predict_server.py"; // --neural-script
static std::string g_rollout_model;                      // --rollout-policy
static std::string g_value_model;                        // --value-model
static std::string g_nn_dump_path;                       // --nn-dump (debug)
static double g_nn_mutate = 0.0;              // --nn-mutate (uniform-random action)
static bool g_cpp_rollout[2] = {false, false};   // per-seat in-process champion
static int g_cpp_seat = -1;                      // --cpp-seat: seat whose DECISIONS
                                                 // come from the .bin champion head
static void nn_spawn(int seat, const std::string& py, const std::string& scr,
                     const std::string& model, double temp, double mutate);
#include "interactive.h"
#include "nn_cpp.h"
#include "search.h"

// ---------------------------------------------------------------------------
// ML decision recorder (Step-2/3).  With --record-decisions <file>, every
// ACCEPTED policy decision is appended as one JSON line:
//   {"ev":"decision","game":..,"turn":..,"step":..,"phase":..,
//    "phase_name":..,"player":0|1,"prompt_type":N,"prompt_name":"...",
//    "prompt_hex":"..","resp_hex":"..","resp_int":N?,"resp_indices":[..]?,
//    "label":{...}|null, "obs":{...}}
// `label` is set when --search-labels N is active: for the decisions the
// search supports (single-card picks, chain responses) the engine also runs
// rollout search and records its chosen option + per-option win rates, so
// training can supervise against a stronger teacher than the greedy rules.
// The POLICY still plays the game; search only supplies the label.
// Records are deferred until the core accepts the response: a MSG_RETRY
// re-answer replaces the pending response, and the line is flushed when the
// next message is not a RETRY (or at close).  Batch mode only.
static int g_ml_game = 0;                 // game index for the record
static std::vector<uint8_t> g_ml_resp;    // last response bytes set by the policy

struct MlPend {
    bool active = false;
    uint8_t player = 0;
    uint8_t type = 0;
    int turn = 0;
    int step = 0;
    uint32_t phase = 0;
    std::vector<uint8_t> prompt;   // full prompt message bytes (from type byte)
    std::string obs;               // viewer snapshot captured at first attempt
    std::vector<uint8_t> resp;
    std::string label;             // "label":{...} fragment ("" = none)
};
static MlPend g_ml_pend;

static void ml_capture_resp(const void* data, uint32_t len) {
    if (!g_ml_out) return;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    g_ml_resp.assign(b, b + len);
}

static const char* ml_msg_name(uint8_t t) {
    switch (t) {
    case MSG_SELECT_BATTLECMD: return "battlecmd";
    case MSG_SELECT_IDLECMD: return "idlecmd";
    case MSG_SELECT_EFFECTYN: return "effectyn";
    case MSG_SELECT_YESNO: return "yesno";
    case MSG_SELECT_OPTION: return "option";
    case MSG_SELECT_CARD: return "card";
    case MSG_SELECT_CHAIN: return "chain";
    case MSG_SELECT_PLACE: return "place";
    case MSG_SELECT_POSITION: return "position";
    case MSG_SELECT_TRIBUTE: return "tribute";
    case MSG_SELECT_COUNTER: return "counter";
    case MSG_SELECT_SUM: return "sum";
    case MSG_SELECT_DISFIELD: return "disfield";
    case MSG_SORT_CARD: return "sort";
    case MSG_SELECT_UNSELECT_CARD: return "unselect";
    case MSG_ROCK_PAPER_SCISSORS: return "rps";
    case MSG_ANNOUNCE_RACE: return "announce_race";
    case MSG_ANNOUNCE_ATTRIB: return "announce_attrib";
    case MSG_ANNOUNCE_CARD: return "announce_card";
    case MSG_ANNOUNCE_NUMBER: return "announce_number";
    default: return "other";
    }
}

// begin a fresh decision record (first presentation of a prompt): capture the
// deciding player's own filtered board view now (pre-decision state).
static void ml_pend_begin(const uint8_t* prompt, size_t len, uint8_t player) {
    if (!g_ml_out) return;
    g_ml_pend = MlPend();
    g_ml_pend.active = true;
    g_ml_pend.player = player;
    g_ml_pend.type = *prompt;
    g_ml_pend.turn = g_turns;
    g_ml_pend.step = g_steps;
    g_ml_pend.phase = g_current_phase;
    g_ml_pend.prompt.assign(prompt, prompt + len);
    g_ml_pend.obs = serialize_state_json(player);   // own-view observation
    g_ml_pend.resp = g_ml_resp;
}

// refresh the response bytes after the policy answered (also on RETRY re-answer)
static void ml_pend_resp() {
    if (!g_ml_out || !g_ml_pend.active) return;
    g_ml_pend.resp = g_ml_resp;
}

// Store a rollout-search label for the pending decision. `chain` selects the
// pass-aware semantics used by chain prompts (action "-1" = pass). The search
// result is converted to OPTION-INDEX terms matching the supervised dataset:
//   card/tribute: actions are "0".."n-1" -> chosen option index
//   chain:        actions are "-1"(pass),"0".. -> -1 = pass, else index
static void ml_pend_label(const SearchResult& res, bool chain) {
    if (!g_ml_out || !g_ml_pend.active || res.chosen < 0)
        return;
    int chosen_opt = -1;
    if (!chain) {
        chosen_opt = atoi(res.actions[res.chosen].c_str());
    } else {
        chosen_opt = (res.chosen == 0) ? -1 : res.chosen - 1;
    }
    std::ostringstream o;
    o << "{\"chosen\":" << chosen_opt << ",\"rollouts\":" << res.rollouts
      << ",\"scores\":[";
    bool first = true;
    for (auto& sc : res.scores) {
        if (!first) o << ",";
        int opt = !chain ? atoi(res.actions[sc.first].c_str())
                         : (sc.first == 0 ? -1 : sc.first - 1);
        o << "[" << opt << "," << static_cast<int>(sc.second) << "]";
        first = false;
    }
    o << "]}";
    g_ml_pend.label = o.str();
}

// Record a label whose "chosen" is already a flat option index (used by
// idlecmd/battlecmd, whose search candidates are semantic actions that need a
// custom map back to the supervised dataset's flat option ordering).
static void ml_pend_label_idx(int chosen_opt, int rollouts) {
    if (!g_ml_out || !g_ml_pend.active || chosen_opt < 0)
        return;
    std::ostringstream o;
    o << "{\"chosen\":" << chosen_opt << ",\"rollouts\":" << rollouts << "}";
    g_ml_pend.label = o.str();
}

static void ml_flush() {
    if (!g_ml_out || !g_ml_pend.active) return;
    const std::vector<uint8_t>& r = g_ml_pend.resp;
    std::ostringstream o;
    int64_t rint = 0;
    bool is_int = false;
    std::vector<int> ridx;
    if (r.size() == 4) {
        is_int = true;
        std::memcpy(&rint, r.data(), 4);
    } else if (r.size() >= 8) {
        // response protocol used by the runner's card selections:
        // [int32 type=2][uint32 count][u8 indices...]
        int32_t t = 0; std::memcpy(&t, r.data(), 4);
        uint32_t c = 0; std::memcpy(&c, r.data() + 4, 4);
        if (t == 2 && 8 + c <= r.size())
            for (uint32_t i = 0; i < c; ++i) ridx.push_back(r[8 + i]);
    }
    char hex[8];
    o << "{\"ev\":\"decision\",\"game\":" << g_ml_game
      << ",\"turn\":" << g_ml_pend.turn << ",\"step\":" << g_ml_pend.step
      << ",\"phase\":" << g_ml_pend.phase
      << ",\"phase_name\":" << json::str(phase_name(g_ml_pend.phase))
      << ",\"player\":" << static_cast<int>(g_ml_pend.player)
      << ",\"prompt_type\":" << static_cast<int>(g_ml_pend.type)
      << ",\"prompt_name\":" << json::str(ml_msg_name(g_ml_pend.type))
      << ",\"prompt_hex\":\"";
    for (auto b : g_ml_pend.prompt) { std::snprintf(hex, sizeof hex, "%02x", b); o << hex; }
    o << "\",\"resp_hex\":\"";
    for (auto b : r) { std::snprintf(hex, sizeof hex, "%02x", b); o << hex; }
    o << "\"";
    if (is_int) o << ",\"resp_int\":" << rint;
    if (!ridx.empty()) {
        o << ",\"resp_indices\":[";
        for (size_t i = 0; i < ridx.size(); ++i) o << (i ? "," : "") << ridx[i];
        o << "]";
    }
    if (!g_ml_pend.label.empty())
        o << ",\"label\":" << g_ml_pend.label;
    o << ",\"obs\":" << g_ml_pend.obs << "}\n";
    std::fputs(o.str().c_str(), g_ml_out);
    std::fflush(g_ml_out);
    g_ml_pend.active = false;
}

static void ml_close() {
    if (!g_ml_out) return;
    ml_flush();   // last decision of the last game, if still pending
    std::fclose(g_ml_out);
    g_ml_out = nullptr;
    g_ml_pend = MlPend();
}

// ---------------------------------------------------------------------------
// neural-policy helpers: ask the python predictor which option to pick for a
// card/tribute/chain prompt. Returns true and sets `out_choice` (option index,
// or -1 for chain pass). On any failure the caller falls back to its policy.

static void capture_end_state(OCG_Duel duel) {
    if (!duel) return;
    for (int p = 0; p < 2; ++p) {
        g_final_lp[p] = std::max(0, g_field.lp[p]);
        g_final_ca[p] = static_cast<int>(
            query_location(duel, static_cast<uint8_t>(p), LOCATION_HAND).size()
            + query_location(duel, static_cast<uint8_t>(p), LOCATION_MZONE).size()
            + query_location(duel, static_cast<uint8_t>(p), LOCATION_SZONE).size());
    }
}

static bool nn_active(int seat) {
    return seat >= 0 && seat <= 1 && g_nn_in[seat] != nullptr
        && g_nn_out[seat] != nullptr && g_nn_seat[seat] == seat;
}

static bool nn_ready(int seat) {
    return nn_active(seat) || (seat >= 0 && seat <= 1 && g_cpp_rollout[seat]);
}

static bool nn_query(uint8_t type, uint8_t player, const uint8_t* pbuf,
                     int len, int& out_choice) {
    if (g_cpp_rollout[player] && nncpp::g_cpp.loaded)
        return nncpp::g_cpp.pick(type, player, pbuf, len, out_choice);
    if (!nn_active(player)) return false;
    FILE* in = g_nn_in[player];
    FILE* out = g_nn_out[player];
    ++g_nn_id;
    std::ostringstream o;
    o << "{\"id\":" << g_nn_id
      << ",\"prompt_name\":\"" << ml_msg_name(type)
      << "\",\"player\":" << static_cast<int>(player)
      << ",\"hex\":\"";
    char hx[8];
    for (int i = 0; i < len; ++i) { std::snprintf(hx, sizeof hx, "%02x", pbuf[i]); o << hx; }
    o << "\",\"obs\":" << serialize_state_json(player) << "}\n";
    std::fputs(o.str().c_str(), in);
    std::fflush(in);
    char buf[8192];
    if (!std::fgets(buf, sizeof buf, out))
        return false;               // predictor died -> policy fallback
    const char* q = std::strstr(buf, "\"choice\":");
    if (!q) return false;
    out_choice = std::atoi(q + 9);
    if (g_verbose)
        std::fprintf(stderr, "[nn] seat=%d type=%d id=%ld choice=%d\n",
                     static_cast<int>(player), static_cast<int>(type),
                     g_nn_id, out_choice);
    return true;
}

// spawn one predictor server for a seat
static void nn_spawn(int seat, const std::string& py, const std::string& scr,
                     const std::string& model, double temp, double mutate) {
    int a2c[2] = {-1, -1}, c2a[2] = {-1, -1};
    if (pipe(a2c) != 0 || pipe(c2a) != 0) {
        std::fprintf(stderr, "[nn] pipe failed for seat %d\n", seat);
        close(a2c[0]); close(a2c[1]); close(c2a[0]); close(c2a[1]);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(a2c[0], 0); dup2(c2a[1], 1);
        close(a2c[0]); close(a2c[1]); close(c2a[0]); close(c2a[1]);
        std::string t = temp > 0.0 ? std::to_string(temp) : "";
        std::string mu = mutate > 0.0 ? std::to_string(mutate) : "";
        if (!t.empty() && !mu.empty()) {
            execlp(py.c_str(), py.c_str(), scr.c_str(), "--model",
                   model.c_str(), "--temp", t.c_str(), "--mutate", mu.c_str(), (char*)0);
        } else if (!t.empty()) {
            execlp(py.c_str(), py.c_str(), scr.c_str(), "--model",
                   model.c_str(), "--temp", t.c_str(), (char*)0);
        } else if (!mu.empty()) {
            execlp(py.c_str(), py.c_str(), scr.c_str(), "--model",
                   model.c_str(), "--mutate", mu.c_str(), (char*)0);
        } else {
            execlp(py.c_str(), py.c_str(), scr.c_str(), "--model",
                   model.c_str(), (char*)0);
        }
        std::fprintf(stderr, "[nn] failed to exec %s\n", py.c_str());
        _exit(127);
    } else if (pid > 0) {
        close(a2c[0]); close(c2a[1]);
        g_nn_in[seat] = fdopen(a2c[1], "w");
        g_nn_out[seat] = fdopen(c2a[0], "r");
        g_nn_pid[seat] = (int)pid;
        g_nn_seat[seat] = seat;
        std::fprintf(stderr, "[nn] neural policy seat %d (model=%s temp=%.2f)\n",
                     seat, model.c_str(), temp);
    } else {
        close(a2c[0]); close(a2c[1]); close(c2a[0]); close(c2a[1]);
        std::fprintf(stderr, "[nn] fork failed for seat %d\n", seat);
    }
}

// map a flat option index (idlecmd/battlecmd ordering shared with the
// python decoder) back to the response encoding: category | index<<16.
// Macro order: idle [bp, ep], battle [m2, ep]. shuffle is not offered to the
// neural (mirrors decode.py); such prompts fall back to the policy.
static bool nn_flat_map(uint8_t type, const uint8_t* pbuf, int len,
                        int choice, int& t, int& s) {
    auto u32at = [&](int o) -> uint32_t {
        uint32_t v = 0; std::memcpy(&v, pbuf + o, 4); return v;
    };
    std::vector<int> starts;
    int total = 0;
    int o = 2;                       // skip [type][player]
    std::vector<std::pair<int,int>> macros;   // (slot_kind, resp_category)
    if (type == MSG_SELECT_IDLECMD) {
        const int es[6] = {10, 10, 7, 10, 10, 19};
        for (int c = 0; c < 6 && o + 4 <= len; ++c) {
            uint32_t n = u32at(o); o += 4;
            if (o + (int)n * es[c] > len) return false;
            starts.push_back(total);
            total += (int)n;
            o += (int)n * es[c];
        }
        if (o + 3 > len) return false;
        if (pbuf[o]) macros.push_back({7, 6});      // bp
        if (pbuf[o + 1]) macros.push_back({8, 7});  // ep
    } else if (type == MSG_SELECT_BATTLECMD) {
        if (o + 4 > len) return false;
        uint32_t na = u32at(o); o += 4;
        if (o + (int)na * 19 > len) return false;
        starts.push_back(0);
        total = (int)na;
        o += (int)na * 19;
        if (o + 4 > len) return false;
        uint32_t nn = u32at(o); o += 4;
        if (o + (int)nn * 8 > len) return false;
        starts.push_back(na);
        total += (int)nn;
        o += (int)nn * 8;
        if (o + 2 > len) return false;
        if (pbuf[o]) macros.push_back({10, 2});     // m2
        if (pbuf[o + 1]) macros.push_back({8, 3});  // ep
    } else {
        return false;
    }
    if (choice >= 0 && choice < total) {
        for (size_t c = 0; c < starts.size(); ++c) {
            int end = (c + 1 < starts.size()) ? starts[c + 1] : total;
            if (choice >= starts[c] && choice < end) {
                t = (int)c;
                s = choice - starts[c];
                return true;
            }
        }
    }
    int mslot = choice - total;
    if (mslot >= 0 && mslot < (int)macros.size()) {
        t = macros[mslot].second;
        s = 0;
        return true;
    }
    return false;
}

// deterministic pick that advances on retries so we never loop forever
static int pick(std::mt19937& rng, const int n, const int attempt) {
    if (n <= 0)
        return -1;
    if (attempt == 0)
        return static_cast<int>(rng() % static_cast<uint32_t>(n));
    return (attempt + static_cast<int>(rng() % 7)) % n;
}

// brute-force subset of vals (indices) whose sum == target, with count in
// [lo, hi]. Returns true and fills chosen.
static bool subset_sum(const std::vector<int32_t>& vals, const int target,
                       const int lo, const int hi, std::vector<int>& chosen) {
    const int n = static_cast<int>(vals.size());
    for (uint64_t mask = 0; mask < (1ull << n); ++mask) {
        if (n > 20)
            break;  // too many — fall back to greedy
        int sum = 0, cnt = 0;
        for (int i = 0; i < n; ++i)
            if (mask & (1ull << i)) { sum += vals[i]; cnt++; }
        if (cnt >= lo && cnt <= hi && sum == target) {
            chosen.clear();
            for (int i = 0; i < n; ++i)
                if (mask & (1ull << i)) chosen.push_back(i);
            return true;
        }
    }
    return false;
}

// binary-response helpers (interactive.h references them)
static void set_response(const intptr_t pduel, const std::vector<uint8_t>& b) {
    ml_capture_resp(b.data(), static_cast<uint32_t>(b.size()));
    OCG_DuelSetResponse(reinterpret_cast<OCG_Duel>(pduel), b.data(), static_cast<uint32_t>(b.size()));
}
static void set_response_i(const intptr_t pduel, const int32_t v) {
    ml_capture_resp(&v, sizeof v);
    OCG_DuelSetResponse(reinterpret_cast<OCG_Duel>(pduel), &v, sizeof v);
}

// ---- harness fast-forward helpers -----------------------------------------
static uint32_t harness_phase_code(const std::string& name) {
    if (name == "draw") return 0x01;
    if (name == "standby") return 0x02;
    if (name == "main" || name == "main1") return 0x04;
    if (name == "battle_start" || name == "battle") return 0x08;  // Battle Phase
    if (name == "battle_step" || name == "step") return 0x10;
    if (name == "damage") return 0x20;
    if (name == "damage_cal") return 0x40;
    if (name == "battle_end") return 0x80;
    if (name == "main2") return 0x100;
    if (name == "end") return 0x200;
    return 0x04;
}
static void harness_parse(const std::string& script) {
    g_h_ff = false;
    g_h_turn = 0;
    g_h_player = -1;
    g_h_phase = 0;
    g_moves.clear();
    size_t pos = script.find("-- harness:");
    if (pos != std::string::npos) {
        size_t eol = script.find('\n', pos);
        std::string line = script.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        std::istringstream ss(line);
        std::string tok;
        while (ss >> tok) {
            auto eq = tok.find('=');
            if (eq == std::string::npos) continue;
            std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
            if (k == "turn_player") g_h_player = atoi(v.c_str());
            else if (k == "turn") g_h_turn = atoi(v.c_str());
            else if (k == "phase") g_h_phase = harness_phase_code(v);
        }
        if (g_h_turn > 0 && g_h_player >= 0 && g_h_phase != 0) {
            g_h_ff = true;
            std::fprintf(stderr, "[harness] fast-forward to turn=%d player=%d phase=0x%x\n",
                         g_h_turn, g_h_player, g_h_phase);
        }
    }
    // forced-move lines: "-- hmove: player=.. turn=.. phase=.. prompt=.. do=.."
    size_t mp = 0;
    while ((mp = script.find("-- hmove:", mp)) != std::string::npos) {
        size_t me = script.find('\n', mp);
        std::string line = script.substr(mp, me == std::string::npos ? std::string::npos : me - mp);
        HarMove mv;
        std::istringstream ls(line);
        std::string tok;
        while (ls >> tok) {
            auto eq = tok.find('=');
            if (eq == std::string::npos) continue;
            std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
            if (k == "player") mv.player = (v == "*") ? -1 : atoi(v.c_str());
            else if (k == "turn") mv.turn = (v == "*") ? 0 : atoi(v.c_str());
            else if (k == "phase") mv.phase = (v == "*") ? 0 : harness_phase_code(v);
            else if (k == "prompt") mv.prompt = v;
            else if (k == "do") mv.act = v;
        }
        g_moves.push_back(mv);
        mp = (me == std::string::npos) ? script.size() : me + 1;
    }
    if (!g_moves.empty())
        std::fprintf(stderr, "[harness] %zu forced moves loaded\n", g_moves.size());
}
// which SELECT prompt marks the *start* of the target phase (so we hand
// control at the natural decision point, not mid-chain)
static bool harness_entry_prompt(uint8_t type, uint32_t phase) {
    if (phase == 0x04 || phase == 0x100)      // main phases -> idlecmd
        return type == MSG_SELECT_IDLECMD;
    if (phase == 0x08 || phase == 0x10 || phase == 0x80)  // battle -> battlecmd
        return type == MSG_SELECT_BATTLECMD;
    return true;                              // draw/standby/end: first prompt
}
// neutral responses that end the turn quickly (no model, no recordings).
// Returns true when the prompt was answered; false => fall back to the
// normal responder (rare prompts, e.g. position/card picks).
static bool harness_auto_respond(intptr_t duel, uint8_t type,
                                 const uint8_t* p, uint32_t len) {
    if (type == MSG_SELECT_IDLECMD) {
        // category counts then [to_bp u8][to_ep u8][shuffle u8] (decode.py layout)
        static const int sizes[6] = {10, 10, 7, 10, 10, 19};
        size_t off = 2;
        for (int i = 0; i < 6; ++i) {
            if (off + 4 > len) return false;
            uint32_t n;
            std::memcpy(&n, p + off, 4);
            off += 4;
            if (off + (uint64_t)n * sizes[i] > len) return false;
            off += (size_t)n * sizes[i];
        }
        if (off + 3 > len) return false;
        bool bp = p[off] != 0, ep = p[off + 1] != 0;
        // when the harness target is a battle phase (or Main Phase 2 — which
        // can only be reached by passing through the Battle Phase) of the
        // current turn's player, enter the Battle Phase instead of ending,
        // so the later phase becomes the forced situation under evaluation
        bool want_battle = g_h_ff && g_turns == g_h_turn
            && g_turn_player == g_h_player
            && (g_h_phase == 0x08 || g_h_phase == 0x10 || g_h_phase == 0x80
                || g_h_phase == 0x100)
            && (g_current_phase == 0x04 || g_current_phase == 0x100);
        if (want_battle && bp) { set_response_i(duel, 6); return true; }
        if (ep) { set_response_i(duel, 7); return true; }  // end phase
        if (bp) { set_response_i(duel, 6); return true; }  // battle phase (skip via battlecmd)
        return false;
    }
    if (type == MSG_SELECT_BATTLECMD) {
        size_t off = 2;
        if (off + 4 > len) return false;
        uint32_t na; std::memcpy(&na, p + off, 4); off += 4;
        if (off + (uint64_t)na * 19 > len) return false; off += (size_t)na * 19;
        if (off + 4 > len) return false;
        uint32_t nn; std::memcpy(&nn, p + off, 4); off += 4;
        if (off + (uint64_t)nn * 8 > len) return false; off += (size_t)nn * 8;
        bool m2 = false, ep = false;
        if (off + 2 <= len) { m2 = p[off] != 0; ep = p[off + 1] != 0; }
        // MP2 target: leave the Battle Phase into Main Phase 2 (only legal way)
        bool want_m2 = g_h_ff && g_h_phase == 0x100
            && g_turns == g_h_turn && g_turn_player == g_h_player;
        if (want_m2 && m2) { set_response_i(duel, 2); return true; }
        if (ep) { set_response_i(duel, 3); return true; }  // end phase
        if (m2) { set_response_i(duel, 2); return true; }  // main phase 2
        return false;
    }
    if (type == MSG_SELECT_CHAIN) {
        bool forced = len > 2 && p[2] != 0;
        uint32_t n = 0;
        if (len >= 16) std::memcpy(&n, p + 12, 4);
        if (forced && n > 0) { set_response_i(duel, 0); return true; }
        set_response_i(duel, -1);           // pass
        return true;
    }
    return false;
}

// ---- forced-move directives -----------------------------------------------
static bool hm_idle_counts(const uint8_t* p, uint32_t len, uint32_t cnt[6],
                           bool& bp, bool& ep) {
    static const int sizes[6] = {10, 10, 7, 10, 10, 19};
    size_t off = 2;
    for (int i = 0; i < 6; ++i) {
        if (off + 4 > len) return false;
        std::memcpy(&cnt[i], p + off, 4);
        off += 4;
        if (off + (uint64_t)cnt[i] * sizes[i] > len) return false;
        off += (size_t)cnt[i] * sizes[i];
    }
    if (off + 3 > len) return false;
    bp = p[off] != 0;
    ep = p[off + 1] != 0;
    return true;
}
static bool hm_battle_counts(const uint8_t* p, uint32_t len, uint32_t& na,
                             uint32_t& nn, bool& m2, bool& ep) {
    size_t off = 2;
    if (off + 4 > len) return false;
    std::memcpy(&na, p + off, 4); off += 4;
    if (off + (uint64_t)na * 19 > len) return false; off += (size_t)na * 19;
    if (off + 4 > len) return false;
    std::memcpy(&nn, p + off, 4); off += 4;
    if (off + (uint64_t)nn * 8 > len) return false; off += (size_t)nn * 8;
    m2 = ep = false;
    if (off + 2 <= len) { m2 = p[off] != 0; ep = p[off + 1] != 0; }
    return true;
}
static bool harness_move_respond(intptr_t duel, uint8_t type,
                                 const uint8_t* p, uint32_t len,
                                 const std::string& act) {
    if (type == MSG_SELECT_IDLECMD) {
        uint32_t cnt[6] = {0, 0, 0, 0, 0, 0};
        bool bp = false, ep = false;
        if (!hm_idle_counts(p, len, cnt, bp, ep)) return false;
        if (act == "bp") { if (bp) { set_response_i(duel, 6); return true; } return false; }
        if (act == "ep" || act == "pass") { if (ep) { set_response_i(duel, 7); return true; } return false; }
        int t = -1;
        if (act == "summon") t = 0;
        else if (act == "sp" || act == "spsummon") t = 1;
        else if (act == "repos") t = 2;
        else if (act == "mset") t = 3;
        else if (act == "sset") t = 4;
        else if (act == "act") t = 5;
        if (t >= 0 && cnt[t] > 0) { set_response_i(duel, t); return true; }
        return false;
    }
    if (type == MSG_SELECT_BATTLECMD) {
        uint32_t na = 0, nn = 0;
        bool m2 = false, ep = false;
        if (!hm_battle_counts(p, len, na, nn, m2, ep)) return false;
        if (act == "attack") { if (na || nn) { set_response_i(duel, 1); return true; } return false; }
        if (act == "m2") { if (m2) { set_response_i(duel, 2); return true; } return false; }
        if (act == "ep" || act == "pass") { if (ep) { set_response_i(duel, 3); return true; } return false; }
        return false;
    }
    if (type == MSG_SELECT_CHAIN) {
        if (act == "pass") { set_response_i(duel, -1); return true; }
        if (act == "first") { set_response_i(duel, 0); return true; }
        return false;
    }
    if (type == MSG_SELECT_POSITION) {
        if (act == "atk") { set_response_i(duel, 1); return true; }
        if (act == "def") { set_response_i(duel, 4); return true; }
        return false;
    }
    return false;
}
static bool harness_try_move(intptr_t duel, uint8_t type,
                             const uint8_t* p, uint32_t len) {
    int pl = (len > 1) ? static_cast<int>(p[1]) : -1;
    for (auto& mv : g_moves) {
        if (mv.done) continue;
        if (mv.player != -1 && mv.player != pl) continue;
        if (mv.turn && mv.turn != g_turns) continue;
        if (mv.phase && mv.phase != g_current_phase) continue;
        bool pmatch = mv.prompt == "*";
        if (!pmatch) {
            if (mv.prompt == "idle" && type == MSG_SELECT_IDLECMD) pmatch = true;
            else if (mv.prompt == "battle" && type == MSG_SELECT_BATTLECMD) pmatch = true;
            else if (mv.prompt == "chain" && type == MSG_SELECT_CHAIN) pmatch = true;
            else if (mv.prompt == "position" && type == MSG_SELECT_POSITION) pmatch = true;
            else if (mv.prompt == "card" && type == MSG_SELECT_CARD) pmatch = true;
        }
        if (!pmatch) continue;
        if (harness_move_respond(duel, type, p, len, mv.act)) {
            mv.done = true;
            std::fprintf(stderr,
                         "[harness] move fired: player=%d turn=%d phase=0x%x "
                         "prompt=%s do=%s\n",
                         pl, g_turns, g_current_phase,
                         mv.prompt.c_str(), mv.act.c_str());
            return true;
        }
    }
    return false;
}

// forward decls (defined below with the field-model helpers)
static int best_attack_target(const std::vector<uint32_t>& codes,
                              const std::vector<uint32_t>& seqs,
                              const std::vector<uint32_t>& poss,
                              uint32_t attacker_code, int32_t attacker_atk);
static int policy_pick(const std::vector<uint32_t>& codes, const Policy& pol,
                       uint64_t hint, std::mt19937& rng, int attempt, bool& used);

// Respond to a SELECT message. `pbuf` points at the message type uint8_t.
// The EDOPro core's response protocol: scalar answers are a 4-uint8_t int32;
// card selections use [int32 type][uint32 count][indices] where type 2 = u8
// indices (type 3 = bitmask, 0/1 = u32/u16 indices); sort uses int8 entries.
static bool handle_select(intptr_t pduel, uint8_t type, const uint8_t* pbuf,
                          int len, std::mt19937& rng) {
    if (g_verbose) std::fprintf(stderr, "  [select] type=%d len=%d\n", static_cast<int>(type), len);
    Msg m(pbuf);
    m.u8();  // message type
    int attempt = g_retries;
    auto resp_i = [&](const int32_t v) {
        ml_capture_resp(&v, sizeof v);
        OCG_DuelSetResponse((OCG_Duel)pduel, &v, sizeof v);
    };
    auto resp_b = [&](const std::vector<uint8_t>& b) {
        ml_capture_resp(b.data(), static_cast<uint32_t>(b.size()));
        OCG_DuelSetResponse((OCG_Duel)pduel, b.data(), static_cast<uint32_t>(b.size()));
    };
    auto cards_resp = [&](const std::vector<int>& idx) {
        // type 2: [int32 2][uint32 count][u8 indices]
        std::vector<uint8_t> b(8 + idx.size());
        int32_t t = 2; std::memcpy(b.data(), &t, 4);
        uint32_t c = static_cast<uint32_t>(idx.size()); std::memcpy(b.data() + 4, &c, 4);
        for (size_t i = 0; i < idx.size(); ++i) b[8 + i] = static_cast<uint8_t>(idx[i]);
        resp_b(b);
    };
    switch (type) {
    case MSG_SELECT_YESNO: {
        int player = m.u8(); m.u64();           // player, description
        if (player == g_search_seat && g_search_rollouts > 0 && attempt == 0) {
            std::fprintf(stderr, "[search] yesno ENTERED\n");
            auto res = search_actions(pduel, type, pbuf, len, {"0", "1"},
                                      g_search_rollouts, seed_of_rng(rng));
            if (res.ran) {
                if (g_verbose) {
                    std::fprintf(stderr, "[search] yesno -> %d (scores ", res.chosen);
                    for (auto& sc : res.scores)
                        std::fprintf(stderr, "%d:%.0f%% ", sc.first, sc.second);
                    std::fprintf(stderr, ")\n");
                }
                if (g_assess) { emit_search_report("yesno", res); g_assessed = true; }
                resp_i(res.chosen == 1 ? 1 : 0);
                return false;
            }
        }
        // generic consent: say yes (activating our plays is usually right)
        const Policy& pol = g_policy[player];
        (void)pol;
        resp_i(pick(rng, 10, attempt) < 8 ? 1 : 0);
        return false;
    }
    case MSG_SELECT_EFFECTYN: {
        int player = m.u8(); uint32_t code = m.u32();
        uint8_t ctrl = m.u8(); uint8_t loc = m.u8();   // handler's controller/location
        m.u32(); m.u32(); m.u64();                      // seq, pos, desc
        if (player == g_search_seat && g_search_rollouts > 0 && attempt == 0) {
            auto res = search_actions(pduel, type, pbuf, len, {"0", "1"},
                                      g_search_rollouts, seed_of_rng(rng));
            if (res.ran) {
                if (g_verbose)
                    std::fprintf(stderr, "[search] effectyn(%u) -> %d\n", code, res.chosen);
                if (g_assess) { emit_search_report("effectyn", res); g_assessed = true; }
                resp_i(res.chosen == 1 ? 1 : 0);
                return false;
            }
        }
        const Policy& pol = g_policy[player];
        if (pol.act_score(code) > 0) { resp_i(1); return false; }
        // own-card effects (revives like Stardust's, self-protections) are
        // almost always worth taking — don't leave the decision to a coin
        // flip inside rollout children
        if (ctrl == static_cast<uint8_t>(player) && (loc == 0x10 /* GRAVE */ || loc == 0x04 /* MZONE */)) {
            resp_i(1);
            return false;
        }
        resp_i(pick(rng, 10, attempt) < 7 ? 1 : 0);
        return false;
    }
    case MSG_SELECT_OPTION: {
        m.u8();
        int n = m.u8();
        m.skip(8 * n);
        resp_i(pick(rng, n, attempt));
        return false;
    }
    case MSG_SELECT_CARD: case MSG_SELECT_TRIBUTE: {
        int player = m.u8();
        int cancelable = m.u8();
        int min = static_cast<int>(m.u32()), max = static_cast<int>(m.u32());
        int n = static_cast<int>(m.u32());
        if (g_trace)
            std::fprintf(stderr, "[select] hint=%llu type=%d min=%d max=%d n=%d\n",
                         static_cast<unsigned long long>(g_last_hint_selectmsg), type, min, max, n);
        std::vector<uint32_t> codes(n), seqs(n), poss(n);
        for (int i = 0; i < n; ++i) {
            codes[i] = m.u32();
            uint8_t ctrl = m.u8(); uint8_t loc = m.u8();
            uint32_t seq = m.u32(); uint32_t pos = m.u32();
            seqs[i] = seq; poss[i] = pos;
            if (type == MSG_SELECT_TRIBUTE) m.u8();  // release_param
            (void)ctrl; (void)loc;
        }
        if (attempt >= 3 && cancelable && min == 0) { resp_i(-1); return false; }
        // search: for the AI, evaluate each single-card target by rollout.
        // This is where effects like Dark Armed Dragon pick what to destroy —
        // the greedy policy had no lookahead and would nuke its own board.
        if (player == g_search_seat && g_search_rollouts > 0 && attempt == 0 && max == 1) {
            std::vector<std::string> acts;
            for (int i = 0; i < n; ++i) acts.push_back(std::to_string(i));
            if (cancelable && min == 0) acts.push_back("-1");
            auto res = search_actions(pduel, type, pbuf, len, acts,
                                      g_search_rollouts, seed_of_rng(rng));
            if (res.ran && res.chosen >= 0) {
                if (g_verbose)
                    std::fprintf(stderr, "[search] card: chosen=%d (target %s)\n",
                                 res.chosen, acts[res.chosen].c_str());
                if (g_assess) {
                    emit_search_report(type == MSG_SELECT_TRIBUTE ? "tribute" : "card", res);
                    g_assessed = true;
                }
                const std::string& a = acts[res.chosen];
                if (a == "-1") { resp_i(-1); return false; }
                cards_resp({std::vector<int>{atoi(a.c_str())}});
                return false;
            }
        }
        const Policy& pol = g_policy[player];
        bool used = false;
        std::vector<int> idx;
        // neural policy: the model answers single-card selects on its seat
        if ((nn_ready(static_cast<int>(player)))
            && attempt == 0 && min == 1 && max == 1 && n >= 2) {
            int nnc = -1;
            if (nn_query(type, static_cast<uint8_t>(player), pbuf, len, nnc)
                && nnc >= 0 && nnc < n) {
                cards_resp({nnc});
                return false;
            }
        }
        // label-only search (--search-labels): compute the rollout-best pick
        // for the record; the policy below still plays the game. Single-card
        // selects only (multi-card subsets are out of scope for v1 labels).
        if (g_ml_labels > 0 && g_ml_out && g_search_rollouts == 0 && attempt == 0
            && !nn_active(static_cast<int>(player))
            && min == 1 && max == 1 && n >= 2) {
            std::vector<std::string> acts;
            for (int i = 0; i < n; ++i) acts.push_back(std::to_string(i));
            auto res = search_actions(pduel, type, pbuf, len, acts,
                                      g_ml_labels, seed_of_rng(rng));
            if (res.ran)
                ml_pend_label(res, false);
        }
        // attack-target selection (hint 549) — pick the target we kill
        if (g_last_hint_selectmsg == 549 && g_attack_pending && type == MSG_SELECT_CARD) {
            int pick_idx = best_attack_target(codes, seqs, poss,
                                              g_attacker_code, g_attacker_atk);
            if (pick_idx >= 0) {
                idx.push_back(pick_idx);
                used = true;
                g_attack_pending = false;
            }
        }
        // tribute/release/multi-card: pick the top-N by policy score
        if (!used && (g_last_hint_selectmsg == 531 || g_last_hint_selectmsg == 500 ||
                      g_last_hint_selectmsg == 501 || type == MSG_SELECT_TRIBUTE)) {
            std::vector<std::pair<int, int>> scored;   // (score, index)
            for (int i = 0; i < n; ++i) {
                int sc = pol.tribute_score(codes[i]) + pol.dump_score(codes[i])
                         - pol.keep_score(codes[i]) * 2;
                scored.push_back({sc, i});
            }
            std::stable_sort(scored.begin(), scored.end(),
                             [](auto& a, auto& b) { return a.first > b.first; });
            for (int k = 0; k < max && k < n; ++k) idx.push_back(scored[k].second);
            used = true;
        }
        // single-card generic target
        if (!used && min == 1 && max == 1) {
            int pick_idx = policy_pick(codes, pol, g_last_hint_selectmsg, rng, attempt, used);
            if (used && pick_idx >= 0) idx.push_back(pick_idx);
        }
        if (idx.empty()) {
            for (int i = 0; i < n; ++i) idx.push_back(i);
            std::shuffle(idx.begin(), idx.end(), rng);
            if (static_cast<int>(idx.size()) > max) idx.resize(max);
        }
        while (static_cast<int>(idx.size()) < min) {
            for (int i = 0; i < n && static_cast<int>(idx.size()) < min; ++i)
                if (std::find(idx.begin(), idx.end(), i) == idx.end())
                    idx.push_back(i);
            break;
        }
        cards_resp(idx);
        return false;
    }
    case MSG_SELECT_CHAIN: {
        int player = m.u8(); m.u8(); int forced = m.u8();
        m.skip(8);                    // hint timings
        int n = static_cast<int>(m.u32());
        std::vector<uint32_t> codes(n);
        std::vector<uint8_t> chain_ctrl(n);   // controller of each option's handler
        std::vector<uint8_t> chain_loc(n);    // location of each option's handler
        for (int i = 0; i < n; ++i) {
            // one link is 24 bytes: code(4) ctrl(1) loc(1) seq(4) pos(4)
            // desc(8) client_mode(1) trigger_flag(1)
            codes[i] = m.u32();
            chain_ctrl[i] = m.u8();
            chain_loc[i] = m.u8();
            m.u32();                  // seq
            m.u32();                  // pos
            m.u64();                  // desc
            m.u8();                   // client_mode
            m.u8();                   // trigger flag (not used by the policy)
        }
        const Policy& pol = g_policy[player];
        // neural policy: the model answers non-forced chain prompts on its seat
        if ((nn_ready(static_cast<int>(player)))
            && attempt == 0 && !forced && n > 0) {
            int nnc = -2;
            if (nn_query(type, static_cast<uint8_t>(player), pbuf, len, nnc)) {
                if (nnc == -1) { resp_i(-1); return false; }
                if (nnc >= 0 && nnc < n) { resp_i(nnc); return false; }
            }
        }
        // label-only search (--search-labels): rollout-best chain response
        // (pass vs activate option k) for the record; the greedy policy below
        // still plays the game.
        if (g_ml_labels > 0 && g_ml_out && g_search_rollouts == 0 && attempt == 0
            && !nn_active(static_cast<int>(player))
            && !forced && n > 0) {
            std::vector<std::string> acts;
            acts.push_back("-1");   // pass
            for (int i = 0; i < n; ++i) acts.push_back(std::to_string(i));
            auto res = search_actions(pduel, type, pbuf, len, acts,
                                      g_ml_labels, seed_of_rng(rng));
            if (res.ran)
                ml_pend_label(res, true);
        }
        // search: for the AI, evaluate pass vs each chain option by rollout
        if (player == g_search_seat && g_search_rollouts > 0 && attempt == 0 && !forced && n > 0) {
            std::vector<std::string> acts;
            acts.push_back("-1");   // pass
            for (int i = 0; i < n; ++i) acts.push_back(std::to_string(i));
            auto res = search_actions(pduel, type, pbuf, len, acts,
                                      g_search_rollouts, seed_of_rng(rng));
            if (res.ran) {
                if (g_verbose) {
                    std::fprintf(stderr, "[search] chain: ");
                    for (auto& sc : res.scores)
                        std::fprintf(stderr, "%s:%.0f%% ", sc.first == 0 ? "pass" : std::to_string(sc.first - 1).c_str(), sc.second);
                    std::fprintf(stderr, "-> pick %d\n", res.chosen);
                }
                if (g_assess) { emit_search_report("chain", res); g_assessed = true; }
                resp_i(res.chosen - 1);   // action index; -1 = pass
                return false;
            }
        }
        // prefer activating our high-priority cards; forced -> 0
        int best = -1, best_score = 0;
        for (int i = 0; i < n; ++i) {
            int sc = pol.act_score(codes[i]) + pol.summon_score(codes[i]);
            // own monster quick effects (Stardust's negation etc.): when the
            // handler is a monster we control on the field, treat it as a
            // high-priority chain option so the AI actually negates/protects
            // instead of coin-flipping on whether to chain.
            if (chain_ctrl[i] == static_cast<uint8_t>(player) && chain_loc[i] == 0x04)
                sc += 70;
            if (sc > best_score) { best_score = sc; best = i; }
        }
        if (g_verbose && n > 0)
            std::fprintf(stderr, "  [chain greedy] p=%d n=%d codes=[%u%s] best=%d sc=%d forced=%d\n",
                         player, n, codes.empty() ? 0 : codes[0],
                         n > 1 ? ",.." : "", best, best_score, forced);
        if (forced) { resp_i(0); return false; }
        if (best >= 0 && best_score > 0 && pick(rng, 10, attempt) < 9) {
            resp_i(best);
            return false;
        }
        if (n > 0 && pick(rng, n + 1, attempt) < n) resp_i(pick(rng, n, attempt));
        else resp_i(-1);
        return false;
    }
    case MSG_SELECT_PLACE: case MSG_SELECT_DISFIELD: {
        int player = m.u8();
        int count = m.u8();
        uint32_t flag = m.u32();
        std::vector<std::tuple<uint8_t, uint8_t, uint8_t>> zones;
        for (int p = 0; p < 2; ++p)
            for (int l : {LOCATION_MZONE, LOCATION_SZONE}) {
                int max_s = (l == LOCATION_MZONE) ? 6 : 7;
                for (int s = 0; s <= max_s; ++s) {
                    uint32_t bit = 1u << (s + (p == player ? 0 : 16) + (l == LOCATION_MZONE ? 0 : 8));
                    if (!(bit & flag))
                        zones.push_back({static_cast<uint8_t>(p), static_cast<uint8_t>(l), static_cast<uint8_t>(s)});
                }
            }
        std::shuffle(zones.begin(), zones.end(), rng);
        std::vector<uint8_t> b;
        for (int i = 0; i < count && !zones.empty(); ++i) {
            auto z = zones[i % zones.size()];
            b.push_back(std::get<0>(z)); b.push_back(std::get<1>(z)); b.push_back(std::get<2>(z));
        }
        resp_b(b);
        return false;
    }
    case MSG_SELECT_POSITION: {
        m.u8(); m.u32();
        int positions = m.u8();
        std::vector<int> opts;
        for (int b = 1; b <= 8; b <<= 1)
            if (positions & b) opts.push_back(b);
        resp_i(opts.empty() ? 1 : opts[pick(rng, static_cast<int>(opts.size()), attempt)]);
        return false;
    }
    case MSG_SELECT_COUNTER: {
        m.u8(); m.u16(); int required = m.u16();
        int n = static_cast<int>(m.u32());
        std::vector<int16_t> avail(n);
        for (int i = 0; i < n; ++i) { m.u32(); m.skip(3); avail[i] = static_cast<int16_t>(m.u16()); }
        std::vector<uint8_t> b(2 * n, 0);
        int total = 0;
        for (int i = 0; i < n && total < required; ++i) {
            int take = std::min(static_cast<int>(avail[i]), required - total);
            b[2 * i] = static_cast<uint8_t>(take & 0xff); b[2 * i + 1] = static_cast<uint8_t>(take >> 8);
            total += take;
        }
        resp_b(b);
        return false;
    }
    case MSG_SELECT_SUM: {
        m.u8(); int mode = m.u8();
        int32_t acc = static_cast<int32_t>(m.u32() & 0xffff);
        int min = static_cast<int>(m.u32()), max = static_cast<int>(m.u32());
        int mcount = static_cast<int>(m.u32());
        std::vector<int32_t> must(mcount);
        for (int i = 0; i < mcount; ++i) { m.u32(); m.skip(10); must[i] = static_cast<int32_t>(m.u32()); }
        int n = static_cast<int>(m.u32());
        std::vector<int32_t> vals(n);
        for (int i = 0; i < n; ++i) { m.u32(); m.skip(10); vals[i] = static_cast<int32_t>(m.u32()); }
        int must_sum = 0;
        for (int v : must) must_sum += v;
        std::vector<int> chosen;
        bool ok = subset_sum(vals, acc - must_sum, std::max(0, min), max, chosen);
        if (!ok && n >= min && n <= max) { chosen.resize(n); for (int i = 0; i < n; ++i) chosen[i] = i; }
        cards_resp(chosen);
        return false;
    }
    case MSG_SORT_CARD: case MSG_SORT_CHAIN: {
        m.u8();
        int n = static_cast<int>(m.u32());
        m.skip(13 * n);
        std::vector<uint8_t> b;
        for (int i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(i));  // identity permutation
        resp_b(b);
        return false;
    }
    case MSG_SELECT_UNSELECT_CARD: {
        m.u8(); m.u8(); int cancelable = m.u8();
        m.u32(); m.u32();
        int n = static_cast<int>(m.u32()); m.skip(14 * n);
        int n2 = static_cast<int>(m.u32()); m.skip(14 * n2);
        if (attempt >= 3 && cancelable) { resp_i(-1); return false; }
        std::vector<uint8_t> b(8);
        int32_t one = 1; std::memcpy(b.data(), &one, 4);
        int32_t idx = (n + n2) ? pick(rng, n + n2, attempt) : 0;
        std::memcpy(b.data() + 4, &idx, 4);
        resp_b(b);
        return false;
    }
    case MSG_SELECT_IDLECMD: {
        if ((nn_ready(pbuf[1])) && attempt == 0
            && g_search_rollouts == 0) {
            int nnc = -1, t = 0, s = 0;
            if (nn_query(MSG_SELECT_IDLECMD, pbuf[1], pbuf, len, nnc)
                && nn_flat_map(MSG_SELECT_IDLECMD, pbuf, len, nnc, t, s)) {
                resp_i(t | (s << 16));
                return false;
            }
        }

        int player = m.u8();
        int summon = static_cast<int>(m.u32());
        std::vector<uint32_t> summon_codes(summon);
        for (int i = 0; i < summon; ++i) { summon_codes[i] = m.u32(); m.skip(6); }
        int spsummon = static_cast<int>(m.u32());
        std::vector<uint32_t> spsummon_codes(spsummon);
        for (int i = 0; i < spsummon; ++i) { spsummon_codes[i] = m.u32(); m.skip(6); }
        int repo = static_cast<int>(m.u32());
        std::vector<uint32_t> repo_codes(repo);
        for (int i = 0; i < repo; ++i) { repo_codes[i] = m.u32(); m.skip(3); }
        int mset = static_cast<int>(m.u32());
        std::vector<uint32_t> mset_codes(mset);
        for (int i = 0; i < mset; ++i) { mset_codes[i] = m.u32(); m.skip(6); }
        int sset = static_cast<int>(m.u32());
        std::vector<uint32_t> sset_codes(sset);
        for (int i = 0; i < sset; ++i) { sset_codes[i] = m.u32(); m.skip(6); }
        int act = static_cast<int>(m.u32());
        std::vector<uint32_t> act_codes(act);
        for (int i = 0; i < act; ++i) { act_codes[i] = m.u32(); m.skip(15); }
        int to_bp = m.u8(), to_ep = m.u8(), can_shuffle = m.u8();
        if (g_verbose)
            std::fprintf(stderr, "  [idlecmd] p=%d summon=%d spsummon=%d repo=%d mset=%d sset=%d act=%d to_bp=%d to_ep=%d\n",
                         player, summon, spsummon, repo, mset, sset, act, to_bp, to_ep);
        const Policy& pol = g_policy[player];
        // category-specific scoring: SETTING a card is not the same as
        // ACTIVATING it, and a normal summon is its own priority.  The old
        // combined score made "set Skill Drain" (act priority 95) beat
        // "activate Heavy Storm" (act priority 80) even though storm must be
        // played before committing your own backrow.
        auto top = [&](const std::vector<uint32_t>& cs, auto score) {
            int best = -1, bs = -1;
            for (size_t i = 0; i < cs.size(); ++i) {
                int sc = score(cs[i]);
                if (sc > bs) { bs = sc; best = static_cast<int>(i); }
            }
            return std::make_pair(best, bs);
        };
        // flip-effect monsters (TYPE_FLIP) want to be SET face-down, not
        // normal summoned face-up — their effect only triggers on flip.
        auto is_flip = [&](const uint32_t c) {
            uint32_t t = 0;
            return card_type_bits(c, t) && (t & TYPE_FLIP) != 0;
        };
        auto summon_score = [&](const uint32_t c) {
            int s = pol.summon_score(c);
            if (is_flip(c)) s -= 100;   // don't summon a flip monster face-up
            return s;
        };
        auto mset_score = [&](const uint32_t c) {
            int s = pol.summon_score(c);
            if (is_flip(c)) s += 40;    // setting a flip monster is its real play
            return s;
        };
        auto act_score = [&](const uint32_t c) { return pol.act_score(c); };
        auto set_score = [&](const uint32_t c) { return pol.set_score(c); };

        // candidate actions for rollout search: the best card per category,
        // plus the phase transitions.
        std::vector<std::string> search_acts;
        std::vector<int> search_flat;   // flat decode.py option index per act
        int flat_cards = summon + spsummon + repo + mset + sset + act;
        auto add_best = [&](const char* kind, const std::vector<uint32_t>& cs,
                            auto score, int flat_base) {
            auto pr = top(cs, score);
            if (pr.first >= 0 && pr.second >= 0) {
                search_acts.push_back("{\"action\":\"" + std::string(kind) +
                                      "\",\"index\":" + std::to_string(pr.first) + "}");
                search_flat.push_back(flat_base + pr.first);
            }
        };
        add_best("summon", summon_codes, summon_score, 0);
        add_best("spsummon", spsummon_codes, summon_score, summon);
        add_best("mset", mset_codes, mset_score, summon + spsummon + repo);
        add_best("sset", sset_codes, set_score, summon + spsummon + repo + mset);
        add_best("act", act_codes, act_score, summon + spsummon + repo + mset + sset);
        if (to_bp) { search_acts.push_back("{\"action\":\"to_bp\"}");
                     search_flat.push_back(flat_cards); }
        if (to_ep) { search_acts.push_back("{\"action\":\"to_ep\"}");
                     search_flat.push_back(flat_cards + (to_bp ? 1 : 0)); }

        // label-only search (--search-labels): rollout-best main-phase action
        // mapped to the supervised dataset's flat idlecmd option index.
        if (g_ml_labels > 0 && g_ml_out && g_search_rollouts == 0 && attempt == 0
            && !nn_active(pbuf[1]) && !search_acts.empty()) {
            auto res = search_actions(pduel, type, pbuf, len, search_acts,
                                      g_ml_labels, seed_of_rng(rng));
            if (res.ran && res.chosen >= 0)
                ml_pend_label_idx(search_flat[res.chosen], res.rollouts);
        }

        // look-ahead search over the main-phase options (player 1 only).
        // This is what fixes sequencing: each candidate is rolled out, so
        // "storm first, then set" out-scores "set first, then storm".
        if (player == g_search_seat && g_search_rollouts > 0 && attempt == 0 && !search_acts.empty()) {
            auto res = search_actions(pduel, type, pbuf, len, search_acts,
                                      g_search_rollouts, seed_of_rng(rng));
            if (res.ran && res.chosen >= 0) {
                if (g_assess) { emit_search_report("idle", res); g_assessed = true; }
                apply_search_action(pduel, type, pbuf, len, search_acts[res.chosen]);
                return false;
            }
        }

        // greedy fallback (rollout children and no-search mode)
        struct Cmd { int type; int score; int sub; };
        std::vector<Cmd> cmds;
        {
            auto pr = top(summon_codes, summon_score);
            if (pr.first >= 0 && pr.second >= 0) cmds.push_back({0, pr.second + 40, pr.first});
        }
        {
            auto pr = top(spsummon_codes, summon_score);
            if (pr.first >= 0 && pr.second >= 0) cmds.push_back({1, pr.second + 30, pr.first});
        }
        {
            auto pr = top(mset_codes, mset_score);
            if (pr.first >= 0 && pr.second >= 0) cmds.push_back({3, pr.second + 15, pr.first});
        }
        {
            auto pr = top(sset_codes, set_score);
            if (pr.first >= 0 && pr.second >= 0) cmds.push_back({4, pr.second + 20, pr.first});
        }
        {
            auto pr = top(act_codes, act_score);
            if (pr.first >= 0 && pr.second >= 0) cmds.push_back({5, pr.second + 25, pr.first});
        }
        // enter battle phase if we likely have a winning attack
        int my_atk = 0;
        for (auto& c : g_field.mzone(player)) my_atk = std::max(my_atk, c.atk);
        int opp_def = 0, opp_atk = 0;
        for (auto& c : g_field.mzone(1 - player)) {
            if (c.faceup) {
                if (c.pos & 0x4) opp_def = std::max(opp_def, c.def);
                else opp_atk = std::max(opp_atk, c.atk);
            } else {
                // face-down monsters sit in face-down DEF — their DEF is what
                // an attacker has to beat, not their (hidden) ATK.
                opp_def = std::max(opp_def, c.def);
            }
        }
        bool good_attack = (g_field.mzone(1 - player).empty()) ||
            (my_atk > opp_atk && my_atk > opp_def);
        // note: we do NOT block entering BP here on known traps, so the
        // battlecmd search still runs; the greedy ATTACK decision inside
        // battlecmd is where known threats stop the attack.
        if (to_bp && good_attack) cmds.push_back({6, 50, 0});
        if (to_ep) cmds.push_back({7, 0, 0});
        if (cmds.empty()) {
            resp_i(to_ep ? 7 : 6);   // safest fallback: end phase (or bp)
            return false;
        }
        std::stable_sort(cmds.begin(), cmds.end(),
                         [](const Cmd& a, const Cmd& b) { return a.score > b.score; });
        // add noise: mostly take the best, occasionally second (but in assess
        // mode play the best line deterministically so the scenario shows the
        // AI's real intent instead of a random skip of the battle phase)
        int take = 0;
        if (!g_assess && pick(rng, 10, attempt) >= 8 && cmds.size() > 1) take = 1;
        Cmd& c = cmds[take];
        int sub = c.sub < 0 ? 0 : c.sub;
        if (g_verbose) {
            std::fprintf(stderr, "  [idlecmd resp] p=%d ncmds=%zu take=%d type=%d sub=%d\n",
                         player, cmds.size(), take, c.type, sub);
            for (auto& cc : cmds)
                std::fprintf(stderr, "    cmd type=%d score=%d\n", cc.type, cc.score);
        }
        resp_i(c.type | (sub << 16));
        return false;
    }
    case MSG_SELECT_BATTLECMD: {
        if ((nn_ready(pbuf[1])) && attempt == 0
            && g_search_rollouts == 0) {
            int nnc = -1, t = 0, s = 0;
            if (nn_query(MSG_SELECT_BATTLECMD, pbuf[1], pbuf, len, nnc)
                && nn_flat_map(MSG_SELECT_BATTLECMD, pbuf, len, nnc, t, s)) {
                resp_i(t | (s << 16));
                return false;
            }
        }

        int player = m.u8();
        if (g_verbose) std::fprintf(stderr, "  [battlecmd] player=%d search_rollouts=%d attempt=%d\n",
                                    player, g_search_rollouts, attempt);
        int act = static_cast<int>(m.u32());
        std::vector<uint32_t> act_codes(act);
        for (int i = 0; i < act; ++i) { act_codes[i] = m.u32(); m.skip(15); }
        int atk = static_cast<int>(m.u32());
        std::vector<uint32_t> atk_codes(atk);
        std::vector<uint32_t> atk_seqs(atk);
        for (int i = 0; i < atk; ++i) {
            atk_codes[i] = m.u32();
            uint8_t c = m.u8(); uint8_t l = m.u8();
            uint8_t seq8 = m.u8(); uint8_t da = m.u8();
            atk_seqs[i] = seq8;
            (void)c; (void)l; (void)da;
        }
        int to_m2 = m.u8(), to_ep = m.u8();
        // label-only search (--search-labels): rollout-best attack/m2/ep,
        // mapped to the dataset's flat battlecmd option index (activate
        // entries first, then attackers, then m2/ep).
        if (g_ml_labels > 0 && g_ml_out && g_search_rollouts == 0 && attempt == 0
            && !nn_active(pbuf[1])) {
            std::vector<std::string> lacts;
            std::vector<int> lflat;
            for (int i = 0; i < atk; ++i) {
                lacts.push_back("{\"action\":\"attack\",\"index\":" +
                                std::to_string(i) + "}");
                lflat.push_back(act + i);
            }
            if (to_m2) { lacts.push_back("{\"action\":\"to_m2\"}");
                         lflat.push_back(act + atk); }
            if (to_ep) { lacts.push_back("{\"action\":\"to_ep\"}");
                         lflat.push_back(act + atk + (to_m2 ? 1 : 0)); }
            if (!lacts.empty()) {
                auto res = search_actions(pduel, type, pbuf, len, lacts,
                                          g_ml_labels, seed_of_rng(rng));
                if (res.ran && res.chosen >= 0)
                    ml_pend_label_idx(lflat[res.chosen], res.rollouts);
            }
        }
        // search: for the AI, evaluate attacking with each monster vs passing
        if (player == g_search_seat && g_search_rollouts > 0 && attempt == 0) {
            std::vector<std::string> acts;
            for (int i = 0; i < atk; ++i) {
                std::string a = "{\"action\":\"attack\",\"index\":" + std::to_string(i) + "}";
                acts.push_back(a);
            }
            if (to_m2) acts.push_back("{\"action\":\"to_m2\"}");
            if (to_ep) acts.push_back("{\"action\":\"to_ep\"}");
            if (!acts.empty()) {
                auto res = search_actions(pduel, type, pbuf, len, acts,
                                          g_search_rollouts, seed_of_rng(rng));
                if (res.ran) {
                    if (g_verbose) {
                        std::fprintf(stderr, "[search] battle: ");
                        for (auto& sc : res.scores)
                            std::fprintf(stderr, "%d:%.0f%% ", sc.first, sc.second);
                        std::fprintf(stderr, "-> %d\n", res.chosen);
                    }
                    if (g_assess) {
                        g_report_attackers = atk_codes;
                        emit_search_report("battle", res);
                        g_report_attackers.clear();
                        g_assessed = true;
                    }
                    if (res.chosen >= 0) {
                        // respond with the chosen action directly
                        size_t idx = acts[res.chosen].find("\"index\":");
                        int t = (acts[res.chosen].find("attack") != std::string::npos) ? 1
                              : (acts[res.chosen].find("to_m2") != std::string::npos) ? 2 : 3;
                        int s = 0;
                        if (idx != std::string::npos) s = atoi(acts[res.chosen].c_str() + idx + 8);
                        if (t == 1) {
                            g_attacker_code = atk_codes[s];
                            int32_t a = 0, d = 0;
                            card_stats(atk_codes[s], a, d);
                            g_attacker_atk = a;
                            g_attack_pending = true;
                        }
                        resp_i(t | (s << 16));
                        return false;
                    }
                }
            }
        }
        const Policy& pol = g_policy[player];
        // prefer activation of high-priority cards before attacking
        int best_act = -1, best_act_score = 0;
        for (int i = 0; i < act; ++i) {
            int sc = pol.act_score(act_codes[i]) + pol.set_score(act_codes[i]);
            if (sc > best_act_score) { best_act_score = sc; best_act = i; }
        }
        if (best_act >= 0 && best_act_score >= 40 && pick(rng, 10, attempt) < 9) {
            resp_i(best_act);   // type 0 = activate
            return false;
        }
        // attack with our strongest attacker if it can win
        int best_atk = -1;
        int32_t best_atk_val = 0;
        for (int i = 0; i < atk; ++i) {
            int32_t a = 0, d = 0;
            card_stats(atk_codes[i], a, d);
            if (a > best_atk_val) { best_atk_val = a; best_atk = i; }
        }
        int opp_atk = 0, opp_def = 0;
        for (auto& c : g_field.mzone(1 - player)) {
            if (c.faceup) {
                if (c.pos & 0x4) opp_def = std::max(opp_def, c.def);
                else opp_atk = std::max(opp_atk, c.atk);
            } else {
                // face-down monsters sit in face-down DEF — their DEF is what
                // an attacker has to beat, not their (hidden) ATK.
                opp_def = std::max(opp_def, c.def);
            }
        }
        bool opp_field = !g_field.mzone(1 - player).empty();
        bool can_kill = opp_field ? (best_atk_val > opp_atk || best_atk_val > opp_def) : true;
        bool will_attack = can_kill || (pol.aggressiveness >= 70 && best_atk_val > 0);
        // threat awareness: never walk into a KNOWN attack-reaction trap
        // (perfect memory).  This also makes rollout children play around it,
        // so the search correctly prices passing vs attacking.
        if (opponent_has_known_attack_trap(static_cast<uint8_t>(1 - player))) {
            if (g_verbose)
                std::fprintf(stderr, "  [threat] player %d sees known attack trap, no attack\n", player);
            will_attack = false;
        }
        if (best_atk >= 0 && will_attack && pick(rng, 10, attempt) < 9) {
            g_attacker_code = atk_codes[best_atk];
            g_attacker_atk = best_atk_val;
            g_attack_pending = true;
            resp_i(1 | (best_atk << 16));   // type 1 = attack
            return false;
        }
        if (to_m2) { resp_i(2); return false; }
        if (to_ep) { resp_i(3); return false; }
        resp_i(2);
        return false;
    }
    case MSG_ROCK_PAPER_SCISSORS: {
        m.u8();
        resp_i(1 + pick(rng, 3, attempt));      // valid: 1..3
        return false;
    }
    case MSG_ANNOUNCE_RACE: {
        m.u8(); int cnt = m.u8(); uint64_t available = m.u64();
        std::vector<int> bits;
        for (int b = 1; b < 64; b <<= 1)
            if (available & static_cast<uint64_t>(b)) bits.push_back(b);
        std::shuffle(bits.begin(), bits.end(), rng);
        uint64_t sel = 0;
        for (int i = 0; i < cnt && i < static_cast<int>(bits.size()); ++i) sel |= static_cast<uint64_t>(bits[i]);
        resp_b({static_cast<uint8_t>(sel & 0xff), static_cast<uint8_t>((sel >> 8) & 0xff),
                static_cast<uint8_t>((sel >> 16) & 0xff), static_cast<uint8_t>((sel >> 24) & 0xff),
                static_cast<uint8_t>((sel >> 32) & 0xff), static_cast<uint8_t>((sel >> 40) & 0xff),
                static_cast<uint8_t>((sel >> 48) & 0xff), static_cast<uint8_t>((sel >> 56) & 0xff)});
        return false;
    }
    case MSG_ANNOUNCE_ATTRIB: {
        m.u8(); int cnt = m.u8(); uint32_t available = m.u32();
        std::vector<int> bits;
        for (int b = 1; b < 32; b <<= 1)
            if (available & static_cast<uint32_t>(b)) bits.push_back(b);
        std::shuffle(bits.begin(), bits.end(), rng);
        uint32_t sel = 0;
        for (int i = 0; i < cnt && i < static_cast<int>(bits.size()); ++i) sel |= static_cast<uint32_t>(bits[i]);
        resp_i(static_cast<int32_t>(sel));
        return false;
    }
    case MSG_ANNOUNCE_CARD: {
        m.u8();
        int n = m.u8();
        std::vector<uint64_t> opts(n);
        for (int i = 0; i < n; ++i) opts[i] = m.u64();
        int idx = pick(rng, n, attempt);
        resp_i(static_cast<int32_t>(idx >= 0 ? opts[idx] : 0));   // response is a card code
        return false;
    }
    case MSG_ANNOUNCE_NUMBER: {
        m.u8();
        int n = m.u8();
        m.skip(8 * n);
        resp_i(pick(rng, n, attempt));                 // response is an index
        return false;
    }
    case MSG_WIN: {
        int player = m.u8();
        m.u8();
        g_winner = player;
        return true;
    }
    default:
        return false;
    }
}
// skip payloads of non-SELECT messages so we can find the prompt
static bool skip_message(const uint8_t type, Msg& m) {
    switch (type) {
    case MSG_HINT: case MSG_PLAYER_HINT: m.u8(); m.u8(); m.u64(); return true;   // hint: type,player,data; player_hint: player,type,data
    case MSG_CONFIRM_DECKTOP: case MSG_CONFIRM_EXTRATOP: { m.u8(); int c = static_cast<int>(m.u32()); m.skip(4 * c); return true; }
    case MSG_CONFIRM_CARDS: { m.u8(); int c = static_cast<int>(m.u32()); m.skip(10 * c); return true; }
    case MSG_SHUFFLE_DECK: {
        const int p = m.u8();
        // Under --trace, say that a Deck was shuffled.
        if (g_trace)
            std::fprintf(stderr, "[trace] shuffle deck p%d\n", p);
        return true;
    }
    case MSG_SWAP_GRAVE_DECK: m.u8(); return true;
    case MSG_SHUFFLE_HAND: case MSG_SHUFFLE_EXTRA: { m.u8(); int c = static_cast<int>(m.u32()); m.skip(4 * c); return true; }
    case MSG_SHUFFLE_SET_CARD: { m.u8(); int c = static_cast<int>(m.u32()); m.skip(6 * c); return true; }
    case MSG_DECK_TOP: m.skip(1 + 4 + 4 + 4); return true;          // player, count(0), code, position
    case MSG_NEW_TURN: m.u8(); return true;
    case MSG_NEW_PHASE: m.u16(); return true;
    case MSG_MOVE: m.u32(); m.skip(20); m.u32(); return true;        // code, loc x2 (10B each), reason
    case MSG_POS_CHANGE: m.u32(); m.skip(5); return true;
    case MSG_SET: m.u32(); m.skip(10); return true;
    case MSG_SWAP: m.skip(28); return true;
    case MSG_FIELD_DISABLED: m.u32(); return true;
    case MSG_SUMMONING: case MSG_SPSUMMONING: case MSG_FLIPSUMMONING:
        m.u32(); m.skip(10); return true;
    case MSG_CHAINING: m.u32(); m.skip(10); m.u8(); m.u8(); m.u32(); m.u64(); m.u32(); return true;
    case MSG_RANDOM_SELECTED: { m.u8(); int c = static_cast<int>(m.u32()); m.skip(10 * c); return true; }
    case MSG_CARD_SELECTED: case MSG_BECOME_TARGET: { int c = static_cast<int>(m.u32()); m.skip(10 * c); return true; }
    case MSG_DRAW: { m.u8(); int c = static_cast<int>(m.u32()); m.skip(8 * c); return true; }
    case MSG_DAMAGE: case MSG_RECOVER: case MSG_LPUPDATE: case MSG_PAY_LPCOST:
        m.u8(); m.u32(); return true;
    case MSG_EQUIP: case MSG_CARD_TARGET: case MSG_CANCEL_TARGET:
        m.skip(20); return true;
    case MSG_ADD_COUNTER: case MSG_REMOVE_COUNTER: m.u16(); m.u8(); m.u8(); m.u8(); m.u16(); return true;
    case MSG_ATTACK: m.skip(20); return true;
    case MSG_BATTLE: m.skip(10); m.u32(); m.u32(); m.u8(); m.skip(10); m.u32(); m.u32(); m.u8(); return true;
    case MSG_TOSS_COIN: case MSG_TOSS_DICE: { m.u8(); int c = m.u8(); m.skip(c); return true; }
    case MSG_HAND_RES: m.u8(); return true;
    case MSG_CARD_HINT: m.skip(10); m.u8(); m.u64(); return true;
    case MSG_MATCH_KILL: m.u8(); return true;
    case MSG_REMOVE_CARDS: { int c = static_cast<int>(m.u32()); m.skip(10 * c); return true; }
    case MSG_MISSED_EFFECT: m.skip(14); return true;   // loc_info (10) + code (4)
    case MSG_REVERSE_DECK: case MSG_SUMMONED: case MSG_SPSUMMONED: case MSG_FLIPSUMMONED:
    case MSG_CHAINED: case MSG_CHAIN_SOLVING: case MSG_CHAIN_SOLVED: case MSG_CHAIN_END:
    case MSG_CHAIN_NEGATED: case MSG_CHAIN_DISABLED: case MSG_ATTACK_DISABLED:
    case MSG_DAMAGE_STEP_START: case MSG_DAMAGE_STEP_END:
        return true;  // no payload
    case MSG_RELOAD_FIELD:
        return true;  // handled (parsed) in update_field; variable length
    default:
        // Unknown message type. The outer loop advances past it via the
        // length prefix (`p = msg_end`), so skipping is always safe here —
        // aborting the whole duel on an unhandled notification (e.g. a hint or
        // selection echo) is what caused mid-game soft-locks. Warn so new
        // message types still surface during development.
        std::fprintf(stderr, "skipping unhandled message type %d\n", static_cast<int>(type));
        return true;
    }
}

// ---------------------------------------------------------------------------
// field model: parse non-SELECT messages to track the board, and score
// policy decisions.  Called for every message (including SELECTs) so the
// model stays current.
// ---------------------------------------------------------------------------
static void update_field(uint8_t type, Msg& m) {
    switch (type) {
    case MSG_HINT: {
        uint8_t htype = m.u8();
        uint8_t player = m.u8();
        uint64_t data = m.u64();
        if (htype == 3)          // HINT_SELECTMSG: remember for SELECT context
            g_last_hint_selectmsg = data;
        return;
    }
    case MSG_RELOAD_FIELD: {
        // scenario setup dump; the runner rebuilds the field model from the
        // scenario JSON instead of parsing this (layout is rule-dependent)
        return;
    }
    case MSG_POS_CHANGE: {
        uint32_t code = m.u32();
        uint8_t ctrl = m.u8(); uint8_t loc = m.u8(); uint8_t seq8 = m.u8();
        uint32_t seq = seq8;
        uint8_t prev = m.u8(); uint8_t cur = m.u8();
        auto it = g_field.zone[ctrl].find({loc, seq});
        if (it != g_field.zone[ctrl].end()) {
            it->second.pos = cur;
            it->second.faceup = (cur & 0x5) != 0;   // attack | defense face-up bits
            if (it->second.faceup) it->second.known = true;  // revealed
        } else {
            FCard c; c.code = code; c.ctrl = ctrl; c.loc = loc; c.seq = seq; c.pos = cur;
            card_stats(code, c.atk, c.def);
            c.faceup = (cur & 0x5) != 0;
            c.known = c.faceup;
            g_field.zone[ctrl][{loc, seq}] = c;
        }
        return;
    }
    case MSG_SET: {
        uint32_t code = m.u32();
        uint8_t ctrl = m.u8(); uint8_t loc = m.u8(); uint32_t seq = m.u32(); uint32_t pos = m.u32();
        FCard c; c.code = code; c.ctrl = ctrl; c.loc = loc; c.seq = seq; c.pos = pos;
        card_stats(code, c.atk, c.def);
        c.faceup = false;
        // Setting a card face-down does NOT reveal its identity to the
        // opponent — the runner knows the code (the message carries it), but
        // the AI only gets to remember cards it has actually SEEN (revealed,
        // flipped face-up, or activated).
        c.known = false;
        g_field.zone[ctrl][{loc, seq}] = c;
        return;
    }
    case MSG_SUMMONING: case MSG_SPSUMMONING: case MSG_FLIPSUMMONING: {
        uint32_t code = m.u32();
        uint8_t ctrl = m.u8(); uint8_t loc = m.u8(); uint32_t seq = m.u32(); uint32_t pos = m.u32();
        FCard c; c.code = code; c.ctrl = ctrl; c.loc = loc; c.seq = seq; c.pos = pos;
        card_stats(code, c.atk, c.def);
        c.faceup = (pos & 0x5) != 0;   // attack | defense face-up bits
        c.known = true;   // summoning reveals the card
        g_field.zone[ctrl][{loc, seq}] = c;
        if (loc == 0x04 && ctrl < 2 && seq < 8)
            g_last_mzone_code[ctrl][seq] = code;
        return;
    }
    case MSG_DRAW: {
        uint8_t player = m.u8();
        int n = static_cast<int>(m.u32());
        for (int i = 0; i < n; ++i) {
            uint32_t code = m.u32();
            m.u32();   // position
            if (static_cast<int>(g_opening[player].size()) < 5)
                g_opening[player].push_back(code);
            if (player < 2)
                g_field.hand[player].push_back(code);
        }
        g_field.hand_count[player] += n;
        return;
    }
    case MSG_MOVE: {
        // code u32, prev loc_info (10B), new loc_info (10B), reason u32
        uint32_t code = m.u32();
        uint8_t pc = m.u8(); uint8_t pl = m.u8(); uint32_t ps = m.u32(); uint32_t pp = m.u32();
        uint8_t nc = m.u8(); uint8_t nl = m.u8(); uint32_t ns = m.u32(); uint32_t np = m.u32();
        m.u32();   // reason
        // Remove the card from whichever tracked list it left.  This is the
        // mirror of the destination handling below and keeps the policy's
        // lightweight field model in sync (the UI snapshot, however, is built
        // from the core directly, so a miss here can never corrupt what the
        // human sees).
        if (pl == 0x04 /* MZONE */ || pl == 0x08 /* SZONE */) {
            auto it = g_field.zone[pc].find({pl, ps});
            if (it != g_field.zone[pc].end()) {
                g_field.zone[pc].erase(it);
                if (pl == 0x04 && pc < 2 && ps < 8) g_last_mzone_code[pc][ps] = 0;
            }
        } else if (pl == 0x02 /* HAND */ && pc < 2) {
            auto& hv = g_field.hand[pc];
            for (auto it = hv.begin(); it != hv.end(); ++it) {
                if (*it == code) {
                    hv.erase(it);
                    if (g_field.hand_count[pc] > 0) --g_field.hand_count[pc];
                    break;
                }
            }
        } else if (pl == 0x10 /* GRAVE */ && pc < 2) {
            auto& gv = g_field.grave[pc];
            for (auto it = gv.begin(); it != gv.end(); ++it) {
                if (*it == code) { gv.erase(it); break; }
            }
        }
        // re-add if it lands on a tracked zone
        if (nl == 0x04 || nl == 0x08) {
            FCard c; c.code = code; c.ctrl = nc; c.loc = nl; c.seq = ns; c.pos = np;
            card_stats(code, c.atk, c.def);
            c.faceup = (np & 0x5) != 0;   // attack | defense face-up bits
            c.known = c.faceup;   // revealed only if it lands face-up
            g_field.zone[nc][{nl, ns}] = c;
            if (nl == 0x04 && nc < 2 && ns < 8) g_last_mzone_code[nc][ns] = code;
        } else if (nl == 0x10 /* GRAVE */ && nc < 2) {
            g_field.grave[nc].push_back(code);
        } else if (nl == 0x02 /* HAND */ && nc < 2) {
            g_field.hand[nc].push_back(code);
            g_field.hand_count[nc]++;
        }
        return;
    }
    case MSG_LPUPDATE: case MSG_DAMAGE: case MSG_RECOVER: case MSG_PAY_LPCOST: {
        uint8_t player = m.u8();
        int32_t delta = static_cast<int32_t>(m.u32());
        if (type == MSG_LPUPDATE)
            g_field.lp[player] = delta;
        else if (type == MSG_DAMAGE || type == MSG_PAY_LPCOST)
            g_field.lp[player] -= delta;
        else
            g_field.lp[player] += delta;
        return;
    }
    case MSG_ATTACK: {
        // attacker loc_info, target loc_info
        uint8_t ac = m.u8(); uint8_t al = m.u8(); uint32_t aseq = m.u32(); m.u32();
        uint8_t tc = m.u8(); uint8_t tl = m.u8(); uint32_t tseq = m.u32(); m.u32();
        (void)ac; (void)al; (void)aseq; (void)tc; (void)tl; (void)tseq;
        return;
    }
    case MSG_BATTLE: {
        m.skip(10); m.u32(); m.u32(); m.u8();
        m.skip(10); m.u32(); m.u32(); m.u8();
        return;
    }
    case MSG_SHUFFLE_HAND: case MSG_SHUFFLE_EXTRA: case MSG_SHUFFLE_DECK:
    case MSG_REVERSE_DECK: case MSG_NEW_TURN: case MSG_NEW_PHASE:
    case MSG_CONFIRM_DECKTOP: case MSG_CONFIRM_EXTRATOP: case MSG_CONFIRM_CARDS:
    case MSG_REMOVE_CARDS: case MSG_RANDOM_SELECTED: case MSG_BECOME_TARGET:
    // equip/target relationships are read from the query API instead (see
    // query_location: QUERY_EQUIP_CARD / QUERY_TARGET_CARD), which reports them
    // for live cards rather than as a message that has to be kept in sync
    case MSG_EQUIP: case MSG_CARD_TARGET: case MSG_CANCEL_TARGET:
    case MSG_TOSS_COIN: case MSG_TOSS_DICE: case MSG_HAND_RES:
    case MSG_CARD_HINT: case MSG_PLAYER_HINT: case MSG_MATCH_KILL:
    case MSG_DECK_TOP: case MSG_SWAP: case MSG_FIELD_DISABLED:
    case MSG_SUMMONED: case MSG_SPSUMMONED: case MSG_FLIPSUMMONED:
    case MSG_CHAINING: case MSG_CHAINED: case MSG_CHAIN_SOLVING:
    case MSG_CHAIN_SOLVED: case MSG_CHAIN_END: case MSG_CHAIN_NEGATED:
    case MSG_CHAIN_DISABLED: case MSG_ATTACK_DISABLED:
    case MSG_DAMAGE_STEP_START: case MSG_DAMAGE_STEP_END:
    case MSG_ADD_COUNTER: case MSG_REMOVE_COUNTER: case MSG_MISSED_EFFECT:
        return;  // no board change we track (or payloads we skip wholesale)
    default:
        return;
    }
}

// choose the best attack target among candidate codes given attacker stats.
// targets come from SELECT_CARD with hint ATTACKTARGET(549): return index.
static int best_attack_target(const std::vector<uint32_t>& codes,
                              const std::vector<uint32_t>& seqs,
                              const std::vector<uint32_t>& poss,
                              uint32_t attacker_code, const int32_t attacker_atk) {
    int best = -1;
    int32_t best_score = -1000000;
    for (size_t i = 0; i < codes.size(); ++i) {
        int32_t tatk = 0, tdef = 0;
        card_stats(codes[i], tatk, tdef);
        uint32_t pos = poss[i];
        bool faceup = (pos & 0x5) != 0;
        bool defpos = (pos & 0x4) != 0 || (pos & 0x8) != 0; // DEF positions
        int32_t relevant = (!faceup || defpos) ? tdef : tatk;
        int32_t score;
        if (attacker_atk > relevant)
            score = 1000 + (attacker_atk - relevant) * 10;  // destroy it
        else if (attacker_atk == relevant)
            score = 500 - relevant;                          // trade
        else
            score = -(relevant - attacker_atk) * 10;         // we die
        if (score > best_score) { best_score = score; best = static_cast<int>(i); }
    }
    return best;
}

// generic selection helper: score candidate cards for a policy-driven pick.
// Returns best index, or -1 for random fallback.
static int policy_pick(const std::vector<uint32_t>& codes, const Policy& pol,
                       const uint64_t hint, std::mt19937& rng, int attempt, bool& used) {
    used = false;
    if (codes.empty())
        return -1;
    // attack target selection
    if (hint == 549 && g_attack_pending) {
        std::vector<uint32_t> seqs(codes.size(), 0), poss(codes.size(), 0);
        // seqs/poss not available generically; fall through to dump scoring
    }
    // tribute/release: prefer tribute_score, then dump
    if (hint == 531 || hint == 500 || hint == 501) {
        std::vector<std::pair<int, int>> scored;
        for (size_t i = 0; i < codes.size(); ++i) {
            int s = pol.tribute_score(codes[i]) + pol.dump_score(codes[i])
                    - pol.keep_score(codes[i]) * 2;
            scored.push_back({s, static_cast<int>(i)});
        }
        std::stable_sort(scored.begin(), scored.end(),
                         [](auto& a, auto& b) { return a.first > b.first; });
        used = true;
        return scored[0].second;
    }
    // generic target: prefer dump (GY enablers), avoid keep
    {
        std::vector<std::pair<int, int>> scored;
        for (size_t i = 0; i < codes.size(); ++i) {
            int s = pol.dump_score(codes[i]) - pol.keep_score(codes[i]) * 2;
            scored.push_back({s, static_cast<int>(i)});
        }
        std::stable_sort(scored.begin(), scored.end(),
                         [](auto& a, auto& b) { return a.first > b.first; });
        used = true;
        return scored[0].second;
    }
}

// ---------------------------------------------------------------------------
// unscripted-card reporting: any deck card without a Lua script is flagged so
// we never silently pretend an effect exists.
// ---------------------------------------------------------------------------
static std::string card_name(const uint32_t code) {
    static sqlite3_stmt* st = nullptr;
    if (!st)
        sqlite3_prepare_v2(g_db, "SELECT name FROM texts WHERE id = ?", -1, &st, nullptr);
    sqlite3_reset(st);
    sqlite3_bind_int(st, 1, static_cast<int>(code));
    if (sqlite3_step(st) == SQLITE_ROW)
        return std::string((const char*)sqlite3_column_text(st, 0));
    return "?";
}

static std::string card_name_ui(const uint32_t code) { return card_name(code); }

static bool script_exists(const uint32_t code) {
    char path[128];
    std::snprintf(path, sizeof path, "./script/c%u.lua", code);
    std::ifstream f(path);
    return f.good();
}

static void report_unscripted(const Deck& a, const Deck& b) {
    std::vector<uint32_t> codes = a.main;
    codes.insert(codes.end(), a.extra.begin(), a.extra.end());
    codes.insert(codes.end(), b.main.begin(), b.main.end());
    codes.insert(codes.end(), b.extra.begin(), b.extra.end());
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
    int n = 0;
    for (uint32_t code : codes) {
        if (!script_exists(code)) {
            std::fprintf(stderr, "[unscripted] %u %s\n", code, card_name(code).c_str());
            ++n;
        }
    }
    if (n)
        std::fprintf(stderr, "[unscripted] %d card(s) have no script and will play as effect-less\n", n);
}

// ---------------------------------------------------------------------------
// one duel (EDOPro core API)
// ---------------------------------------------------------------------------
static bool run_game(const Deck& a, const Deck& b, uint64_t seed, int game_idx,
                     int max_steps, uint64_t flags, intptr_t* out_pduel) {
    OCG_DuelOptions opts{};
    opts.seed[0] = seed + static_cast<uint64_t>(game_idx) * 1000003ull;
    opts.seed[1] = seed >> 1;
    opts.seed[2] = seed * 2654435761ull;
    opts.seed[3] = ~opts.seed[2];
    opts.flags = flags;
    opts.team1 = {8000, 5, 1};
    opts.team2 = {8000, 5, 1};
    opts.cardReader = card_reader_cb;
    opts.scriptReader = script_reader_cb;
    opts.logHandler = log_handler_cb;
    OCG_Duel duel = nullptr;
    g_bootstrapped = false;   // each duel has a fresh Lua state; re-bootstrap it
    if (OCG_CreateDuel(&duel, &opts) != OCG_DUEL_CREATION_SUCCESS) {
        std::fprintf(stderr, "OCG_CreateDuel failed\n");
        return false;
    }
    *out_pduel = (intptr_t)duel;
    g_duel = (intptr_t)duel;
    auto load = [&](const Deck& d, const uint8_t team) {
        for (size_t i = 0; i < d.main.size(); ++i) {
            OCG_NewCardInfo info{team, 0, d.main[i], team, LOCATION_DECK, static_cast<uint32_t>(i), 0x8};
            OCG_DuelNewCard(duel, &info);
        }
        for (size_t i = 0; i < d.extra.size(); ++i) {
            OCG_NewCardInfo info{team, 0, d.extra[i], team, LOCATION_EXTRA, static_cast<uint32_t>(i), 0x8};
            OCG_DuelNewCard(duel, &info);
        }
    };
    if (!g_scenario.empty()) {
        // scenario mode: the Debug.* script builds the whole board first
        // (ReloadFieldBegin wipes the field, so it must run before decks),
        // then decks are loaded on top so the duel can start normally.
        std::string s = read_file(g_scenario.c_str());
        if (s.empty()) {
            std::fprintf(stderr, "cannot read scenario %s\n", g_scenario.c_str());
            return false;
        }
        harness_parse(s);     // honor "-- harness:" fast-forward marker
        if (OCG_LoadScript(duel, s.data(), static_cast<uint32_t>(s.size()), g_scenario.c_str()))
            std::fprintf(stderr, "[scenario] loaded %s\n", g_scenario.c_str());
        else {
            std::fprintf(stderr, "[scenario] FAILED to load %s\n", g_scenario.c_str());
            return false;
        }
        if (!g_scenario_state.empty()) {
            if (load_scenario_state(g_scenario_state))
                std::fprintf(stderr, "[scenario] state injected from %s\n", g_scenario_state.c_str());
            else
                std::fprintf(stderr, "[scenario] WARNING: could not load state %s\n", g_scenario_state.c_str());
        }
    }
    // decks always load (real decks, or empty in pure-scenario mode); they are
    // NOT wiped by ReloadFieldBegin because that ran above
    {
        std::mt19937 deck_rng(static_cast<uint32_t>(seed + (uint64_t) game_idx * 7919ull + 17u));
        Deck sa = a, sb = b;
        std::shuffle(sa.main.begin(), sa.main.end(), deck_rng);
        std::shuffle(sb.main.begin(), sb.main.end(), deck_rng);
        load(sa, 0);
        load(sb, 1);
    }
    if (g_verbose)
        std::fprintf(stderr, "[decks] a main=%zu extra=%zu b main=%zu extra=%zu\n",
                     a.main.size(), a.extra.size(), b.main.size(), b.extra.size());
    {
        int32_t d0 = OCG_DuelQueryCount(duel, 0, LOCATION_DECK);
        int32_t d1 = OCG_DuelQueryCount(duel, 1, LOCATION_DECK);
        int32_t e0 = OCG_DuelQueryCount(duel, 0, LOCATION_EXTRA);
        int32_t e1 = OCG_DuelQueryCount(duel, 1, LOCATION_EXTRA);
        if (g_verbose)
            std::fprintf(stderr, "[query] p0 deck=%d extra=%d | p1 deck=%d extra=%d\n", d0, e0, d1, e1);
    }
    OCG_StartDuel(duel);

    g_winner = -1;
    g_win_reason = 0;
    g_final_lp[0] = g_final_lp[1] = 0;
    g_final_ca[0] = g_final_ca[1] = 0;
    g_turns = 0;
    g_steps = 0;
    g_retries = 0;
    g_assessed = false;
    g_assessed_count = 0;
    g_end_emitted = false;
    g_field.clear();
    if (!g_scenario_state.empty())
        load_scenario_state(g_scenario_state);   // re-apply after clear()
    g_last_hint_selectmsg = 0;
    g_attacker_code = 0;
    g_attacker_atk = 0;
    g_attack_pending = false;
    g_opening[0].clear();
    g_opening[1].clear();
    log_reset();
    std::mt19937 rng(static_cast<uint32_t>(seed + (uint64_t) game_idx * 7919ull));

    std::vector<uint8_t> buf;
    buf.resize(0x100000);
    while (g_steps++ < max_steps) {
        int status = OCG_DuelProcess(duel);
        uint32_t len = 0;
        const uint8_t* msg = static_cast<const uint8_t *>(OCG_DuelGetMessage(duel, &len));
        if (g_verbose)
            std::fprintf(stderr, "[%d] status=%d len=%u\n", g_steps, status, len);
        std::memcpy(buf.data(), msg, len);
        const uint8_t* p = buf.data();
        const uint8_t* end = p + len;
        bool responded = false;
        bool done = false;
        // ML recorder: a decision from the previous process() call is accepted
        // unless this batch starts with MSG_RETRY (we re-answer below and the
        // pending record is refreshed instead of flushed).
        {
            uint8_t first_type = 0xff;
            if (len >= 8) {
                uint32_t m0 = 0;
                std::memcpy(&m0, buf.data(), 4);
                if (m0 >= 1 && 4 + m0 <= len)
                    first_type = buf[4];
            }
            if (g_ml_pend.active && first_type != MSG_RETRY)
                ml_flush();
        }
        // The EDOPro core length-prefixes each message: [u32 len][bytes]...
        while (p < end) {
            if (end - p < 4) {
                std::fprintf(stderr, "truncated length prefix, aborting game\n");
                return false;
            }
            uint32_t mlen = 0;
            std::memcpy(&mlen, p, 4);
            p += 4;
            if (mlen == 0 || p + mlen > end) {
                std::fprintf(stderr, "bad message length %u, aborting game\n", mlen);
                return false;
            }
            const uint8_t* msg_start = p;
            const uint8_t* msg_end = p + mlen;
            uint8_t type = *p;
            if (g_verbose)
                std::fprintf(stderr, "   msg %d len=%u\n", static_cast<int>(type), mlen);
            Msg m(p);
            m.u8();
            // keep the field model current with every message (selects included)
            {
                Msg mf(msg_start);
                mf.u8();
                update_field(type, mf);
                if (g_interactive) {
                    Msg ml(msg_start);
                    ml.u8();
                    log_game_event(type, ml);
                    Msg ma(msg_start); // separate cursor for animation  hint

                    ma.u8();
                    anim_game_event(type, ma);
                }
            }
            // keep the current-phase global fresh in EVERY mode (headless ML
            // runs and decision records included).  log_game_event above only
            // runs for interactive play, so without this the recorded phase
            // stuck at Main Phase 1 for the whole turn — battle and Main Phase
            // 2 prompts were mislabelled.  Idempotent with log_game_event's own
            // updates in interactive mode.
            if (type == MSG_NEW_PHASE) {
                Msg mp(msg_start);
                mp.u8();
                g_current_phase = mp.u16();
                // harness handoff on the phase transition itself — needed for
                // phases that never produce a decision prompt (End Phase etc.)
                if (g_h_ff && g_turns == g_h_turn && g_turn_player == g_h_player
                    && g_current_phase == g_h_phase) {
                    g_h_ff = false;
                    std::fprintf(stderr,
                                 "[harness] reached target turn=%d player=%d "
                                 "phase=0x%x (phase entry) — recording starts now\n",
                                 g_h_turn, g_h_player, g_h_phase);
                }
            } else if (type == MSG_DAMAGE_STEP_START) {
                g_current_phase = 0x20;
            } else if (type == MSG_DAMAGE_STEP_END) {
                g_current_phase = 0x10;
            }
            // reveal/confirm: surface the revealed cards to the player who sees
            // them (e.g. "reveal your hand", "look at the top of the deck")
            if (g_interactive
                && (type == MSG_CONFIRM_CARDS || type == MSG_CONFIRM_DECKTOP || type == MSG_CONFIRM_EXTRATOP)) {
                Msg mr(msg_start);
                mr.u8();
                uint8_t viewer = mr.u8();
                uint32_t count = mr.u32();
                std::ostringstream ro;
                ro << "{\"ev\":\"reveal\",\"player\":" << static_cast<int>(viewer)
                   << ",\"turn\":" << g_turns << ",\"cards\":[";
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t code = mr.u32();
                    uint8_t ctrl = mr.u8(); uint8_t loc = mr.u8(); mr.u32();
                    if (i) ro << ",";
                    ro << "{\"code\":" << code << ",\"name\":" << json::str(card_name_ui(code))
                       << ",\"p\":" << static_cast<int>(ctrl) << ",\"loc\":" << static_cast<int>(loc) << "}";
                }
                ro << "]}";
                emit_line(ro.str());
            }
            if ((type >= MSG_SELECT_BATTLECMD && type <= MSG_SELECT_UNSELECT_CARD)
                || type == MSG_ROCK_PAPER_SCISSORS
                || (type >= MSG_ANNOUNCE_RACE && type <= MSG_ANNOUNCE_NUMBER)) {
                g_last_prompt.assign(msg_start, msg_end);
                // forced-move directives take priority over everything
                if (harness_try_move((intptr_t)duel, type, msg_start, mlen)) {
                    responded = true;
                    g_retries = 0;
                    p = msg_end;
                    break;
                }
                // harness fast-forward: until the authored target state is
                // reached, answer neutrally (no model, no recordings) so the
                // AI is FORCED into the situation instead of being waited for.
                if (g_h_ff) {
                    if (g_turns == g_h_turn && g_turn_player == g_h_player
                        && g_current_phase == g_h_phase
                        && harness_entry_prompt(type, g_h_phase)) {
                        g_h_ff = false;
                        std::fprintf(stderr,
                                     "[harness] reached target turn=%d player=%d "
                                     "phase=0x%x — recording starts now\n",
                                     g_h_turn, g_h_player, g_h_phase);
                    } else if (harness_auto_respond((intptr_t)duel, type,
                                                    msg_start, mlen)) {
                        responded = true;
                        g_retries = 0;
                        p = msg_end;
                        break;
                    }
                    // unhandled prompt family: fall through to the normal
                    // responder below (still no recordings while g_h_ff)
                }
                // human_handle_select returns true when it handled the prompt
                // (and set a response); false means it doesn't know this
                // SELECT type, so fall back to the AI responder.  Without this
                // fallback an unhandled prompt (e.g. synchro material
                // MSG_SELECT_UNSELECT_CARD) would spin on MSG_RETRY forever.
                bool human_handled = false;
                if (g_interactive) {
                    // player id is the first payload byte for most SELECTs
                    uint8_t player = (mlen > 1) ? msg_start[1] : 0;
                    // assess mode: auto-play everyone via policy; multiplayer:
                    // both seats are human; single-human: only player 0 is human
                    if (!g_assess && (g_multiplayer || player == 0)) {
                        human_handled = human_handle_select((intptr_t)duel, type,
                                                            msg_start, static_cast<int>(mlen), rng);
                    }
                }
                // ML recorder: fresh decision record (obs = deciding player's
                // own pre-decision view) MUST start before handle_select so
                // in-prompt label-search can attach its result; the policy's
                // response is captured afterwards.
                if (g_ml_out && !g_interactive && !g_assess && !g_h_ff) {
                    ml_pend_begin(msg_start, mlen, (mlen > 1) ? msg_start[1] : 0);
                }
                if (!human_handled) {
                    if (handle_select((intptr_t)duel, type, msg_start, static_cast<int>(mlen), rng))
                        done = true;
                }
                if (g_ml_out && !g_interactive && !g_assess && !g_h_ff) {
                    ml_pend_resp();
                }
                responded = true;
                g_retries = 0;
                p = msg_end;
                break;
            } else if (type == MSG_RETRY) {
                ++g_retries;
                if (g_retries > 200) {
                    std::fprintf(stderr, "retry loop (>200) on prompt %d, aborting game\n",
                                 g_last_prompt.empty() ? -1 : g_last_prompt[0]);
                    return false;
                }
                if (!g_last_prompt.empty()
                    && harness_try_move((intptr_t)duel, g_last_prompt[0],
                                        g_last_prompt.data(),
                                        static_cast<uint32_t>(g_last_prompt.size()))) {
                    g_retries = 0;
                    p = msg_end;
                    break;
                }
                if (!g_last_prompt.empty()) {
                    bool ff_handled = false;
                    if (g_h_ff)
                        ff_handled = harness_auto_respond(
                            (intptr_t)duel, g_last_prompt[0], g_last_prompt.data(),
                            static_cast<uint32_t>(g_last_prompt.size()));
                    if (!ff_handled) {
                        bool human_handled = false;
                        if (g_interactive && !g_assess && g_last_prompt.size() > 1
                            && (g_multiplayer || g_last_prompt[1] == 0)) {
                            human_handled = human_handle_select((intptr_t)duel, g_last_prompt[0],
                                                                g_last_prompt.data(),
                                                                static_cast<int>(g_last_prompt.size()), rng);
                        }
                        if (!human_handled)
                            handle_select((intptr_t)duel, g_last_prompt[0], g_last_prompt.data(),
                                          static_cast<int>(g_last_prompt.size()), rng);
                    }
                    if (g_ml_out && !g_interactive && !g_assess && !g_h_ff)
                        ml_pend_resp();   // keep the pending record on the re-answer
                }
                p = msg_end;
                break;
            } else if (type == MSG_WIN) {
                g_winner = m.u8();
                g_win_reason = m.u8();
                std::fprintf(stderr, "[win] winner=%d reason=%d\n", g_winner, g_win_reason);
                capture_end_state(duel);
                if (g_interactive) {
                    emit_state();   // final board snapshot before the end event
                    std::ostringstream o;
                    o << "{\"ev\":\"end\",\"winner\":" << g_winner
                      << ",\"reason\":" << g_win_reason << ",\"turns\":" << g_turns << "}";
                    emit_line(o.str());
                    g_end_emitted = true;
                }
                done = true;
                p = msg_end;
                break;
            } else if (type == MSG_NEW_TURN) {
                ++g_turns;
                g_turn_player = m.u8();
                p = msg_end;
                if (g_max_turns > 0 && g_turns > g_max_turns) {
                    capture_end_state(duel);
                    // scenario ran its N turns (through the end phase); stop
                    // here at the turn change so the final board shows the
                    // outcome of the AI's decisions, not a deck-out grind
                    if (g_interactive) {
                        emit_state();
                        std::ostringstream o;
                        o << "{\"ev\":\"end\",\"winner\":-1,\"reason\":0,"
                          << "\"capped\":false,\"turn_limit\":true,"
                          << "\"max_turns\":" << g_max_turns
                          << ",\"turns\":" << g_turns
                          << ",\"reports\":" << g_assessed_count << "}";
                        emit_line(o.str());
                        g_end_emitted = true;
                    }
                    return true;
                }
            } else {
                if (!skip_message(type, m)) {
                    std::fprintf(stderr, "unknown message type %d, aborting game\n", static_cast<int>(type));
                    return false;
                }
                p = msg_end;
            }
        }
        if (g_trace) {
            for (int p = 0; p < 2; ++p) {
                std::string mz;
                for (auto& c : g_field.mzone(p))
                    mz += card_name(c.code) + "(" + std::to_string(c.atk) + "," + (c.faceup ? "U" : "D") + ") ";
                std::fprintf(stderr, "[trace] p%d LP=%d hand=%d field: %s\n",
                             p, g_field.lp[p], g_field.hand_count[p], mz.c_str());
            }
        }
        if (done)
            return true;
        if (status == OCG_DUEL_STATUS_END) {
            capture_end_state(duel);
            if (g_interactive && !g_end_emitted) {
                emit_state();   // final board snapshot before the end event
                std::ostringstream o;
                o << "{\"ev\":\"end\",\"winner\":" << g_winner
                  << ",\"reason\":0,\"turns\":" << g_turns
                  << ",\"capped\":false,\"reports\":" << g_assessed_count << "}";
                emit_line(o.str());
                g_end_emitted = true;
            }
            return true;
        }
        if (g_assess_cap > 0 && g_assessed_count >= g_assess_cap) {
            // enough search decisions captured; stop here instead of looping
            // a static board forever (each repeated prompt re-runs a search)
            if (g_interactive) {
                emit_state();   // final board snapshot before the end event
                std::ostringstream o;
                o << "{\"ev\":\"end\",\"winner\":-1,\"reason\":0,"
                  << "\"capped\":true,\"reports\":" << g_assessed_count
                  << ",\"turns\":" << g_turns << "}";
                emit_line(o.str());
                g_end_emitted = true;
            }
            return true;
        }
        (void)responded;
    }
    std::fprintf(stderr, "step limit (%d) reached\n", max_steps);
    return false;
}

// ---------------------------------------------------------------------------
// A cheap content hash (FNV-1a 64) of a deck file: a recording says which decks
// it was made with, so a replay can tell when it is being handed different ones.
static unsigned long long deck_hash(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    unsigned long long h = 1469598103934665603ull;
    if (!f) return 0;
    unsigned char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; ++i) { h ^= buf[i]; h *= 1099511628211ull; }
    std::fclose(f);
    return h;
}

int main(int argc, char** argv) {
    std::string deck1, deck2, out_path, ml_path;
    std::string nn_model, nn_model2;
    std::string nn_script = "ml/predict_server.py", nn_python = "python3";
    int nn_seat_flag = 0, nn_seat2_flag = 1;
    double nn_temp = 0.0;
    std::string policy1, policy2, dump_policy_name, scenario_file, scenario_state_file;
    std::string record_path, replay_path;
    uint64_t seed = 1;
    int games = 1;
    int max_steps = 200000;
    uint64_t flags = EDISON_FLAGS;
    bool no_flags = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--deck1" && i + 1 < argc) deck1 = argv[++i];
        else if (a == "--deck2" && i + 1 < argc) deck2 = argv[++i];
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--games" && i + 1 < argc) games = atoi(argv[++i]);
        else if (a == "--max-steps" && i + 1 < argc) max_steps = atoi(argv[++i]);
        else if (a == "--max-turns" && i + 1 < argc) g_max_turns = atoi(argv[++i]);
        else if (a == "--rollout-turns" && i + 1 < argc) g_rollout_turns = atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--flags" && i + 1 < argc) flags = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--no-flags") no_flags = true;
        else if (a == "--policy1" && i + 1 < argc) policy1 = argv[++i];
        else if (a == "--policy2" && i + 1 < argc) policy2 = argv[++i];
        else if (a == "--dump-policy" && i + 1 < argc) { dump_policy_name = argv[++i]; }
        else if (a == "--scenario" && i + 1 < argc) { scenario_file = argv[++i]; }
        else if (a == "--record-decisions" && i + 1 < argc) { ml_path = argv[++i]; }
        else if (a == "--search-labels" && i + 1 < argc) { g_ml_labels = atoi(argv[++i]); }
        else if (a == "--neural-policy" && i + 1 < argc) { nn_model = argv[++i]; }
        else if (a == "--neural-seat" && i + 1 < argc) { nn_seat_flag = atoi(argv[++i]); }
        else if (a == "--neural-policy2" && i + 1 < argc) { nn_model2 = argv[++i]; }
        else if (a == "--neural-seat2" && i + 1 < argc) { nn_seat2_flag = atoi(argv[++i]); }
        else if (a == "--neural-python" && i + 1 < argc) { nn_python = argv[++i]; }
        else if (a == "--neural-script" && i + 1 < argc) { nn_script = argv[++i]; }
        else if (a == "--neural-temp" && i + 1 < argc) { nn_temp = atof(argv[++i]); }
        else if (a == "--nn-mutate" && i + 1 < argc) { g_nn_mutate = atof(argv[++i]); }
        else if (a == "--rollout-policy" && i + 1 < argc) { g_rollout_model = argv[++i]; }
        else if (a == "--value-model" && i + 1 < argc) { g_value_model = argv[++i]; }
        else if (a == "--cpp-seat" && i + 1 < argc) { g_cpp_seat = atoi(argv[++i]); }
        else if (a == "--nn-dump" && i + 1 < argc) { g_nn_dump_path = argv[++i]; }
        else if (a == "--scenario-state" && i + 1 < argc) { scenario_state_file = argv[++i]; }
        else if (a == "--record" && i + 1 < argc) { record_path = argv[++i]; }
        else if (a == "--replay" && i + 1 < argc) { replay_path = argv[++i]; }
        else if (a == "--verify") { g_verify = true; }
        else if (a == "--search" && i + 1 < argc) { g_search_rollouts = atoi(argv[++i]); }
        else if (a == "--search-seat" && i + 1 < argc) { g_search_seat = atoi(argv[++i]); }
        else if (a == "--assess-cap" && i + 1 < argc) { g_assess_cap = atoi(argv[++i]); }
        else if (a == "--assess") { g_assess = true; g_interactive = true; g_in = stdin; }
        else if (a == "--interactive") { g_interactive = true; g_in = stdin; }
        else if (a == "--multiplayer") { g_multiplayer = true; g_interactive = true; g_in = stdin; }
        else if (a == "--verbose") g_verbose = true;
        else if (a == "--trace") g_trace = true;
    }
    if (no_flags) flags = 0;
    // a replay brings its own seed, so `--replay file` needs no other flags
    if (!replay_path.empty()) {
        uint64_t recorded_seed = 0;
        if (!load_replay(replay_path, &recorded_seed)) {
            std::fprintf(stderr, "cannot read replay %s\n", replay_path.c_str());
            return 2;
        }
        if (recorded_seed) seed = recorded_seed;
        g_replay = true;
        g_interactive = true;
        g_in = stdin;
        std::fprintf(stderr, "[replay] %zu events, %zu answers\n",
                     g_replay_events.size(), g_replay_acts.size());
    }
    g_nn_python = nn_python;
    g_nn_script = nn_script;
    // --dump-policy needs no decks: emit the built-in policy JSON and exit
    if (!dump_policy_name.empty()) {
        if (sqlite3_open("cards.cdb", &g_db) != SQLITE_OK) {
            std::fprintf(stderr, "cannot open cards.cdb\n");
            return 2;
        }
        Policy p = make_policy(dump_policy_name);
        std::string json = policy_to_json(p);
        std::string path = "policies/" + dump_policy_name + ".json";
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 2; }
        std::fprintf(f, "%s\n", json.c_str());
        std::fclose(f);
        std::fprintf(stderr, "wrote %s\n", path.c_str());
        sqlite3_close(g_db);
        return 0;
    }
    if (sqlite3_open("cards.cdb", &g_db) != SQLITE_OK) {
        std::fprintf(stderr, "cannot open cards.cdb\n");
        return 2;
    }
    g_scenario = scenario_file;
    g_scenario_state = scenario_state_file;
    Deck a, b;
    // decks load whenever both are given (scenario mode still wants a draw
    // pile; pure-scenario mode leaves them empty and must avoid deck-out)
    if (!deck1.empty() && !deck2.empty()) {
        if (!parse_ydk(deck1, a) || !parse_ydk(deck2, b)) {
            std::fprintf(stderr, "cannot parse decks\n");
            return 2;
        }
        report_unscripted(a, b);
    }
    g_policy[0] = load_policy_for(policy1);
    g_policy[1] = load_policy_for(policy2);
    std::fprintf(stderr, "[flags] Edison-rulings flags: 0x%llx%s\n",
                 static_cast<unsigned long long>(flags), no_flags ? " (disabled)" : "");
    std::fprintf(stderr, "[policy] p0=%s p1=%s\n",
                 policy1.empty() ? "random" : policy1.c_str(),
                 policy2.empty() ? "random" : policy2.c_str());

    FILE* out = out_path.empty() ? stdout : std::fopen(out_path.c_str(), "w");
    if (!out) { std::fprintf(stderr, "cannot open output\n"); return 2; }

    // ML decision recorder (batch mode only)
    if (!ml_path.empty()) {
        if (g_interactive) {
            std::fprintf(stderr, "[ml] --record-decisions ignored in interactive mode\n");
        } else {
            g_ml_out = std::fopen(ml_path.c_str(), "w");
            if (!g_ml_out) { std::fprintf(stderr, "cannot open %s\n", ml_path.c_str()); return 2; }
            std::fprintf(stderr, "[ml] recording decisions -> %s\n", ml_path.c_str());
        }
    }
    if (g_ml_labels > 0) {
        if (!g_ml_out) {
            std::fprintf(stderr, "[ml] --search-labels needs --record-decisions; ignoring\n");
            g_ml_labels = 0;
        } else if (g_search_rollouts > 0) {
            std::fprintf(stderr, "[ml] --search-labels disabled while --search is active\n");
            g_ml_labels = 0;
        } else {
            std::fprintf(stderr, "[ml] rollout-search labels: %d rollouts per candidate\n", g_ml_labels);
        }
    }

    // neural-policy servers: one python predictor per configured seat
    for (int seat = 0; seat <= 1; ++seat) {
        std::string m;
        int seatflag = -1;
        if (!nn_model.empty() && nn_seat_flag == seat) { m = nn_model; seatflag = seat; }
        if (m.empty() && !nn_model2.empty() && nn_seat2_flag == seat) { m = nn_model2; seatflag = seat; }
        if (m.empty())
            continue;
        if (g_interactive || g_search_rollouts > 0) {
            std::fprintf(stderr, "[nn] --neural-policy ignored with --interactive/--search\n");
        } else {
            nn_spawn(seatflag, nn_python, nn_script, m, nn_temp, g_nn_mutate);
        }
    }

    // in-process champion for search rollouts (--rollout-policy *.bin)
    if (g_rollout_model.size() >= 4
        && g_rollout_model.compare(g_rollout_model.size() - 4, 4, ".bin") == 0) {
        nncpp::g_cpp.load_stats();
        if (nncpp::g_cpp.load(g_rollout_model)) {
            std::fprintf(stderr, "[nn] in-process rollout champion loaded (%s, %d inputs)\n",
                         g_rollout_model.c_str(), nncpp::g_cpp.n_in);
        } else {
            std::fprintf(stderr, "[nn] failed to load %s\n", g_rollout_model.c_str());
        }
    }
    // --cpp-seat N: seat N plays the loaded .bin champion directly (no python,
    // no search) — used by interactive "play vs champion" sessions.  Prompts
    // the model answers (card/tribute/chain/idle/battle) go through g_cpp.pick;
    // everything else falls back to the seat's rule policy.
    if (g_cpp_seat >= 0 && g_cpp_seat <= 1) {
        if (nncpp::g_cpp.loaded) {
            g_cpp_rollout[g_cpp_seat] = true;
            std::fprintf(stderr, "[nn] seat %d plays in-process champion\n", g_cpp_seat);
        } else {
            std::fprintf(stderr, "[nn] --cpp-seat %d but no champion .bin loaded "
                                 "(need --rollout-policy model.bin)\n", g_cpp_seat);
        }
    }
    // value head (--value-model *.bin): P(win) for value-guided search
    if (g_value_model.size() >= 4
        && g_value_model.compare(g_value_model.size() - 4, 4, ".bin") == 0) {
        nncpp::g_value.load_stats();
        if (nncpp::g_value.load(g_value_model)) {
            std::fprintf(stderr, "[nn] value head loaded (%s, %d inputs)\n",
                         g_value_model.c_str(), nncpp::g_value.n_in);
        } else {
            std::fprintf(stderr, "[nn] failed to load value head %s\n",
                         g_value_model.c_str());
        }
    }
    if (!g_nn_dump_path.empty()) {
        nncpp::g_dump = std::fopen(g_nn_dump_path.c_str(), "w");
    }

    if (!record_path.empty()) {
        std::ostringstream h;
        h << R"({"ev":"replay","v":1)"
          << ",\"games\":" << games
          << ",\"seed\":" << seed
          << ",\"multiplayer\":" << (g_multiplayer ? "true" : "false")
          << ",\"interactive\":" << (g_interactive ? "true" : "false")
          << ",\"max_steps\":" << max_steps
          << ",\"flags\":" << flags
          << ",\"deck1\":" << json::str(deck1)
          << ",\"deck2\":" << json::str(deck2)
          << ",\"deck1_hash\":" << deck_hash(deck1)
          << ",\"deck2_hash\":" << deck_hash(deck2)
          // a scenario duel has no decks: the .lua that built the board is what
          // a re-simulation needs, so its hash goes in too
          << ",\"scenario\":" << json::str(scenario_file)
          << ",\"scenario_hash\":" << deck_hash(scenario_file)
          << ",\"started\":" << static_cast<long long>(std::time(nullptr))
          << "}";
        // the caller owns the directory (the server creates REPLAYS_DIR at
        // startup); if it is not writable the open below fails loudly rather
        // than the duel starting without a recording
        if (!start_recording(record_path, h.str())) {
            std::fprintf(stderr, "cannot write record %s\n", record_path.c_str());
            return 2;
        }
    }

    int completed = 0, aborted = 0;
    for (int g = 0; g < games; ++g) {
        g_ml_game = g;
        intptr_t pduel = 0;
        bool ok = run_game(a, b, seed, g, max_steps, flags, &pduel);
        if (ok) completed++; else aborted++;
        if (g_interactive) {
            if (pduel) OCG_DestroyDuel((OCG_Duel)pduel);
            continue;   // interactive protocol already emitted its own events
        }
        std::fprintf(out,
            "{\"game\":%d,\"winner\":%d,\"reason\":%d,\"flp\":[%d,%d],\"fca\":[%d,%d],"
            "\"turns\":%d,\"steps\":%d,\"seed\":%llu,\"ok\":%s,\"opening0\":[",
            g, g_winner, g_win_reason, g_final_lp[0], g_final_lp[1],
            g_final_ca[0], g_final_ca[1],
            g_turns, g_steps, static_cast<unsigned long long>(seed + (uint64_t) g * 1000003ull),
            ok ? "true" : "false");
        for (size_t i = 0; i < g_opening[0].size(); ++i)
            std::fprintf(out, "%s%u", i ? "," : "", g_opening[0][i]);
        std::fprintf(out, "],\"opening1\":[");
        for (size_t i = 0; i < g_opening[1].size(); ++i)
            std::fprintf(out, "%s%u", i ? "," : "", g_opening[1][i]);
        std::fprintf(out, "]}\n");
        if (pduel)
            OCG_DestroyDuel((OCG_Duel)pduel);
    }
    ml_close();
    for (int seat = 0; seat <= 1; ++seat) {
        if (g_nn_pid[seat] <= 0)
            continue;
        if (g_nn_in[seat]) { std::fputs("{\"cmd\":\"exit\"}\n", g_nn_in[seat]); std::fflush(g_nn_in[seat]); }
        if (g_nn_in[seat]) std::fclose(g_nn_in[seat]);
        if (g_nn_out[seat]) std::fclose(g_nn_out[seat]);
        g_nn_in[seat] = g_nn_out[seat] = nullptr;
        kill(g_nn_pid[seat], SIGTERM);
        waitpid(g_nn_pid[seat], nullptr, 0);
    }
    std::fprintf(stderr, "games=%d completed=%d aborted=%d\n", games, completed, aborted);
    if (g_record) {
        std::fclose(g_record);
        g_record = nullptr;
        std::fprintf(stderr, "[record] wrote %s\n", record_path.c_str());
    }
    if (!out_path.empty())
        std::fclose(out);
    sqlite3_close(g_db);
    if (g_verify) {
        // the whole point of a replay: it either is the same game or it is not
        const size_t n = g_replay_events.size();
        if (g_replay_ran_out) {
            std::fprintf(stderr, "[replay] DIVERGED: the replay asked for an answer the recording does not have\n");
            return 3;
        }
        if (g_replay_events_at != n || g_replay_mismatches) {
            std::fprintf(stderr, "[replay] DIVERGED: %zu of %zu events compared, %zu different\n",
                         g_replay_events_at, n, g_replay_mismatches);
            return 3;
        }
        std::fprintf(stderr, "[replay] verified: %zu events identical\n", n);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// search: apply a candidate semantic action for a choice prompt (used inside
// forked children during rollout search).
//   action = {"yesno": 0|1} | {"effectyn": 0|1} | {"chain": index|-1}
//          | {"position": pos} | {"option": index}
// ---------------------------------------------------------------------------
static void apply_search_action(const intptr_t duel, const uint8_t type, const uint8_t* pbuf,
                                int len, const std::string& action) {
    Msg m(pbuf);
    m.u8();
    int32_t val = -1;
    if (type == MSG_SELECT_YESNO || type == MSG_SELECT_EFFECTYN) {
        if (action.find(":1") != std::string::npos || action == "1")
            val = 1;
        else
            val = 0;
    } else if (type == MSG_SELECT_CHAIN) {
        // accept both {"chain": k} and plain "k" / "-1" labels
        size_t p = action.find("chain");
        if (p != std::string::npos) {
            p = action.find(':', p);
            if (p != std::string::npos) val = atoi(action.c_str() + p + 1);
        } else {
            val = atoi(action.c_str());   // "-1" = pass, "k" = activate option k
        }
    } else if (type == MSG_SELECT_POSITION) {
        m.u8(); m.u32(); int positions = m.u8();
        // prefer attack (1), else face-up defense (4), else face-down (8)
        val = (positions & 1) ? 1 : (positions & 4) ? 4 : 8;
    } else if (type == MSG_SELECT_OPTION) {
        val = 0;
    } else if (type == MSG_SELECT_CARD || type == MSG_SELECT_TRIBUTE) {
        // action is a bare card index, or "-1" to cancel (min == 0)
        if (action == "-1") {
            set_response_i(duel, -1);
            return;
        }
        int idx = atoi(action.c_str());
        std::vector<uint8_t> b(8 + 1);
        int32_t t = 2; std::memcpy(b.data(), &t, 4);
        uint32_t c = 1; std::memcpy(b.data() + 4, &c, 4);
        b[8] = static_cast<uint8_t>(idx);
        set_response(duel, b);
        return;
    } else if (type == MSG_SELECT_BATTLECMD || type == MSG_SELECT_IDLECMD) {
        // action JSON: {"action":"attack","index":k} etc. -> t | (s<<16)
        // idlecmd types: summon=0 spsummon=1 repo=2 mset=3 sset=4 act=5
        //                to_bp=6 to_ep=7  (battlecmd: act=0 attack=1 to_m2=2 to_ep=3)
        int t = 7, s = 0;   // default to_ep
        if (action.find("\"action\":\"attack\"") != std::string::npos) { t = 1; }
        else if (action.find("\"action\":\"spsummon\"") != std::string::npos) { t = 1; }
        else if (action.find("\"action\":\"summon\"") != std::string::npos) { t = 0; }
        else if (action.find("\"action\":\"mset\"") != std::string::npos) { t = 3; }
        else if (action.find("\"action\":\"sset\"") != std::string::npos) { t = 4; }
        else if (action.find("\"action\":\"act\"") != std::string::npos) { t = (type == MSG_SELECT_BATTLECMD) ? 0 : 5; }
        else if (action.find("\"action\":\"to_m2\"") != std::string::npos) { t = 2; }
        else if (action.find("\"action\":\"to_bp\"") != std::string::npos) { t = 6; }
        else if (action.find("\"action\":\"to_ep\"") != std::string::npos) { t = (type == MSG_SELECT_BATTLECMD) ? 3 : 7; }
        size_t idx = action.find("\"index\":");
        if (idx != std::string::npos) s = atoi(action.c_str() + idx + 8);
        if (g_verbose)
            std::fprintf(stderr, "  [apply] %s -> t=%d s=%d\n", action.c_str(), t, s);
        set_response_i(duel, t | (s << 16));
        return;
    }
    set_response_i(duel, val);
}
