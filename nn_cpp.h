// nn_cpp.h — in-process champion/value inference for search rollouts.
//
// Mirrors the Python feature pipeline (ml/dataset.py + ml/decode.py +
// ml/predict_server.py) bit-for-bit so a rollout child can play the champion
// without spawning a Python process: weights and card stats are loaded once in
// the parent and inherited by fork() children via copy-on-write.
//
// Card identity (added 2025-09): .bin files trained with the embedded
// architecture carry an extra section after the MLP weights:
//   [E i32][n_rows i32][n_vocab i32][vocab codes u32*n_vocab]
//   [embed f32 n_rows*E]
// Embedding row semantics match dataset.py exactly: row 0 = "no card" (zero
// vector, never trained), rows 1..n_vocab = era codes sorted ascending,
// row n_rows-1 = shared UNKNOWN. A flat feature row is then
//   [ctx static (CTX_DIM) | opt static+pass (OPT_BASE+1)
//    | ctx code rows (32) | opt code row (1)]   <- policy pick
//   [ctx static (CTX_DIM) | ctx code rows (32)] <- value evaluate
// and forward_row() expands the trailing code columns through the embedding
// table before running the MLP, exactly like the torch nn.Embedding concat.
// Legacy .bin files without the section load and run the old stat-only net.
//
// Included from duel_runner_edo.cpp AFTER interactive.h/search.h, so it may use
// g_duel, g_field, g_turns, g_current_phase, g_last_hint_selectmsg,
// query_location() and the LOCATION_* / TYPE_* constants.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace nncpp {

constexpr int CF_LEN = 38;
constexpr int N_KINDS = 12;
constexpr int OPT_BASE = CF_LEN + 6 + N_KINDS + 1;   // 57
constexpr int SLOT_D = CF_LEN + 2;                    // 40
constexpr int HAND_SLOTS = 6, MZ_SLOTS = 5, SZ_SLOTS = 5;
constexpr int CTX_SCALARS = 15;
constexpr int CTX_DIM = CTX_SCALARS + (HAND_SLOTS + MZ_SLOTS * 2 + SZ_SLOTS * 2
                                       + HAND_SLOTS) * SLOT_D;   // 1295
// embedding code columns (mirrors ml/dataset.py CTX_CODES / POLICY_NCODES)
constexpr int N_CTX_CODES = HAND_SLOTS + MZ_SLOTS * 2 + SZ_SLOTS * 2
                            + HAND_SLOTS;                        // 32
constexpr int POLICY_NCODES = N_CTX_CODES + 1;                   // 33
constexpr int POLICY_STATIC = CTX_DIM + OPT_BASE + 1;            // 1353

inline float sat(float x, float m) {
    return std::max(0.0f, std::min(1.0f, x / m));
}

struct Stats { int32_t atk = 0, def = 0, lvl = 0, race = 0, attr = 0, typ = 0; };

struct Opt {
    uint32_t code = 0;
    uint8_t ctrl = 0, loc = 0;
    uint32_t pos = 0;
    uint64_t desc = 0;
    uint8_t mode = 0;
    int kind = 0;
};

static FILE* g_dump = nullptr;   // --nn-dump debug log

struct Model {
    bool loaded = false;
    int n_in = 0, n_h1 = 0, n_h2 = 0, n_out = 0;
    std::vector<float> W0, b0, W1, b1, W2, b2;
    std::unordered_map<uint32_t, Stats> stats;
    // learned per-code embedding (optional section in .bin)
    bool has_emb_ = false;
    int embE_ = 0;                 // embedding width per card
    int embRows_ = 0;              // total rows (0 + vocab + UNKNOWN)
    std::vector<uint32_t> embVocab_;   // sorted era codes (rows 1..n)
    std::vector<float> embTab_;        // embRows_ * embE_ floats

    void load_stats() {
        stats.clear();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                "SELECT id, atk, def, level, race, attribute, type FROM datas",
                -1, &st, nullptr) != SQLITE_OK)
            return;
        while (sqlite3_step(st) == SQLITE_ROW) {
            uint32_t id = static_cast<uint32_t>(sqlite3_column_int(st, 0));
            Stats s;
            s.atk = sqlite3_column_int(st, 1);
            s.def = sqlite3_column_int(st, 2);
            s.lvl = sqlite3_column_int(st, 3);
            s.race = sqlite3_column_int(st, 4);
            s.attr = sqlite3_column_int(st, 5);
            s.typ = sqlite3_column_int(st, 6);
            stats[id] = s;
        }
        sqlite3_finalize(st);
    }

    bool load(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        int hdr[5] = {0};
        if (std::fread(hdr, sizeof(int), 5, f) != 5 || hdr[0] != 0x4E4E43) {
            std::fclose(f);
            return false;
        }
        n_in = hdr[1]; n_h1 = hdr[2]; n_h2 = hdr[3]; n_out = hdr[4];
        auto rd = [&](std::vector<float>& v, int n) {
            v.resize(n);
            return std::fread(v.data(), sizeof(float), n, f) == (size_t)n;
        };
        bool ok = rd(W0, n_h1 * n_in) && rd(b0, n_h1)
               && rd(W1, n_h2 * n_h1) && rd(b1, n_h2)
               && rd(W2, n_out * n_h2) && rd(b2, n_out);
        // optional embedding section: [E i32][n_rows i32][n_vocab i32]
        // [vocab codes u32*n_vocab][emb f32 n_rows*E]; legacy files end here
        has_emb_ = false; embE_ = 0; embRows_ = 0;
        embVocab_.clear(); embTab_.clear();
        int eh[3] = {0, 0, 0};
        if (ok && std::fread(eh, sizeof(int), 3, f) == 3
            && eh[0] > 0 && eh[1] == eh[2] + 2 && eh[2] >= 0) {
            embE_ = eh[0]; embRows_ = eh[1];
            embVocab_.resize((size_t)eh[2]);
            if (std::fread(embVocab_.data(), sizeof(uint32_t),
                           embVocab_.size(), f) != embVocab_.size())
                ok = false;
            embTab_.resize((size_t)embRows_ * (size_t)embE_);
            if (ok && std::fread(embTab_.data(), sizeof(float),
                                 embTab_.size(), f) != embTab_.size())
                ok = false;
            has_emb_ = ok;
            if (!ok) { embE_ = 0; embRows_ = 0; embTab_.clear(); embVocab_.clear(); }
        }
        std::fclose(f);
        loaded = ok;
        return ok;
    }

    // card_feats: 38 dims, exact mirror of ml/dataset.py::card_feats
    void card_feats(uint32_t code, float* f) const {
        Stats st;
        auto it = stats.find(code);
        if (it != stats.end()) st = it->second;
        int lv = std::max(0, std::min(static_cast<int>(st.lvl), 12));
        f[0] = sat(static_cast<float>(std::max(0, st.atk)), 4000.0f);
        f[1] = sat(static_cast<float>(std::max(0, st.def)), 4000.0f);
        const uint32_t bits[21] = {0x1, 0x2, 0x4, 0x10, 0x20, 0x40, 0x80,
                                   0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000,
                                   0x4000, 0x10000, 0x20000, 0x40000, 0x80000,
                                   0x100000, 0x200000, 0x400000};
        uint32_t t = static_cast<uint32_t>(st.typ);
        for (int i = 0; i < 21; ++i) f[2 + i] = (t & bits[i]) ? 1.0f : 0.0f;
        f[23] = sat(static_cast<float>(st.attr), 16.0f);
        f[24] = sat(static_cast<float>(st.race), 67108864.0f);   // 1<<26
        f[25] = sat(static_cast<float>(lv), 12.0f);
        for (int i = 0; i < 12; ++i) f[26 + i] = (lv == i + 1) ? 1.0f : 0.0f;
    }

    // slot_vec: card_feats + [faceup, def] (ml/dataset.py::_slot_vec)
    void slot_vec(uint32_t code, uint32_t pos, float* f) const {
        card_feats(code, f);
        f[CF_LEN + 0] = (pos & 0x5) ? 1.0f : 0.0f;
        f[CF_LEN + 1] = (pos & 0x2) ? 1.0f : 0.0f;
    }

    // option_vec: 57 dims (ml/dataset.py::option_vec, OPT_BASE)
    void option_feats(const Opt& o, uint8_t player, float* f) const {
        card_feats(o.code, f);
        float own = (o.ctrl == player) ? 1.0f : 0.0f;
        f[CF_LEN + 0] = sat(static_cast<float>(o.loc), 64.0f);
        f[CF_LEN + 1] = sat(static_cast<float>(o.pos), 8.0f);
        f[CF_LEN + 2] = (o.pos & 0x5) ? 1.0f : 0.0f;
        f[CF_LEN + 3] = own;
        f[CF_LEN + 4] = sat(static_cast<float>(o.mode), 3.0f);
        f[CF_LEN + 5] = sat(static_cast<float>(o.desc), 4294967296.0f); // 1<<32
        for (int i = 0; i < N_KINDS; ++i)
            f[CF_LEN + 6 + i] = (i == o.kind) ? 1.0f : 0.0f;
        f[CF_LEN + 6 + N_KINDS] = (o.code != 0) ? 1.0f : 0.0f;
    }

    // context_vec: 1295 dims (ml/dataset.py::context_vec) — static only; the
    // per-slot embedding rows come from ctx_code_row() below.
    void context_vec(uint8_t viewer, float* out) const {
        const auto duel = reinterpret_cast<OCG_Duel>(g_duel);
        const uint8_t opp = 1 - viewer;
        auto codes = [&](uint8_t p, uint32_t loc) {
            std::vector<uint32_t> v;
            for (auto& qc : query_location(duel, p, loc))
                if (qc.code) v.push_back(qc.code);
            return v;
        };
        auto hand0 = codes(viewer, LOCATION_HAND);
        auto hand1q = query_location(duel, opp, LOCATION_HAND);
        auto g0 = codes(viewer, LOCATION_GRAVE);
        auto g1 = codes(opp, LOCATION_GRAVE);
        auto ban0 = codes(viewer, LOCATION_REMOVED);
        auto ban1 = codes(opp, LOCATION_REMOVED);
        auto extra0 = codes(viewer, LOCATION_EXTRA);
        int extra1_count = static_cast<int>(query_location(duel, opp, LOCATION_EXTRA).size());
        int deck0_count = static_cast<int>(query_location(duel, viewer, LOCATION_DECK).size());
        int deck1_count = static_cast<int>(query_location(duel, opp, LOCATION_DECK).size());

        int o = 0;
        out[o++] = sat(static_cast<float>(g_turns), 30.0f);
        out[o++] = sat(static_cast<float>(g_current_phase), 512.0f);
        out[o++] = sat(static_cast<float>(g_field.lp[viewer]), 8000.0f);
        out[o++] = sat(static_cast<float>(g_field.lp[opp]), 8000.0f);
        out[o++] = sat(static_cast<float>(g_last_hint_selectmsg), 1000.0f);
        out[o++] = sat(static_cast<float>(hand0.size()), 12.0f);
        out[o++] = sat(static_cast<float>(hand1q.size()), 12.0f);
        out[o++] = sat(static_cast<float>(deck0_count), 60.0f);
        out[o++] = sat(static_cast<float>(deck1_count), 60.0f);
        out[o++] = sat(static_cast<float>(extra0.size()), 20.0f);
        out[o++] = sat(static_cast<float>(extra1_count), 20.0f);
        out[o++] = sat(static_cast<float>(g0.size()), 60.0f);
        out[o++] = sat(static_cast<float>(g1.size()), 60.0f);
        out[o++] = sat(static_cast<float>(ban0.size()), 30.0f);
        out[o++] = sat(static_cast<float>(ban1.size()), 30.0f);

        // hand0 (viewer hand): code only, pos 0
        for (int i = 0; i < HAND_SLOTS; ++i) {
            if (i < (int)hand0.size()) {
                card_feats(hand0[i], out + o + i * SLOT_D);
                out[o + i * SLOT_D + CF_LEN] = 0.0f;
                out[o + i * SLOT_D + CF_LEN + 1] = 0.0f;
            } else {
                std::memset(out + o + i * SLOT_D, 0, SLOT_D * sizeof(float));
            }
        }
        o += HAND_SLOTS * SLOT_D;
        pad_zone(viewer, LOCATION_MZONE, viewer, out + o); o += MZ_SLOTS * SLOT_D;
        pad_zone(viewer, LOCATION_SZONE, viewer, out + o); o += SZ_SLOTS * SLOT_D;
        pad_zone(opp, LOCATION_MZONE, viewer, out + o); o += MZ_SLOTS * SLOT_D;
        pad_zone(opp, LOCATION_SZONE, viewer, out + o); o += SZ_SLOTS * SLOT_D;
        // opponent hand hidden: all zeros
        std::memset(out + o, 0, HAND_SLOTS * SLOT_D * sizeof(float));
    }

    void pad_zone(uint8_t player, uint32_t loc, uint8_t viewer, float* out) const {
        const auto duel = reinterpret_cast<OCG_Duel>(g_duel);
        auto q = query_location(duel, player, loc);
        int slots = (loc == LOCATION_MZONE) ? MZ_SLOTS : SZ_SLOTS;
        int n = std::min(slots, static_cast<int>(q.size()));
        for (int i = 0; i < n; ++i) {
            auto& qc = q[i];
            bool faceup = (qc.pos & 0x5) != 0;
            bool hidden = (player == (1 - viewer)) && !faceup;
            uint32_t code = hidden ? 0 : qc.code;
            slot_vec(code, qc.pos, out + i * SLOT_D);
        }
        for (int i = n; i < slots; ++i)
            std::memset(out + i * SLOT_D, 0, SLOT_D * sizeof(float));
    }

    // ---- learned per-card embeddings ------------------------------------
    // embedding row for a card code (0 = empty, 1+ = position in vocab+1,
    // last row = UNKNOWN); mirrors ml/dataset.py::emb_idx_from_codes.
    uint32_t code_row(uint32_t code) const {
        if (!has_emb_ || code == 0) return 0;
        size_t lo = 0, hi = embVocab_.size();
        while (lo < hi) {
            size_t mid = (lo + hi) >> 1;
            if (embVocab_[mid] < code) lo = mid + 1; else hi = mid;
        }
        if (lo < embVocab_.size() && embVocab_[lo] == code)
            return static_cast<uint32_t>(1 + lo);
        return static_cast<uint32_t>(embRows_ - 1);
    }

    // anti-clairvoyance: options referencing an opponent's FACE-DOWN card are
    // position-only in a real game; zero the code so the model can't read its
    // identity.  Mirrors ml/dataset.py::opt_visible.
    uint32_t hide_unknown(uint32_t code, uint8_t ctrl, uint32_t pos,
                          uint8_t player) const {
        if (code && ctrl != player && !(pos & 0x5u))
            return 0;
        return code;
    }

    // 32 embedding rows for the context slots, in the exact order context_vec
    // pads slots (hand0, mz0, sz0, mz1, sz1, opp-hand zeros) — mirrors
    // ml/dataset.py::ctx_codes -> emb_idx. Hidden opponent facedown cards and
    // empty slots both yield row 0 (their static slot features stay zero).
    void ctx_code_row(uint8_t viewer, float* out) const {
        const auto duel = reinterpret_cast<OCG_Duel>(g_duel);
        const uint8_t opp = 1 - viewer;
        size_t o = 0;
        auto handq = query_location(duel, viewer, LOCATION_HAND);
        size_t hn = std::min((size_t)HAND_SLOTS, handq.size());
        for (size_t i = 0; i < hn; ++i) out[o++] = (float)code_row(handq[i].code);
        for (; o < (size_t)HAND_SLOTS; ++o) out[o] = 0.0f;
        // slot order must match context_vec: viewer MZ/SZ then opp MZ/SZ
        pad_code_zone(viewer, LOCATION_MZONE, opp, out, o);
        pad_code_zone(viewer, LOCATION_SZONE, opp, out, o);
        pad_code_zone(opp, LOCATION_MZONE, opp, out, o);
        pad_code_zone(opp, LOCATION_SZONE, opp, out, o);
        for (int i = 0; i < HAND_SLOTS; ++i) out[o++] = 0.0f;
    }

    void pad_code_zone(uint8_t player, uint32_t loc, uint8_t opp,
                       float* out, size_t& o) const {
        const auto duel = reinterpret_cast<OCG_Duel>(g_duel);
        auto q = query_location(duel, player, loc);
        int slots = (loc == LOCATION_MZONE) ? MZ_SLOTS : SZ_SLOTS;
        int n = std::min(slots, static_cast<int>(q.size()));
        for (int i = 0; i < n; ++i) {
            auto& qc = q[i];
            bool faceup = (qc.pos & 0x5) != 0;
            bool hidden = (player == opp) && !faceup;
            out[o++] = (float)code_row(hidden ? 0 : qc.code);
        }
        for (int i = n; i < slots; ++i) out[o++] = 0.0f;
    }

    // P(viewer wins) in [0,1] for the CURRENT position (value head).
    float evaluate(uint8_t viewer) const {
        const int ncc = has_emb_ ? N_CTX_CODES : 0;
        std::vector<float> row(CTX_DIM + ncc);
        context_vec(viewer, row.data());
        if (ncc) ctx_code_row(viewer, row.data() + CTX_DIM);
        float logit = forward_row(row.data(), CTX_DIM, ncc);
        return 1.0f / (1.0f + std::exp(-logit));
    }

    // MLP body over an already-expanded vector of length n_in.
    float forward_mlp(const float* x) const {
        std::vector<float> h1(n_h1);
        for (int i = 0; i < n_h1; ++i) {
            float s = b0[i];
            const float* w = W0.data() + i * n_in;
            for (int j = 0; j < n_in; ++j) s += w[j] * x[j];
            h1[i] = s > 0.0f ? s : 0.0f;
        }
        std::vector<float> h2(n_h2);
        for (int i = 0; i < n_h2; ++i) {
            float s = b1[i];
            const float* w = W1.data() + i * n_h1;
            for (int j = 0; j < n_h1; ++j) s += w[j] * h1[j];
            h2[i] = s > 0.0f ? s : 0.0f;
        }
        float out = b2[0];
        const float* w = W2.data();
        for (int j = 0; j < n_h2; ++j) out += w[j] * h2[j];
        return out;
    }

    // forward over a flat feature row [static (row_static) | code rows
    // (n_codes)]; expands the code rows through the embedding table then runs
    // the MLP. Without embeddings (legacy .bin) x is the whole MLP input.
    float forward_row(const float* x, int row_static, int n_codes) const {
        if (!has_emb_ || n_codes <= 0)
            return forward_mlp(x);
        const int expect = row_static + n_codes * embE_;
        if (expect != n_in) return 0.0f;     // model/layout mismatch guard
        std::vector<float> big((size_t)expect);
        std::memcpy(big.data(), x, (size_t)row_static * sizeof(float));
        float* e = big.data() + row_static;
        for (int k = 0; k < n_codes; ++k) {
            int r = static_cast<int>(x[row_static + k] + 0.5f);
            if (r < 0) r = 0;
            else if (r >= embRows_) r = embRows_ - 1;
            std::memcpy(e, embTab_.data() + (size_t)r * (size_t)embE_,
                        (size_t)embE_ * sizeof(float));
            e += embE_;
        }
        return forward_mlp(big.data());
    }

    // score one option row: [ctx static (1295) | opt feats+pass (58)
    // | ctx code rows (32) | opt code row (1)]  — mirrors dataset.py rows.
    float score_row(const float* ctx, const float* cid, const Opt& o,
                    uint8_t player, float pass_flag) const {
        const int ncc = has_emb_ ? N_CTX_CODES : 0;
        const int row_static = CTX_DIM + OPT_BASE + 1;          // 1353
        std::vector<float> x(row_static + ncc + (has_emb_ ? 1 : 0));
        std::memcpy(x.data(), ctx, CTX_DIM * sizeof(float));
        option_feats(o, player, x.data() + CTX_DIM);
        x[CTX_DIM + OPT_BASE] = pass_flag;
        if (has_emb_) {
            std::memcpy(x.data() + row_static, cid,
                        (size_t)N_CTX_CODES * sizeof(float));
            x[row_static + N_CTX_CODES] = (float)code_row(o.code);
        }
        return forward_row(x.data(), row_static, ncc + (has_emb_ ? 1 : 0));
    }

    static uint32_t u32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
             | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    // Decode the prompt and return the argmax option (or -1 for chain pass).
    bool pick(uint8_t type, uint8_t player, const uint8_t* p, int len,
              int& choice) const {
        if (!loaded) return false;
        std::vector<float> ctx(CTX_DIM);
        context_vec(player, ctx.data());
        std::vector<float> cid(N_CTX_CODES);
        if (has_emb_) ctx_code_row(player, cid.data());
        std::vector<Opt> opts;
        if (type == 15) {          // card (single select, min==1 max==1)
            int n = static_cast<int>(u32(p + 11));
            for (int i = 0; i < n; ++i) {
                const uint8_t* o = p + 15 + i * 14;
                Opt x; x.code = u32(o); x.ctrl = o[4]; x.loc = o[5];
                x.pos = u32(o + 10); x.kind = 0;
                x.code = hide_unknown(x.code, x.ctrl, x.pos, player);
                opts.push_back(x);
            }
        } else if (type == 16) {   // chain
            int n = static_cast<int>(u32(p + 12));
            for (int i = 0; i < n; ++i) {
                // 24 bytes per link: code(4) ctrl(1) loc(1) seq(4) pos(4)
                // desc(8) client_mode(1) trigger_flag(1).  The trailing flag is
                // not a model feature (the weights have no input for it) — it
                // only has to be stepped over so later links decode correctly.
                const uint8_t* o = p + 16 + i * 24;
                Opt x; x.code = u32(o); x.ctrl = o[4]; x.loc = o[5];
                x.pos = u32(o + 10); x.desc = u32(o + 14); x.mode = o[22];
                x.kind = 6;
                x.code = hide_unknown(x.code, x.ctrl, x.pos, player);
                opts.push_back(x);
            }
        } else if (type == 11) {   // idlecmd
            int off = 2;
            const int esz[6] = {10, 10, 7, 10, 10, 19};
            for (int cat = 0; cat < 6; ++cat) {
                int n = static_cast<int>(u32(p + off)); off += 4;
                for (int i = 0; i < n; ++i) {
                    Opt x; x.code = u32(p + off); x.ctrl = p[off + 4];
                    x.loc = p[off + 5]; x.kind = cat + 1;
                    opts.push_back(x);
                    off += esz[cat];
                }
            }
            if (p[off]) opts.push_back({0, 0, 0, 0, 0, 0, 7});        // bp
            if (p[off + 1]) opts.push_back({0, 0, 0, 0, 0, 0, 8});    // ep
        } else if (type == 10) {   // battlecmd
            int off = 2;
            int na = static_cast<int>(u32(p + off)); off += 4;
            for (int i = 0; i < na; ++i) {
                const uint8_t* o = p + off;
                Opt x; x.code = u32(o); x.ctrl = o[4]; x.loc = o[5];
                x.desc = u32(o + 10); x.mode = o[18]; x.kind = 6;
                opts.push_back(x);
                off += 19;
            }
            int nn = static_cast<int>(u32(p + off)); off += 4;
            for (int i = 0; i < nn; ++i) {
                const uint8_t* o = p + off;
                Opt x; x.code = u32(o); x.ctrl = o[4]; x.loc = o[5];
                x.kind = 11;
                opts.push_back(x);
                off += 8;
            }
            if (p[off]) opts.push_back({0, 0, 0, 0, 0, 0, 10});        // m2
            if (p[off + 1]) opts.push_back({0, 0, 0, 0, 0, 0, 8});     // ep
        } else {
            return false;
        }
        if (opts.empty()) return false;

        std::vector<float> scores;
        int best = 0;
        float best_score = -1e30f;
        int pass_idx = (type == 16) ? static_cast<int>(opts.size()) : -1;
        for (int i = 0; i < (int)opts.size(); ++i) {
            float sc = score_row(ctx.data(), cid.data(), opts[i], player, 0.0f);
            scores.push_back(sc);
            if (sc > best_score) { best_score = sc; best = i; }
        }
        if (type == 16) {
            Opt pas; pas.kind = 0;
            float sc = score_row(ctx.data(), cid.data(), pas, player, 1.0f);
            scores.push_back(sc);
            if (sc > best_score) { best_score = sc; best = pass_idx; }
        }
        if (g_dump) {
            std::fprintf(g_dump, "%d %d %d %d", type, player,
                         (int)opts.size(), best);
            for (float s : scores) std::fprintf(g_dump, " %.6f", s);
            std::fprintf(g_dump, "\n");
        }
        choice = (type == 16 && best == pass_idx) ? -1 : best;
        return true;
    }
};

static Model g_cpp;
static Model g_value;   // value head (P(win)); 1295(+32 emb) inputs
static bool g_cpp_active[2] = {false, false};

}  // namespace nncpp
