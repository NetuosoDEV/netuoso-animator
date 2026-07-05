#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_set>

using namespace geode::prelude;

struct Keyframe {
    float t{}, x{}, y{}, r{}, sx{1.f}, sy{1.f};
};

class AnimState {
public:
    std::map<int, std::vector<Keyframe>> tracks;
    std::unordered_set<int> assignedGroups;
    std::map<int, int> objGroup;
    float playhead = 0.f;
    bool recording = false;
    std::string levelKey;

    static AnimState& get() {
        static AnimState s;
        return s;
    }

    static std::filesystem::path fileFor(std::string const& key) {
        auto dir = Mod::get()->getSaveDir() / "anims";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string safe;
        for (char c : key) safe += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
        if (safe.empty()) safe = "unnamed";
        return dir / (safe + ".txt");
    }

    void reset(std::string const& key) {
        tracks.clear();
        assignedGroups.clear();
        objGroup.clear();
        playhead = 0.f;
        recording = false;
        levelKey = key;
        std::ifstream in(fileFor(key));
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            int g;
            Keyframe k;
            if (ss >> g >> k.t >> k.x >> k.y >> k.r >> k.sx >> k.sy) {
                tracks[g].push_back(k);
                assignedGroups.insert(g);
            }
        }
        for (auto& [g, v] : tracks) sortKeys(v);
    }

    void save() {
        if (levelKey.empty()) return;
        std::ofstream out(fileFor(levelKey));
        for (auto& [g, v] : tracks)
            for (auto& k : v)
                out << g << ' ' << k.t << ' ' << k.x << ' ' << k.y << ' ' << k.r << ' ' << k.sx << ' ' << k.sy << '\n';
    }

    static void sortKeys(std::vector<Keyframe>& v) {
        std::sort(v.begin(), v.end(), [](Keyframe const& a, Keyframe const& b) { return a.t < b.t; });
    }

    void addKey(int g, Keyframe k) {
        auto& v = tracks[g];
        auto it = std::find_if(v.begin(), v.end(), [&](Keyframe const& o) { return std::fabs(o.t - k.t) < 0.001f; });
        if (it != v.end()) *it = k;
        else {
            v.push_back(k);
            sortKeys(v);
        }
        save();
    }

    void clearGroup(int g) {
        tracks.erase(g);
        save();
    }
};

static int ensureGroup(LevelEditorLayer* lel, GameObject* obj) {
    auto& st = AnimState::get();
    if (auto it = st.objGroup.find(obj->m_uniqueID); it != st.objGroup.end()) return it->second;
    if (obj->m_groupCount > 0 && obj->m_groups) {
        int g = (*obj->m_groups)[0];
        st.assignedGroups.insert(g);
        st.objGroup[obj->m_uniqueID] = g;
        return g;
    }
    int g = lel->getNextFreeGroupID(st.assignedGroups);
    if (g <= 0) {
        g = 1;
        while (st.assignedGroups.contains(g)) g++;
    }
    obj->addToGroup(g);
    lel->addToGroup(obj, g, false);
    st.assignedGroups.insert(g);
    st.objGroup[obj->m_uniqueID] = g;
    return g;
}

static GameObject* singleSelected(EditorUI* ui) {
    if (ui->m_selectedObject) return ui->m_selectedObject;
    if (ui->m_selectedObjects && ui->m_selectedObjects->count() == 1)
        return static_cast<GameObject*>(ui->m_selectedObjects->objectAtIndex(0));
    return nullptr;
}

static Keyframe snapOf(GameObject* obj, float t) {
    auto pos = obj->getPosition();
    return {t, pos.x, pos.y, obj->getRotation(), obj->getScaleX(), obj->getScaleY()};
}

class AnimPanel : public CCLayer {
protected:
    EditorUI* m_ui{};
    CCNode* m_header{};
    CCNode* m_ruler{};
    CCDrawNode* m_draw{};
    CCLabelBMFont* m_timeLbl{};
    CCSprite* m_recDot{};
    CCSprite* m_collapseSpr{};
    CCNode* m_tickHolder{};
    CCMenuItemSpriteExtra* m_keyBtn{};
    CCMenuItemSpriteExtra* m_genBtn{};
    CCLayerColor* m_headerBG{};
    CCLayerColor* m_topLine{};
    std::vector<std::pair<CCMenuItemSpriteExtra*, float>> m_btns;
    float m_w{}, m_rulerH = 44.f, m_headerH = 16.f;
    float m_x0 = 14.f, m_x1{};
    float m_viewStart = 0.f, m_span = 10.f;
    bool m_scrubbing = false;
    bool m_collapsed = false;

    bool init(EditorUI* ui) {
        if (!CCLayer::init()) return false;
        m_ui = ui;
        auto win = CCDirector::get()->getWinSize();
        m_w = win.width;
        m_x1 = m_w - 14.f;

        this->setPosition(0.f, 0.f);

        m_ruler = CCNode::create();
        auto rulerBG = CCLayerColor::create({21, 22, 27, 255}, m_w, m_rulerH);
        m_ruler->addChild(rulerBG, 0);
        m_draw = CCDrawNode::create();
        m_ruler->addChild(m_draw, 2);
        m_tickHolder = CCNode::create();
        m_ruler->addChild(m_tickHolder, 2);
        this->addChild(m_ruler, 1);

        m_header = CCNode::create();
        m_headerBG = CCLayerColor::create({32, 33, 41, 255}, m_w, m_headerH);
        m_header->addChild(m_headerBG, 0);
        auto line = CCLayerColor::create({70, 72, 86, 255}, m_w, 1.f);
        m_header->addChild(line, 1);
        m_topLine = CCLayerColor::create({70, 72, 86, 255}, m_w, 1.f);
        m_topLine->setPositionY(m_headerH - 1.f);
        m_header->addChild(m_topLine, 1);

        auto menu = CCMenu::create();
        menu->setPosition(0, 0);
        m_header->addChild(menu, 3);

        float by = m_headerH / 2, cx = m_w / 2;

        auto addBtn = [&](char const* png, float x, float scale, ccColor3B col, SEL_MenuHandler cb) -> CCMenuItemSpriteExtra* {
            auto spr = CCSprite::create(png);
            spr->setScale(scale);
            spr->setColor(col);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            btn->setPosition(x, by);
            menu->addChild(btn);
            m_btns.push_back({btn, 1.f});
            return btn;
        };

        m_timeLbl = CCLabelBMFont::create("0.00s", "chatFont.fnt");
        m_timeLbl->setScale(0.45f);
        m_timeLbl->setAnchorPoint({0.f, 0.5f});
        m_timeLbl->setPosition(10.f, by);
        m_header->addChild(m_timeLbl, 3);

        m_keyBtn = addBtn("key.png"_spr, 88.f, 0.46f, {255, 216, 74}, menu_selector(AnimPanel::onKey));
        auto recBtn = addBtn("record.png"_spr, cx - 39.f, 0.46f, {120, 120, 128}, menu_selector(AnimPanel::onRecord));
        m_recDot = static_cast<CCSprite*>(recBtn->getNormalImage());
        addBtn("prev.png"_spr, cx - 13.f, 0.46f, {215, 215, 222}, menu_selector(AnimPanel::onPrev));
        addBtn("next.png"_spr, cx + 13.f, 0.46f, {215, 215, 222}, menu_selector(AnimPanel::onNext));
        addBtn("trash.png"_spr, cx + 39.f, 0.46f, {215, 215, 222}, menu_selector(AnimPanel::onTrash));
        m_genBtn = addBtn("gen.png"_spr, m_w - 56.f, 0.44f, {140, 235, 130}, menu_selector(AnimPanel::onGenerate));
        auto colBtn = addBtn("collapse.png"_spr, m_w - 22.f, 0.42f, {200, 200, 210}, menu_selector(AnimPanel::onCollapse));
        m_collapseSpr = static_cast<CCSprite*>(colBtn->getNormalImage());

        this->layout();
        this->addChild(m_header, 2);

        this->setTouchEnabled(true);
        this->schedule(schedule_selector(AnimPanel::onTick), 0.15f);
        this->refresh();
        return true;
    }

    float headerH() {
        return m_collapsed ? 12.f : m_headerH;
    }

    void layout() {
        float f = m_collapsed ? 0.72f : 1.f;
        float h = this->headerH(), by = h / 2;
        m_ruler->setVisible(!m_collapsed);
        m_header->setPosition(0.f, m_collapsed ? 0.f : m_rulerH);
        m_headerBG->setContentSize({m_w, h});
        m_topLine->setPositionY(h - 1.f);
        for (auto& [btn, base] : m_btns) {
            btn->setScale(f);
            btn->setPositionY(by);
        }
        m_timeLbl->setScale(0.45f * f);
        m_timeLbl->setPositionY(by);
        m_collapseSpr->setFlipY(m_collapsed);
        m_keyBtn->setVisible(!m_collapsed);
        m_genBtn->setVisible(!m_collapsed);
    }

    void registerWithTouchDispatcher() override {
        CCTouchDispatcher::get()->addTargetedDelegate(this, -110, true);
    }

    bool active() {
        return this->isVisible() && m_ui && m_ui->isVisible();
    }

    float totalH() {
        return m_collapsed ? this->headerH() : m_rulerH + m_headerH;
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (!this->active()) return false;
        auto p = this->convertTouchToNodeSpace(touch);
        if (p.x < 0 || p.x > m_w || p.y < 0 || p.y > this->totalH()) return false;
        if (!m_collapsed && p.y < m_rulerH) {
            m_scrubbing = true;
            this->scrubTo(p.x);
        }
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (!m_scrubbing) return;
        this->scrubTo(this->convertTouchToNodeSpace(touch).x);
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        m_scrubbing = false;
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_scrubbing = false;
    }

    void scrubTo(float x) {
        float t = m_viewStart + (std::clamp(x, m_x0, m_x1) - m_x0) / (m_x1 - m_x0) * m_span;
        AnimState::get().playhead = std::max(0.f, std::round(t * 20.f) / 20.f);
        this->refresh();
    }

    void onTick(float) {
        if (this->active()) this->refresh();
    }

    int currentGroup() {
        auto obj = singleSelected(m_ui);
        if (!obj) return -1;
        auto& st = AnimState::get();
        if (auto it = st.objGroup.find(obj->m_uniqueID); it != st.objGroup.end()) return it->second;
        if (obj->m_groupCount > 0 && obj->m_groups) return (*obj->m_groups)[0];
        return -1;
    }

    void onRecord(CCObject*) {
        auto& st = AnimState::get();
        st.recording = !st.recording;
        this->refresh();
    }

    void onPrev(CCObject*) { this->step(-1.f); }
    void onNext(CCObject*) { this->step(1.f); }

    void step(float dir) {
        auto& st = AnimState::get();
        float s = static_cast<float>(Mod::get()->getSettingValue<double>("step-seconds"));
        st.playhead = std::max(0.f, st.playhead + dir * s);
        this->refresh();
    }

    void onTrash(CCObject*) {
        int g = this->currentGroup();
        if (g <= 0) {
            Notification::create("Select an animated object first", NotificationIcon::Warning)->show();
            return;
        }
        geode::createQuickPopup(
            "Are you sure?",
            "<cy>This will clear ABSOLUTELY ALL keyframes of the selected object</c>",
            "Cancel", "Clear",
            [this, g](FLAlertLayer*, bool btn2) {
                if (!btn2) return;
                AnimState::get().clearGroup(g);
                this->refresh();
            }
        );
    }

    void onKey(CCObject*) {
        this->keySelected();
    }

    void onCollapse(CCObject*) {
        m_collapsed = !m_collapsed;
        this->layout();
        this->refresh();
    }

    void onGenerate(CCObject*) {
        auto lel = m_ui->m_editorLayer;
        auto& st = AnimState::get();
        float K = static_cast<float>(Mod::get()->getSettingValue<double>("pos-multiplier"));
        std::string out;
        int n = 0;
        for (auto& [g, keys] : st.tracks) {
            for (size_t i = 0; i + 1 < keys.size(); ++i) {
                auto& a = keys[i];
                auto& b = keys[i + 1];
                float d = b.t - a.t;
                if (d < 0.01f) continue;
                float px = lel->posForTime(a.t).x;
                float dx = (b.x - a.x) * K, dy = (b.y - a.y) * K, dr = b.r - a.r;
                float fx = std::fabs(a.sx) > 1e-4f ? b.sx / a.sx : 1.f;
                float fy = std::fabs(a.sy) > 1e-4f ? b.sy / a.sy : 1.f;
                if (std::fabs(dx) > 0.001f || std::fabs(dy) > 0.001f) {
                    out += fmt::format("1,901,2,{:.2f},3,{:.2f},10,{:.3f},28,{:.3f},29,{:.3f},51,{},30,0;", px, a.y + 90.f, d, dx, dy, g);
                    n++;
                }
                if (std::fabs(dr) > 0.001f) {
                    out += fmt::format("1,1346,2,{:.2f},3,{:.2f},10,{:.3f},68,{:.3f},51,{},71,{};", px, a.y + 120.f, d, dr, g, g);
                    n++;
                }
                if (std::fabs(fx - 1.f) > 0.005f || std::fabs(fy - 1.f) > 0.005f) {
                    out += fmt::format("1,2067,2,{:.2f},3,{:.2f},10,{:.3f},51,{},71,{},150,{:.4f},151,{:.4f};", px, a.y + 150.f, d, g, g, fx, fy);
                    n++;
                }
            }
        }
        if (out.empty()) {
            Notification::create("No keyframe pairs to compile", NotificationIcon::Warning)->show();
            return;
        }

        std::map<int, GameObject*> byGroup;
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
            int fg = -1;
            bool tracked = false;
            if (auto it = st.objGroup.find(obj->m_uniqueID); it != st.objGroup.end()) {
                fg = it->second;
                tracked = true;
            }
            else if (obj->m_groupCount > 0 && obj->m_groups) fg = (*obj->m_groups)[0];
            if (fg > 0 && st.tracks.count(fg)) {
                if (tracked) byGroup[fg] = obj;
                else byGroup.emplace(fg, obj);
            }
        }
        for (auto& [g, keys] : st.tracks) {
            if (keys.empty()) continue;
            auto it = byGroup.find(g);
            if (it == byGroup.end()) continue;
            auto obj = it->second;
            auto& k0 = keys.front();
            m_ui->moveObject(obj, {k0.x - obj->getPositionX(), k0.y - obj->getPositionY()});
            obj->setRotation(k0.r);
            obj->setScaleX(k0.sx);
            obj->setScaleY(k0.sy);
            obj->m_scaleX = k0.sx;
            obj->m_scaleY = k0.sy;
        }

        lel->createObjectsFromString(out, true, true);
        Notification::create(fmt::format("Spawned {} triggers", n), NotificationIcon::Success)->show();
    }

    void drawDiamond(CCPoint c, float s, ccColor4F col) {
        CCPoint pts[4] = {{c.x, c.y + s}, {c.x + s, c.y}, {c.x, c.y - s}, {c.x - s, c.y}};
        m_draw->drawPolygon(pts, 4, col, 0.f, col);
    }

public:
    static AnimPanel* create(EditorUI* ui) {
        auto ret = new AnimPanel();
        if (ret->init(ui)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void keySelected() {
        auto obj = singleSelected(m_ui);
        if (!obj) {
            Notification::create("Select exactly one object", NotificationIcon::Warning)->show();
            return;
        }
        this->recordKey(obj);
    }

    void recordKey(GameObject* obj) {
        auto& st = AnimState::get();
        int g = ensureGroup(m_ui->m_editorLayer, obj);
        st.addKey(g, snapOf(obj, st.playhead));
        this->refresh();
    }

    void refresh() {
        auto& st = AnimState::get();

        if (st.playhead < m_viewStart) m_viewStart = st.playhead;
        if (st.playhead > m_viewStart + m_span) m_viewStart = st.playhead - m_span;
        m_viewStart = std::max(0.f, m_viewStart);

        int g = this->currentGroup();
        m_timeLbl->setString(fmt::format("{:.2f}s{}", st.playhead, g > 0 ? fmt::format("  g{}", g) : "").c_str());
        m_recDot->setColor(st.recording ? ccColor3B{235, 45, 45} : ccColor3B{120, 120, 128});

        m_draw->clear();
        m_tickHolder->removeAllChildren();
        if (m_collapsed) return;

        float ry = 18.f;
        m_draw->drawSegment({m_x0, ry}, {m_x1, ry}, 0.6f, {1, 1, 1, 0.22f});

        float t0 = m_viewStart;
        for (float t = std::ceil(t0 * 2) / 2; t <= t0 + m_span + 0.01f; t += 0.5f) {
            float x = m_x0 + (t - t0) / m_span * (m_x1 - m_x0);
            if (x < m_x0 - 0.5f || x > m_x1 + 0.5f) continue;
            bool whole = std::fabs(t - std::round(t)) < 0.01f;
            m_draw->drawSegment({x, ry - (whole ? 7.f : 4.f)}, {x, ry + (whole ? 7.f : 4.f)}, 0.5f, {1, 1, 1, whole ? 0.4f : 0.22f});
            if (whole) {
                auto lbl = CCLabelBMFont::create(fmt::format("{}", static_cast<int>(std::round(t))).c_str(), "chatFont.fnt");
                lbl->setScale(0.38f);
                lbl->setOpacity(120);
                lbl->setPosition(x, ry + 13.f);
                m_tickHolder->addChild(lbl);
            }
        }

        if (g > 0) {
            auto it = st.tracks.find(g);
            if (it != st.tracks.end()) {
                for (auto& k : it->second) {
                    if (k.t < t0 || k.t > t0 + m_span) continue;
                    float x = m_x0 + (k.t - t0) / m_span * (m_x1 - m_x0);
                    bool at = std::fabs(k.t - st.playhead) < 0.01f;
                    this->drawDiamond({x, ry}, 4.5f, at ? ccColor4F{1.f, 0.85f, 0.2f, 1.f} : ccColor4F{0.78f, 0.78f, 0.82f, 0.95f});
                }
            }
        }

        float px = m_x0 + (st.playhead - t0) / m_span * (m_x1 - m_x0);
        m_draw->drawSegment({px, 3.f}, {px, m_rulerH - 3.f}, 1.f, {0.35f, 0.65f, 1.f, 1.f});
        CCPoint tri[3] = {{px - 5.f, m_rulerH - 3.f}, {px + 5.f, m_rulerH - 3.f}, {px, m_rulerH - 12.f}};
        m_draw->drawPolygon(tri, 3, {0.35f, 0.65f, 1.f, 1.f}, 0.f, {0.35f, 0.65f, 1.f, 1.f});
    }
};

class $modify(AnimEditorUI, EditorUI) {
    struct Fields {
        AnimPanel* panel = nullptr;
        int lastUid = 0;
        Keyframe lastSnap{};
    };

    bool init(LevelEditorLayer* lel) {
        if (!EditorUI::init(lel)) return false;

        auto lvl = lel->m_level;
        AnimState::get().reset(lvl ? std::string(lvl->m_levelName) : "unnamed");

        m_fields->panel = AnimPanel::create(this);
        this->addChild(m_fields->panel, 500);
        return true;
    }

    void keyDown(enumKeyCodes key, double timestamp) {
        if (key == enumKeyCodes::KEY_I && m_fields->panel) {
            m_fields->panel->keySelected();
            return;
        }
        EditorUI::keyDown(key, timestamp);
    }

    void ccTouchEnded(CCTouch* touch, CCEvent* event) {
        EditorUI::ccTouchEnded(touch, event);
        this->maybeRecord();
    }

    void moveObjectCall(EditCommand command) {
        EditorUI::moveObjectCall(command);
        this->maybeRecord();
    }

    void transformObjectCall(EditCommand command) {
        EditorUI::transformObjectCall(command);
        this->maybeRecord();
    }

    void maybeRecord() {
        auto& st = AnimState::get();
        auto p = m_fields->panel;
        if (!p || !st.recording) return;
        auto obj = singleSelected(this);
        if (!obj) return;
        Keyframe cur = snapOf(obj, st.playhead);
        if (obj->m_uniqueID != m_fields->lastUid) {
            m_fields->lastUid = obj->m_uniqueID;
            m_fields->lastSnap = cur;
            return;
        }
        auto& l = m_fields->lastSnap;
        if (std::fabs(cur.x - l.x) < 0.01f && std::fabs(cur.y - l.y) < 0.01f && std::fabs(cur.r - l.r) < 0.01f
            && std::fabs(cur.sx - l.sx) < 0.001f && std::fabs(cur.sy - l.sy) < 0.001f) return;
        m_fields->lastSnap = cur;
        p->recordKey(obj);
    }
};
