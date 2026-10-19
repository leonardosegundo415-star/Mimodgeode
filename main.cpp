// Predictive Input Buffer (Mobile) - recreación desde cero.
// Hooks usados: GJBaseGameLayer::handleButton / playerTouchedRing,
// PlayerObject::update, PlayLayer::resetLevel / onQuit, PauseLayer::customSetup.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;

namespace pb {

    struct Slot {
        bool held = false;            // el juego cree que el botón está pulsado
        bool pendingRelease = false;  // release retrasado
        bool consumed = false;        // este press ya se reutilizó en un orbe
        int64_t pressTick = -1000000;
        int64_t releaseTick = 0;
        int limit = 0;
        void* lastRing = nullptr;
        int64_t lastRingTick = -1000000;
    };

    inline int64_t g_tick = 0;
    inline Slot g_slots[2];            // 0 = jugador 1, 1 = jugador 2
    inline bool g_bypass = false;      // true mientras inyectamos inputs propios
    inline int g_allLimit = 2;
    inline int g_waveLimit = 2;

    inline void applyStage(std::string const& s) {
        int a = 2, w = 2;
        if (s == "4/4") { a = 4; w = 4; }
        else if (s == "8/8") { a = 8; w = 8; }
        else if (s == "16/8") { a = 16; w = 8; }
        else if (s == "OFF") { a = 0; w = 0; }
        g_allLimit = a;
        g_waveLimit = w;
    }

    inline void loadSetting() {
        applyStage(Mod::get()->getSettingValue<std::string>("stage"));
    }

    inline std::string nextStage(std::string const& cur) {
        static const char* order[] = {"2/2", "4/4", "8/8", "16/8", "OFF"};
        for (int i = 0; i < 5; ++i) {
            if (cur == order[i]) return order[(i + 1) % 5];
        }
        return "2/2";
    }

    inline bool enabled() { return g_allLimit > 0 || g_waveLimit > 0; }

    inline void reset() {
        g_tick = 0;
        for (auto& s : g_slots) s = Slot{};
        g_bypass = false;
        loadSetting();
    }

    inline int limitFor(bool p1) {
        auto pl = PlayLayer::get();
        if (!pl) return 0;
        auto p = p1 ? pl->m_player1 : pl->m_player2;
        return (p && p->m_isDart) ? g_waveLimit : g_allLimit;
    }

    // Ejecuta los releases retrasados cuyo momento ya llegó.
    inline void flushPending(GJBaseGameLayer* layer) {
        for (int i = 0; i < 2; ++i) {
            auto& s = g_slots[i];
            if (s.pendingRelease && g_tick >= s.releaseTick) {
                s.pendingRelease = false;
                s.held = false;
                g_bypass = true;
                layer->handleButton(false, 1, i == 0);
                g_bypass = false;
            }
        }
    }
}

class $modify(PBGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        GJBaseGameLayer* self = this;
        if (pb::g_bypass || button != 1 || !pl || static_cast<GJBaseGameLayer*>(pl) != self) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto& s = pb::g_slots[isPlayer1 ? 0 : 1];
        int limit = pb::limitFor(isPlayer1);

        // Buffer apagado: pasa todo tal cual, soltando antes lo pendiente.
        if (limit <= 0) {
            bool wasPending = s.pendingRelease;
            s.pendingRelease = false;
            if (wasPending && down) GJBaseGameLayer::handleButton(false, 1, isPlayer1);
            s.held = down;
            GJBaseGameLayer::handleButton(down, 1, isPlayer1);
            return;
        }

        if (down) {
            if (s.pendingRelease) {
                // Había un hold virtual: ciérralo con un release real antes del nuevo press.
                s.pendingRelease = false;
                GJBaseGameLayer::handleButton(false, 1, isPlayer1);
            }
            s.held = true;
            s.consumed = false;
            s.pressTick = pb::g_tick;
            s.limit = limit;
            GJBaseGameLayer::handleButton(true, 1, isPlayer1);
        } else {
            if (!s.held) {
                GJBaseGameLayer::handleButton(false, 1, isPlayer1);
                return;
            }
            int64_t age = pb::g_tick - s.pressTick;
            if (age < s.limit) {
                // Toque más corto que el buffer: mantén el hold hasta completar los ticks.
                s.pendingRelease = true;
                s.releaseTick = s.pressTick + s.limit;
            } else {
                s.held = false;
                s.pendingRelease = false;
                GJBaseGameLayer::handleButton(false, 1, isPlayer1);
            }
        }
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        GJBaseGameLayer::playerTouchedRing(player, ring);

        if (pb::g_bypass || !pb::enabled()) return;
        auto pl = PlayLayer::get();
        GJBaseGameLayer* self = this;
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != self) return;

        bool p1 = (player == m_player1);
        auto& s = pb::g_slots[p1 ? 0 : 1];

        // ¿Es un contacto nuevo con este orbe?
        bool newContact = s.lastRing != static_cast<void*>(ring) || (pb::g_tick - s.lastRingTick) > 1;
        s.lastRing = ring;
        s.lastRingTick = pb::g_tick;

        if (!newContact || !s.held || s.consumed) return;
        if (s.pressTick >= pb::g_tick) return;                 // el press fue durante/después del contacto
        if (pb::g_tick - s.pressTick > s.limit) return;        // press demasiado viejo

        // El press llegó unos ticks antes de tocar el orbe: repítelo ahora.
        s.consumed = true;
        pb::g_bypass = true;
        GJBaseGameLayer::handleButton(false, 1, p1);
        GJBaseGameLayer::handleButton(true, 1, p1);
        pb::g_bypass = false;
    }
};

class $modify(PBPlayer, PlayerObject) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        if (pl && this == pl->m_player1) {
            pb::g_tick++;
            pb::flushPending(pl);
        }
        PlayerObject::update(dt);
    }
};

class $modify(PBPlayLayer, PlayLayer) {
    void resetLevel() {
        pb::reset();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        pb::reset();
        PlayLayer::onQuit();
    }
};

class $modify(PBPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto stage = Mod::get()->getSettingValue<std::string>("stage");
        auto spr = ButtonSprite::create(("Buffer: " + stage).c_str());
        spr->setScale(0.7f);

        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PBPause::onBufferCycle));
        auto menu = CCMenu::create();
        menu->setID("predictive-buffer-menu"_spr);
        menu->addChild(btn);

        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 70.f, 30.f});
        this->addChild(menu, 100);
    }

    void onBufferCycle(CCObject* sender) {
        auto next = pb::nextStage(Mod::get()->getSettingValue<std::string>("stage"));
        Mod::get()->setSettingValue<std::string>("stage", next);
        pb::applyStage(next);

        if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(sender)) {
            if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                spr->setString(("Buffer: " + next).c_str());
            }
        }
    }
};
