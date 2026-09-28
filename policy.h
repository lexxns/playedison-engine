// policy.h — per-deck rule-based AI policies for the Edison sim runner.
//
// A Policy turns the raw SELECT-message candidates (card codes) into scored
// choices: activation, summon, set, tribute, attack targeting, chain
// responses.  Priorities are hand-tuned per archetype from the deck lists in
// decks/*.json.  A lightweight field model (built from MSG_MOVE /
// MSG_POS_CHANGE / MSG_SUMMONING / MSG_SET / MSG_ATTACK / ...) lets the
// policy reason about the opponent's board for battle decisions.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <utility>

// ---------------------------------------------------------------------------
// field model
// ---------------------------------------------------------------------------
struct FCard {
    uint32_t code = 0;
    uint8_t ctrl = 0;      // owner/controler
    uint8_t loc = 0;       // LOCATION_*
    uint32_t seq = 0;
    uint32_t pos = 0;      // POS_*
    int32_t atk = 0;
    int32_t def = 0;
    bool faceup = false;
    bool known = false;    // perfect-memory: the AI has seen/remembers this card
};

struct FieldState {
    // per player: monsters then spells/traps, keyed by (loc, seq)
    std::map<std::pair<uint8_t, uint32_t>, FCard> zone[2];
    int lp[2] = {8000, 8000};
    int hand_count[2] = {0, 0};
    // full hand / GY contents (codes) for interactive UI
    std::vector<uint32_t> hand[2];
    std::vector<uint32_t> grave[2];
    // currently-targeted cards: (player, location, seq)
    std::vector<std::tuple<uint8_t, uint8_t, uint32_t>> targets;

    std::vector<FCard> mzone(uint8_t p) const {
        std::vector<FCard> out;
        for (auto& kv : zone[p])
            if (kv.first.first == 0x04 /* LOCATION_MZONE */)
                out.push_back(kv.second);
        return out;
    }
    std::vector<FCard> szone(uint8_t p) const {
        std::vector<FCard> out;
        for (auto& kv : zone[p])
            if (kv.first.first == 0x08 /* LOCATION_SZONE */)
                out.push_back(kv.second);
        return out;
    }
    void clear() {
        zone[0].clear(); zone[1].clear();
        lp[0] = lp[1] = 8000;
        hand_count[0] = hand_count[1] = 0;
        hand[0].clear(); hand[1].clear();
        grave[0].clear(); grave[1].clear();
        targets.clear();
    }
};

// ---------------------------------------------------------------------------
// policy tables
// ---------------------------------------------------------------------------
struct Policy {
    // activation priority: spells/traps/effects we want to play (higher = sooner)
    std::map<uint32_t, int> act;
    // normal summon priority (higher = preferred normal summon)
    std::map<uint32_t, int> summon;
    // special-summon / self-summon priority
    std::map<uint32_t, int> spsummon;
    // set-face-down priority (traps, Book of Moon)
    std::map<uint32_t, int> set;
    // cards we want in the GY (Foolish/Allure/mill targets); also used to
    // prefer them as tribute/synchro materials and discard costs
    std::map<uint32_t, int> dump;
    // cards we want to keep in hand (DAD, Gorz, Honest, Kalut ...)
    std::map<uint32_t, int> keep;
    // tribute priority: which of OUR monsters we prefer to sacrifice
    std::map<uint32_t, int> tribute;
    // threat rating: opponent cards we most want destroyed/removed
    std::map<uint32_t, int> threat;
    // generic tuning
    int aggressiveness = 60;   // 0..100: willingness to attack into uncertain boards
    bool always_attack = false; // attack even when trades are bad

    int act_score(uint32_t c) const {
        auto it = act.find(c);
        return it == act.end() ? 0 : it->second;
    }
    int summon_score(uint32_t c) const {
        auto it = summon.find(c);
        int s = it == summon.end() ? 0 : it->second;
        auto it2 = spsummon.find(c);
        return s + (it2 == spsummon.end() ? 0 : it2->second);
    }
    int set_score(uint32_t c) const {
        auto it = set.find(c);
        return it == set.end() ? 0 : it->second;
    }
    int dump_score(uint32_t c) const {
        auto it = dump.find(c);
        return it == dump.end() ? 0 : it->second;
    }
    int keep_score(uint32_t c) const {
        auto it = keep.find(c);
        return it == keep.end() ? 0 : it->second;
    }
    int tribute_score(uint32_t c) const {
        auto it = tribute.find(c);
        return it == tribute.end() ? 0 : it->second;
    }
    int threat_score(uint32_t c) const {
        auto it = threat.find(c);
        return it == threat.end() ? 0 : it->second;
    }
};

// ---------------------------------------------------------------------------
// per-deck policy builders (hand-tuned from decks/*.json)
// ---------------------------------------------------------------------------
static Policy make_policy_vayu_turbo() {
    Policy p;
    p.aggressiveness = 55;
    // combo enablers
    p.act[81439173] = 95;    // Foolish Burial
    p.act[67723438] = 90;    // Emergency Teleport
    p.act[1475311] = 80;     // Allure of Darkness
    p.act[83764718] = 85;    // Monster Reborn
    p.act[87910978] = 75;    // Brain Control
    p.act[37520316] = 75;    // Mind Control
    p.act[98045062] = 70;    // Enemy Controller
    p.act[67169062] = 65;    // Pot of Avarice
    // removal / disruption
    p.act[19613556] = 85;    // Heavy Storm
    p.act[5318639] = 80;     // MST
    p.act[60682203] = 75;    // Cold Wave
    p.act[14087893] = 70;    // Book of Moon
    p.act[29401950] = 60;    // Bottomless
    p.act[44095762] = 55;    // Mirror Force
    p.act[53582587] = 55;    // Torrential
    p.act[63356631] = 60;    // Phoenix Wing Wind Blast
    p.act[4178474] = 60;     // Raigeki Break
    p.act[64697231] = 65;    // Trap Dustshoot
    p.act[93016201] = 50;    // Royal Oppression (own synchro engine hurts)
    p.act[94192409] = 55;    // Compulsory
    p.act[97077563] = 70;    // Call of the Haunted
    p.act[41420027] = 65;    // Solemn Judgment
    // summon priorities
    p.spsummon[59575539] = 95;   // Krebons (E-Tele target)
    p.spsummon[72714392] = 90;   // Vayu (from GY / hand)
    p.summon[28985331] = 85;     // Armageddon Knight (dumps Vayu/PSZ)
    p.summon[14536035] = 75;     // Dark Grepher
    p.summon[33420078] = 70;     // Plaguespreader (tuner)
    p.summon[65192027] = 80;     // DAD
    p.summon[9596126] = 70;      // Chaos Sorcerer
    p.summon[70095154] = 60;     // Cyber Dragon
    p.summon[23205979] = 55;     // Spirit Reaper
    p.spsummon[98777036] = 60;   // Tragoedia (hand)
    p.spsummon[44330098] = 65;   // Gorz (hand)
    // dump priorities (GY targets)
    p.dump[72714392] = 100;   // Vayu
    p.dump[33420078] = 90;    // Plaguespreader
    p.dump[28985331] = 20;    // Armageddon (itself not a target)
    p.dump[21502796] = 40;    // Ryko
    // keep in hand
    p.keep[65192027] = 80;    // DAD needs exactly 3 DARK in GY
    p.keep[44330098] = 70;    // Gorz
    p.keep[98777036] = 60;    // Tragoedia
    p.keep[65192027] = 80;
    // tribute preference
    p.tribute[23205979] = 90; // Spirit Reaper
    p.tribute[21502796] = 80; // Ryko
    p.tribute[28985331] = 70; // Armageddon after effect used
    // threats
    p.threat[93016201] = 90;  // Royal Oppression
    p.threat[82732705] = 80;  // Skill Drain
    p.threat[81674782] = 80;  // Dimensional Fissure
    p.threat[94853057] = 75;  // Banisher
    p.threat[42009836] = 85;  // Fossil Dyna
    return p;
}

static Policy make_policy_blackwing() {
    Policy p;
    p.aggressiveness = 70;
    // spells
    p.act[1475311] = 85;     // Allure
    p.act[83764718] = 85;    // Monster Reborn
    p.act[19613556] = 85;    // Heavy Storm
    p.act[5318639] = 80;     // MST
    p.act[87910978] = 75;    // Brain Control
    p.act[60682203] = 75;    // Cold Wave
    p.act[81439173] = 70;    // Foolish Burial (dump Vayu/Mistral)
    p.act[37520316] = 70;    // Mind Control
    p.act[67169062] = 70;    // Pot of Avarice
    p.act[14087893] = 70;    // Book of Moon
    // traps
    p.act[53567095] = 80;    // Icarus Attack
    p.act[64697231] = 70;    // Trap Dustshoot
    p.act[29401950] = 65;    // Bottomless
    p.act[44095762] = 60;    // Mirror Force
    p.act[53582587] = 60;    // Torrential
    p.act[93016201] = 45;    // Royal Oppression (hurts own synchro)
    p.act[41420027] = 65;    // Solemn
    p.act[97077563] = 65;    // Call of the Haunted
    // summons
    p.summon[22835145] = 90; // Blizzard (tuner)
    p.summon[2009101] = 90;  // Gale (tuner, halves)
    p.summon[58820853] = 85; // Shura
    p.summon[49003716] = 85; // Bora
    p.summon[75498415] = 70; // Sirocco
    p.summon[46710683] = 60; // Mistral (tuner)
    p.summon[72714392] = 55; // Vayu
    p.summon[65192027] = 75; // DAD
    p.spsummon[85215458] = 0; // Kalut: keep in hand
    // keep
    p.keep[85215458] = 100;  // Kalut (hand trap)
    p.keep[65192027] = 80;   // DAD
    p.keep[44330098] = 70;   // Gorz
    // dump
    p.dump[72714392] = 90;   // Vayu
    p.dump[46710683] = 80;   // Mistral (tuner for Vayu in GY)
    // tribute
    p.tribute[46710683] = 80; // Mistral (for Icarus)
    p.tribute[72714392] = 70;
    p.tribute[58820853] = 60;
    // threats
    p.threat[93016201] = 90;
    p.threat[82732705] = 80;
    p.threat[81674782] = 80;
    p.threat[94853057] = 75;
    p.threat[42009836] = 85;
    return p;
}

static Policy make_policy_frog_monarchs() {
    Policy p;
    p.aggressiveness = 50;
    // frogs: dump/keep rules
    p.dump[12538374] = 100;  // Treeborn Frog -> GY at all costs
    p.dump[20663556] = 90;   // Substitoad
    p.dump[1357146] = 85;    // Ronintoadin
    p.dump[9126351] = 80;    // Swap Frog
    p.dump[46239604] = 75;   // Dupe Frog
    // monarchs
    p.summon[9748752] = 95;  // Caius
    p.summon[73125233] = 90; // Raiza
    p.summon[26205777] = 80; // Thestalos
    p.summon[4929256] = 80;  // Mobius
    p.summon[47297616] = 85; // LaDD
    p.summon[89111398] = 70; // Dark Dust Spirit
    p.summon[70095154] = 60; // Cyber Dragon
    p.spsummon[12538374] = 70; // Treeborn self-SS
    p.spsummon[1357146] = 70;  // Ronin
    p.spsummon[9126351] = 70;  // Swap
    p.spsummon[46239604] = 65; // Dupe
    // spells
    p.act[83764718] = 85;
    p.act[19613556] = 85;
    p.act[5318639] = 80;
    p.act[87910978] = 80;
    p.act[37520316] = 75;
    p.act[81439173] = 90;    // Foolish -> Treeborn
    p.act[98045062] = 75;
    p.act[60682203] = 70;
    p.act[14087893] = 70;
    p.act[67169062] = 60;
    // traps
    p.act[29401950] = 65;
    p.act[44095762] = 60;
    p.act[53582587] = 60;
    p.act[41420027] = 65;
    p.act[64697231] = 70;
    p.act[63356631] = 60;
    p.act[4178474] = 60;
    // tribute: frogs are the fodder
    p.tribute[46239604] = 90;
    p.tribute[1357146] = 90;
    p.tribute[9126351] = 85;
    p.tribute[20663556] = 85;
    p.tribute[12538374] = 70; // Treeborn if stuck
    // keep
    p.keep[44330098] = 70;
    // threats
    p.threat[93016201] = 90;
    p.threat[82732705] = 80;
    p.threat[81674782] = 85;
    p.threat[94853057] = 75;
    p.threat[42009836] = 85;
    return p;
}

static Policy make_policy_lightsworn() {
    Policy p;
    p.aggressiveness = 45;
    // mill engine
    p.act[94886282] = 95;    // Charge of the Light Brigade
    p.act[691925] = 95;      // Solar Recharge
    p.act[16255442] = 80;    // Beckoning Light
    p.act[81439173] = 75;    // Foolish
    p.act[83764718] = 80;
    p.act[19613556] = 80;
    p.act[5318639] = 75;
    p.act[87910978] = 70;
    p.act[14087893] = 65;
    p.act[60682203] = 65;
    p.act[67169062] = 70;    // Avarice
    // summons
    p.summon[95503687] = 95; // Lumina
    p.summon[22624373] = 85; // Lyla
    p.summon[21502796] = 80; // Ryko
    p.summon[85087012] = 80; // Card Trooper
    p.summon[94381039] = 85; // Celestia
    p.summon[96235275] = 70; // Jain
    p.summon[44178886] = 70; // Ehren
    p.summon[59019082] = 70; // Garoth
    p.summon[57774843] = 90; // Judgment Dragon
    p.spsummon[58996430] = 85; // Wulf
    p.spsummon[33420078] = 70; // Plaguespreader
    p.spsummon[37742478] = 0;  // Honest: hand
    // keep
    p.keep[37742478] = 100;  // Honest
    p.keep[57774843] = 90;   // JD (summon condition)
    p.keep[44330098] = 70;   // Gorz
    p.keep[4906301] = 60;    // Necro Gardna (GY though)
    // dump
    p.dump[4906301] = 90;    // Necro Gardna
    p.dump[58996430] = 85;   // Wulf
    p.dump[33420078] = 80;
    p.dump[21502796] = 50;
    // tribute
    p.tribute[21502796] = 70;
    p.tribute[85087012] = 70;
    p.tribute[96235275] = 60;
    // traps
    p.act[44095762] = 60;
    p.act[53582587] = 60;
    p.act[41420027] = 65;
    p.act[64697231] = 70;
    p.act[29401950] = 60;
    // threats
    p.threat[93016201] = 90;
    p.threat[82732705] = 80;
    p.threat[81674782] = 80;
    p.threat[94853057] = 75;
    p.threat[42009836] = 85;
    return p;
}

static Policy make_policy_zombies() {
    Policy p;
    p.aggressiveness = 55;
    // recursion / mills
    p.act[2204140] = 85;     // Book of Life
    p.act[83764718] = 85;
    p.act[81439173] = 85;    // Foolish -> Mezuki/PSZ
    p.act[19613556] = 80;
    p.act[5318639] = 75;
    p.act[87910978] = 70;
    p.act[1475311] = 75;     // Allure
    p.act[67169062] = 70;
    p.act[60682203] = 65;
    p.act[14087893] = 65;
    p.act[31036355] = 70;    // Creature Swap
    // summons
    p.summon[63665875] = 85; // Goblin Zombie
    p.summon[77044671] = 85; // Pyramid Turtle
    p.summon[17259470] = 80; // Zombie Master
    p.summon[92826944] = 75; // Mezuki
    p.summon[33420078] = 80; // Plaguespreader
    p.summon[70595331] = 80; // Il Blud
    p.summon[65192027] = 75; // DAD
    p.summon[70095154] = 60;
    p.summon[23205979] = 55; // Spirit Reaper
    p.summon[26202165] = 65; // Sangan
    p.summon[21502796] = 60; // Ryko
    p.summon[85087012] = 60; // Card Trooper
    p.spsummon[92826944] = 85; // Mezuki revive
    p.spsummon[70595331] = 75; // Il Blud
    p.spsummon[17259470] = 75; // Zombie Master
    // keep
    p.keep[65192027] = 80;
    p.keep[44330098] = 70;
    p.keep[98777036] = 60;
    // dump
    p.dump[92826944] = 95;   // Mezuki
    p.dump[33420078] = 90;
    p.dump[63665875] = 60;
    p.dump[77044671] = 60;
    // tribute
    p.tribute[23205979] = 80;
    p.tribute[21502796] = 70;
    p.tribute[26202165] = 70;
    p.tribute[63665875] = 60;
    p.tribute[77044671] = 60;
    // traps
    p.act[29401950] = 65;
    p.act[44095762] = 60;
    p.act[53582587] = 60;
    p.act[41420027] = 65;
    p.act[64697231] = 70;
    p.act[63356631] = 60;
    p.act[97077563] = 65;
    // threats
    p.threat[93016201] = 90;
    p.threat[82732705] = 85;
    p.threat[81674782] = 85;
    p.threat[94853057] = 75;
    p.threat[42009836] = 85;
    return p;
}

static Policy make_policy_anti_meta_stun() {
    Policy p;
    p.aggressiveness = 65;
    // floodgates first
    p.act[81674782] = 95;    // Dimensional Fissure
    p.act[82732705] = 95;    // Skill Drain
    p.act[93016201] = 85;    // Royal Oppression
    p.act[29401950] = 80;    // Bottomless
    p.act[70342110] = 80;    // Dimensional Prison
    p.act[44095762] = 70;    // Mirror Force
    p.act[53582587] = 70;    // Torrential
    p.act[41420027] = 75;    // Solemn
    p.act[64697231] = 75;    // Dustshoot
    p.act[19613556] = 80;
    p.act[5318639] = 75;
    p.act[87910978] = 70;
    p.act[60682203] = 65;
    p.act[14087893] = 70;
    p.act[98045062] = 65;
    p.act[97169186] = 80;    // Smashing Ground
    p.act[69162969] = 75;    // Lightning Vortex
    p.act[83764718] = 75;
    // summons
    p.summon[42009836] = 95; // Fossil Dyna
    p.summon[71564252] = 90; // Rai-Oh
    p.summon[94853057] = 90; // Banisher
    p.summon[78700060] = 85; // Doomcaliber
    p.summon[88240808] = 80; // Kycoo
    p.summon[24317029] = 75; // Gravekeeper's Spy
    p.summon[17393207] = 70; // Commandant
    p.summon[71413901] = 65; // Breaker
    p.summon[70095154] = 55;
    p.spsummon[24317029] = 70; // Spy flips out
    // keep
    p.keep[44330098] = 70;
    // tribute
    p.tribute[24317029] = 60;
    p.tribute[17393207] = 70;
    p.tribute[70095154] = 60;
    p.tribute[71413901] = 50;
    // threats: what we fear
    p.threat[691925] = 85;   // Solar Recharge
    p.threat[94886282] = 85; // Charge
    p.threat[67723438] = 80; // E-Tele
    p.threat[20663556] = 70; // Substitoad
    p.threat[72714392] = 60; // Vayu
    return p;
}

// generic policy for arbitrary user decks: reasonable default priorities
// (summon monsters, activate removal, attack with strongest, dump nothing)
static Policy make_policy_generic() {
    Policy p;
    p.aggressiveness = 55;
    // generic spells everyone plays
    p.act[83764718] = 85;   // Monster Reborn
    p.act[19613556] = 85;   // Heavy Storm
    p.act[5318639] = 80;    // MST
    p.act[87910978] = 75;   // Brain Control
    p.act[37520316] = 70;   // Mind Control
    p.act[14087893] = 65;   // Book of Moon
    p.act[60682203] = 70;   // Cold Wave
    p.act[81439173] = 70;   // Foolish Burial
    p.act[1475311] = 70;    // Allure
    p.act[67169062] = 65;   // Pot of Avarice
    p.act[29401950] = 65;   // Bottomless
    p.act[44095762] = 60;   // Mirror Force
    p.act[53582587] = 60;   // Torrential
    p.act[41420027] = 65;   // Solemn
    p.act[97077563] = 65;   // Call of the Haunted
    // generic big monsters worth summoning
    p.summon[65192027] = 80;   // DAD
    p.summon[70095154] = 70;   // Cyber Dragon
    p.summon[44330098] = 65;   // Gorz (hand)
    p.spsummon[44330098] = 65;
    p.spsummon[98777036] = 60; // Tragoedia
    p.spsummon[23205979] = 50; // Spirit Reaper
    p.keep[44330098] = 70;
    p.keep[65192027] = 80;
    p.keep[37742478] = 80;   // Honest
    return p;
}

// build a policy from a deck name; empty/"generic" -> generic policy
static Policy make_policy(const std::string& deck) {
    if (deck == "vayu_turbo") return make_policy_vayu_turbo();
    if (deck == "blackwing") return make_policy_blackwing();
    if (deck == "frog_monarchs") return make_policy_frog_monarchs();
    if (deck == "lightsworn") return make_policy_lightsworn();
    if (deck == "zombies") return make_policy_zombies();
    if (deck == "anti_meta_stun") return make_policy_anti_meta_stun();
    return make_policy_generic();  // user decks / unknown
}

// ---------------------------------------------------------------------------
// minimal JSON serialization for policies (dump for the tuning UI, load the
// edited JSON back).  Format:
//   { "aggressiveness": 55, "always_attack": false,
//     "act": {"84013237": 90, ...}, "summon": {...}, "spsummon": {...},
//     "set": {...}, "dump": {...}, "keep": {...}, "tribute": {...},
//     "threat": {...} }
// ---------------------------------------------------------------------------
#include <sstream>
#include <cstdio>
#include <cctype>

static std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        default: o += c;
        }
    }
    return o;
}

static std::string policy_to_json(const Policy& p) {
    auto map_str = [](const char* name, const std::map<uint32_t, int>& m) {
        std::ostringstream o;
        o << "  \"" << name << "\": {";
        bool first = true;
        for (auto& kv : m) {
            o << (first ? "" : ", ") << "\"" << kv.first << "\": " << kv.second;
            first = false;
        }
        o << "}";
        return o.str();
    };
    std::ostringstream o;
    o << "{\n";
    o << "  \"aggressiveness\": " << p.aggressiveness << ",\n";
    o << "  \"always_attack\": " << (p.always_attack ? "true" : "false") << ",\n";
    o << map_str("act", p.act) << ",\n";
    o << map_str("summon", p.summon) << ",\n";
    o << map_str("spsummon", p.spsummon) << ",\n";
    o << map_str("set", p.set) << ",\n";
    o << map_str("dump", p.dump) << ",\n";
    o << map_str("keep", p.keep) << ",\n";
    o << map_str("tribute", p.tribute) << ",\n";
    o << map_str("threat", p.threat) << "\n";
    o << "}";
    return o.str();
}

// tiny JSON object parser: only parses the policy shape (object of objects
// of ints, plus two scalar keys).  Returns false on malformed input.
static bool policy_from_json(const std::string& text, Policy& out) {
    size_t i = 0;
    auto skip_ws = [&]() { while (i < text.size() && isspace((unsigned char)text[i])) ++i; };
    auto parse_string = [&](std::string& out_s) {
        if (i >= text.size() || text[i] != '"') return false;
        ++i;
        std::string s;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < text.size()) { s += text[i + 1]; i += 2; }
            else s += text[i++];
        }
        if (i >= text.size()) return false;
        ++i;  // closing quote
        out_s = s;
        return true;
    };
    auto parse_int = [&](int& v) {
        skip_ws();
        long val = 0; bool neg = false; bool any = false;
        if (i < text.size() && text[i] == '-') { neg = true; ++i; }
        while (i < text.size() && isdigit((unsigned char)text[i])) { val = val * 10 + (text[i] - '0'); ++i; any = true; }
        if (!any) return false;
        v = (int)(neg ? -val : val);
        return true;
    };
    auto parse_bool = [&](bool& v) {
        skip_ws();
        if (text.compare(i, 4, "true") == 0) { v = true; i += 4; return true; }
        if (text.compare(i, 5, "false") == 0) { v = false; i += 5; return true; }
        return false;
    };

    skip_ws();
    if (i >= text.size() || text[i] != '{') return false;
    ++i;
    while (true) {
        skip_ws();
        if (i < text.size() && text[i] == '}') { ++i; break; }
        std::string key;
        if (!parse_string(key)) return false;
        skip_ws();
        if (i >= text.size() || text[i] != ':') return false;
        ++i;
        skip_ws();
        if (key == "aggressiveness") {
            int v; if (!parse_int(v)) return false; out.aggressiveness = v;
        } else if (key == "always_attack") {
            bool v; if (!parse_bool(v)) return false; out.always_attack = v;
        } else {
            // category object: {"code": int, ...}
            if (i >= text.size() || text[i] != '{') return false;
            ++i;
            std::map<uint32_t, int>* target = nullptr;
            if (key == "act") target = &out.act;
            else if (key == "summon") target = &out.summon;
            else if (key == "spsummon") target = &out.spsummon;
            else if (key == "set") target = &out.set;
            else if (key == "dump") target = &out.dump;
            else if (key == "keep") target = &out.keep;
            else if (key == "tribute") target = &out.tribute;
            else if (key == "threat") target = &out.threat;
            while (true) {
                skip_ws();
                if (i < text.size() && text[i] == '}') { ++i; break; }
                std::string code_s;
                if (!parse_string(code_s)) return false;
                skip_ws();
                if (i >= text.size() || text[i] != ':') return false;
                ++i;
                int v; if (!parse_int(v)) return false;
                if (target) {
                    unsigned long code = strtoul(code_s.c_str(), nullptr, 10);
                    if (code > 0 && code <= 0xfffffffful)
                        (*target)[(uint32_t)code] = v;
                }
                skip_ws();
                if (i < text.size() && text[i] == ',') { ++i; continue; }
                if (i < text.size() && text[i] == '}') { ++i; break; }
                return false;
            }
        }
        skip_ws();
        if (i < text.size() && text[i] == ',') { ++i; continue; }
        if (i < text.size() && text[i] == '}') { ++i; break; }
        return false;
    }
    return true;
}
