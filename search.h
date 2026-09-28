// search.h — fork-based Monte Carlo rollout search for the AI.
//
// At an AI decision point the runner has a SELECT prompt (type + raw bytes)
// and a live duel.  Instead of picking one policy answer, we:
//   1. enumerate the candidate actions (the same set the policy sees),
//   2. for each candidate, fork(): the child applies that response, then
//      plays the game to completion with the fast policy for BOTH sides
//      (the opponent also uses a policy), writing "winner 0|1|2" to a pipe,
//   3. the parent aggregates win rates per candidate and picks the best
//      (with a little exploration noise).
//
// fork() gives us copy-on-write snapshots of the mid-game duel for free —
// the core has no snapshot API, but process memory duplication is exactly
// that.  The child must NOT write to shared buffers after fork, so we only
// use the pipe for the result and _exit() immediately after.
#pragma once
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_search_rollouts = 0;   // 0 = no search, policy only
static int g_search_seat = 1;       // which seat the search plays (--search-seat)
static uint64_t g_search_seed = 1;   // seed base for rollouts
static bool g_assessed = false;      // assess mode: a search report was emitted
static int g_assessed_count = 0;     // how many search reports emitted so far
static int g_assess_cap = 0;         // stop the game after this many reports (0 = unlimited)
static bool g_end_emitted = false;   // interactive/assess: end event already sent

static uint64_t seed_of_rng(std::mt19937& rng) {
    (void)rng;
    return g_search_seed++;
}

// fast self-play: keep responding to prompts (both players use policies)
// until the duel ends or the step cap hits.  Returns the winner.
// forward decls (defined in duel_runner_edo.cpp)
static bool handle_select(intptr_t pduel, uint8_t type, const uint8_t* pbuf,
                          int len, std::mt19937& rng);
static void update_field(uint8_t type, Msg& m);
static bool skip_message(uint8_t type, Msg& m);
static void log_game_event(uint8_t type, Msg& m);

static void apply_search_action(intptr_t pduel, uint8_t type, const uint8_t* pbuf,
                                int len, const std::string& action);

// simple position evaluator for horizon-limited rollouts (player 1's view).
// Returns a score in [-100, 100]; positive = good for player 1 (AI).
static double eval_position() {
    int lp1 = g_field.lp[1] - g_field.lp[0];
    double board = 0;
    for (int p = 0; p < 2; ++p) {
        double atk = 0, def = 0;
        for (auto& c : g_field.mzone(p)) {
            atk += c.atk;
            if (c.pos & 0x4) def += c.def;
        }
        // monsters with higher ATK are threats; count face-up ATK monsters
        if (p == 1) board += atk + def * 0.5;
        else board -= atk + def * 0.5;
    }
    double hand_adv = (g_field.hand[1].size() - g_field.hand[0].size()) * 8.0;
    double grave_adv = (g_field.grave[1].size() - g_field.grave[0].size()) * 2.0;
    return lp1 * 0.5 + board * 0.4 + hand_adv + grave_adv;
}

// --- progress (delta) evaluator for short-turn scenarios -------------------
// The absolute evaluator saturates on static boards: "pass and keep my big
// board forever" scores ~100, so the AI never attacks.  For one-turn
// scenario runs we instead score what CHANGED this turn from the decision
// point: damage dealt vs taken, opponent field cards removed, our field
// cards lost.  Passing = no progress = ~50%; a correct attack line that
// deals damage while keeping the board scores higher; walking a monster
// into a known trap scores lower.
static int g_prog_lp0 = 0, g_prog_lp1 = 0;        // LP at decision point
static int g_prog_field0 = 0, g_prog_field1 = 0;  // # field cards (mz+sz) at decision point

static void snapshot_progress() {
    g_prog_lp0 = g_field.lp[0];
    g_prog_lp1 = g_field.lp[1];
    g_prog_field0 = (int)(g_field.mzone(0).size() + g_field.szone(0).size());
    g_prog_field1 = (int)(g_field.mzone(1).size() + g_field.szone(1).size());
}

// delta score in a range where the logistic mapping is informative
static double eval_progress() {
    double opp_lp_lost = std::max(0, g_prog_lp0 - g_field.lp[0]);   // damage dealt to opp
    double my_lp_lost  = std::max(0, g_prog_lp1 - g_field.lp[1]);   // damage taken
    int my_delta = (int)(g_field.mzone(1).size() + g_field.szone(1).size()) - g_prog_field1;
    int opp_delta = (int)(g_field.mzone(0).size() + g_field.szone(0).size()) - g_prog_field0;
    double score = (opp_lp_lost - my_lp_lost) / 40.0   // 2100 damage ≈ +52
                 + my_delta * 25.0                      // summoned/revived our monsters
                 - opp_delta * 25.0;                    // opponent developed their board
    return score;
}

static double g_rollout_score = 0;   // evaluator output when horizon hit
static int g_rollout_turns = 0;      // stop rollouts after N turn changes (0 = step horizon)
static int g_max_turns = 0;          // stop the MAIN game after N turns (0 = unlimited)

// fast self-play with a step horizon; returns winner (-1 if horizon hit and
// the position should be judged by g_rollout_score instead).
// If g_rollout_turns > 0, stop after that many turn changes (each MSG_NEW_TURN
// counts one) and evaluate the position — used for one-turn scenario runs.
static int selfplay_to_end(intptr_t duel, int max_steps, uint64_t seed) {
    std::mt19937 rng((uint32_t)(seed ^ 0x9e3779b97f4a7c15ull));
    g_winner = -1;
    int steps = 0;
    int turn_changes = 0;
    OCG_Duel d = (OCG_Duel)duel;
    while (steps++ < max_steps) {
        int status = OCG_DuelProcess(d);
        uint32_t len = 0;
        const uint8_t* msg = (const uint8_t*)OCG_DuelGetMessage(d, &len);
        const uint8_t* p = msg;
        const uint8_t* end = p + len;
        bool done = false;
        while (p < end) {
            if (end - p < 4) { g_rollout_score = eval_position(); return -1; }
            uint32_t mlen = 0;
            std::memcpy(&mlen, p, 4);
            p += 4;
            if (mlen == 0 || p + mlen > end) { g_rollout_score = eval_position(); return -1; }
            const uint8_t* msg_start = p;
            const uint8_t* msg_end = p + mlen;
            uint8_t type = *p;
            Msg m(p);
            m.u8();
            // update field model (shared copy, fine)
            {
                Msg mf(msg_start); mf.u8(); update_field(type, mf);
                if (g_interactive) { Msg ml(msg_start); ml.u8(); log_game_event(type, ml); }
            }
            if (type == MSG_NEW_TURN) {
                ++turn_changes;
                m.u8();
                p = msg_end;
                if (g_rollout_turns > 0 && turn_changes >= g_rollout_turns) {
                    g_rollout_score = eval_position();
                    return -1;
                }
                continue;
            }
            if ((type >= MSG_SELECT_BATTLECMD && type <= MSG_SELECT_UNSELECT_CARD)
                || type == MSG_ROCK_PAPER_SCISSORS
                || (type >= MSG_ANNOUNCE_RACE && type <= MSG_ANNOUNCE_NUMBER)) {
                handle_select((intptr_t)duel, type, msg_start, (int)mlen, rng);
                p = msg_end;
                break;
            } else if (type == MSG_RETRY) {
                p = msg_end;
                break;
            } else if (type == MSG_WIN) {
                g_winner = m.u8();
                m.u8();
                done = true;
                p = msg_end;
                break;
            } else {
                if (!skip_message(type, m)) { g_rollout_score = eval_position(); return -1; }
                p = msg_end;
            }
        }
        if (done) return g_winner;
        if (status == OCG_DUEL_STATUS_END) return g_winner;
    }
    g_rollout_score = eval_position();
    return -1;
}

// run search over the candidate actions for the current AI prompt.
// `actions` is a list of semantic responses (as JSON strings) the policy
// would send; each is applied in a forked child and rolled out.
// `rollouts` is the total rollout budget; it is spread across candidates
// (at least 4 per candidate, so win rates aren't single-rollout noise).
// Returns the index of the chosen action, or -1 (pass).
struct SearchResult {
    int chosen = -1;
    std::vector<std::pair<int, double>> scores;  // (action idx, winrate 0..100)
    std::vector<std::string> actions;            // candidate action labels
    int rollouts = 0;          // total rollouts actually executed
    int per_candidate = 0;     // rollouts per candidate
    int ties = 0;              // candidates sharing the top winrate (>=1)
    bool ran = false;
};

// maximum children to have in flight at once (parallel forks)
static constexpr int kMaxParallel = 8;

// structured search-report event for the API/UI: candidates with scores.
// For battle prompts, g_report_attackers (if non-empty) carries the attacker
// card codes so the UI can name "Attack with X" instead of "monster 0".
static std::vector<uint32_t> g_report_attackers;
static void emit_search_report(const char* prompt_type, const SearchResult& res) {
    std::ostringstream o;
    double chosen_wr = 0;
    for (auto& sc : res.scores)
        if (sc.first == res.chosen) chosen_wr = sc.second;
    o << "{\"ev\":\"search\",\"type\":\"" << prompt_type << "\",\"chosen\":"
      << res.chosen << ",\"chosen_winrate\":" << chosen_wr
      << ",\"rollouts\":" << res.rollouts
      << ",\"per_candidate\":" << res.per_candidate
      << ",\"ties\":" << res.ties;
    if (!g_report_attackers.empty()) {
        o << ",\"attackers\":[";
        for (size_t i = 0; i < g_report_attackers.size(); ++i)
            o << (i ? "," : "") << g_report_attackers[i];
        o << "]";
    }
    o << ",\"candidates\":[";
    for (size_t i = 0; i < res.actions.size(); ++i) {
        if (i) o << ",";
        double wr = 0;
        for (auto& sc : res.scores)
            if (sc.first == (int)i) wr = sc.second;
        o << "{\"label\":" << json::str(res.actions[i])
          << ",\"winrate\":" << wr << "}";
    }
    o << "]}";
    emit_line(o.str());
    ++g_assessed_count;
}

static SearchResult search_actions(intptr_t duel, uint8_t type, const uint8_t* pbuf,
                                   int len, const std::vector<std::string>& actions,
                                   int rollouts, uint64_t seed) {
    SearchResult out;
    out.ran = true;
    out.actions = actions;
    if (actions.empty())
        return out;
    // spread the budget across candidates, never fewer than 4 per candidate
    // (2/candidate at default 10 rollouts made win rates single-rollout noise).
    // The opponent keeps a random/heuristic policy even when --rollout-policy
    // is set, so rollouts stay stochastic and need the same 4+ repeats.
    int per = std::max(4, rollouts / (int)actions.size());
    out.per_candidate = per;
    out.rollouts = per * (int)actions.size();
    std::vector<double> totals(actions.size(), 0.0);
    std::vector<int> counts(actions.size(), 0);
    // baseline for the progress (delta) evaluator, shared by all children
    snapshot_progress();
    // spawn all (action, rollout) jobs up front, waiting only when we have
    // kMaxParallel live children; this parallelizes the forks across cores
    struct Job { size_t ai; int r; pid_t pid; int fd; };
    std::vector<Job> live;
    auto spawn = [&](size_t ai, int r) {
        int pipefd[2];
        if (pipe(pipefd) != 0) return;
        pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[0]);
            FILE* f = fdopen(pipefd[1], "w");
            g_search_rollouts = 0;      // NO recursive search in rollouts
            g_interactive = false;      // policy-only responses, no stdin
            g_ml_labels = 0;            // NO recursive label-search in rollouts
            g_ml_out = nullptr;         // and no recorder writes from children
            if (g_rollout_model.size() >= 4
                && g_rollout_model.compare(g_rollout_model.size() - 4, 4,
                                           ".bin") == 0) {
                // in-process champion (nn_cpp.h): weights inherited via COW,
                // no Python spawn. Only the SEARCH side plays the champion;
                // the opponent keeps the heuristic/random policy so the
                // rollout matches the real game (champion vs random opponent).
                int sp = pbuf[1];
                g_cpp_rollout[sp] = true;
                g_cpp_rollout[1 - sp] = false;
                g_nn_in[0] = g_nn_in[1] = nullptr;
                g_nn_out[0] = g_nn_out[1] = nullptr;
                g_nn_seat[0] = g_nn_seat[1] = -1;
            } else if (!g_rollout_model.empty()) {
                // rollout policy (prototype): both seats spawn a predict_server
                nn_spawn(0, g_nn_python, g_nn_script, g_rollout_model, 0.0, 0.0);
                nn_spawn(1, g_nn_python, g_nn_script, g_rollout_model, 0.0, 0.0);
            } else {
                for (int _nn = 0; _nn < 2; ++_nn) {
                    g_nn_in[_nn] = nullptr;   // NO neural queries from children
                    g_nn_out[_nn] = nullptr;
                    g_nn_seat[_nn] = -1;
                }
            }
            apply_search_action(duel, type, pbuf, len, actions[ai]);
            int w = selfplay_to_end(duel, 2500,
                                    seed + (uint64_t)r * 2654435761ull + (uint64_t)ai);
            if (w == -1) {
                if (nncpp::g_value.loaded) {
                    // value-guided: evaluate the horizon position with the
                    // value head (P(search side wins) -> 0..100)
                    double v = nncpp::g_value.evaluate(pbuf[1]);
                    std::fprintf(f, "s%.1f\n", v * 100.0);
                } else {
                    // horizon reached: report the evaluator score 0..100
                    double ev = (g_rollout_turns > 0) ? eval_progress() : g_rollout_score;
                    double wr = 100.0 / (1.0 + std::exp(-ev / 40.0));
                    std::fprintf(f, "s%.1f\n", wr);
                }
            } else {
                std::fprintf(f, "w%d\n", w);
            }
            std::fclose(f);
            _exit(0);
        } else if (pid > 0) {
            close(pipefd[1]);
            live.push_back({ai, r, pid, pipefd[0]});
        } else {
            close(pipefd[0]); close(pipefd[1]);
        }
    };
    // wait for the oldest child, collect its result
    auto collect = [&]() {
        for (auto it = live.begin(); it != live.end(); ++it) {
            int status = 0;
            pid_t rp = waitpid(it->pid, &status, WNOHANG);
            if (rp == it->pid) {
                char buf[32] = {0};
                size_t n = read(it->fd, buf, sizeof buf - 1);
                close(it->fd);
                double score = 0;
                if (n > 0) {
                    if (buf[0] == 'w') {
                        int w = atoi(buf + 1);
                        score = (w == 1) ? 100.0 : (w == 2) ? 50.0 : 0.0;
                    } else if (buf[0] == 's') {
                        score = atof(buf + 1);
                    }
                }
                totals[it->ai] += score;
                counts[it->ai]++;
                live.erase(it);
                return;
            }
        }
        // no child finished; wait a bit
        usleep(2000);
    };
    for (size_t ai = 0; ai < actions.size(); ++ai)
        for (int r = 0; r < per; ++r) {
            while ((int)live.size() >= kMaxParallel) collect();
            spawn(ai, r);
        }
    while (!live.empty()) collect();

    double best = -1.0;
    for (size_t ai = 0; ai < actions.size(); ++ai) {
        double avg = counts[ai] ? totals[ai] / counts[ai] : 0.0;
        out.scores.push_back({(int)ai, avg});
        if (avg > best + 0.001) { best = avg; out.chosen = (int)ai; }
    }
    // how many candidates share the top winrate (tie info for the UI)
    out.ties = 0;
    for (auto& sc : out.scores)
        if (std::fabs(sc.second - best) < 0.01) out.ties++;
    return out;
}
