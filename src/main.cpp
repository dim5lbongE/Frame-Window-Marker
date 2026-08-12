#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

namespace {
struct Marker {
    float x = 0.f;
    float y = 0.f;
    int frames = 1;
};

std::string levelKey(GJGameLevel* level) {
    if (!level) return "unknown";
    auto id = level->m_levelID.value();
    if (id != 0) return fmt::format("level-{}", id);
    return fmt::format("local-{}", level->m_levelName);
}

std::vector<Marker> loadMarkers(GJGameLevel* level) {
    std::vector<Marker> out;
    auto root = Mod::get()->getSavedValue<matjson::Value>(levelKey(level), matjson::Value::array());
    if (!root.isArray()) return out;
    for (auto const& item : root) {
        auto x = item["x"].asDouble();
        auto y = item["y"].asDouble();
        auto frames = item["frames"].asInt();
        if (x.isOk() && y.isOk() && frames.isOk()) {
            out.push_back({
                static_cast<float>(x.unwrap()),
                static_cast<float>(y.unwrap()),
                std::clamp(static_cast<int>(frames.unwrap()), 1, 10)
            });
        }
    }
    return out;
}

void saveMarkers(GJGameLevel* level, std::vector<Marker> const& markers) {
    matjson::Value root = matjson::Value::array();
    for (auto const& marker : markers) {
        matjson::Value item = matjson::Value::object();
        item["x"] = marker.x;
        item["y"] = marker.y;
        item["frames"] = marker.frames;
        root.push(item);
    }
    Mod::get()->setSavedValue(levelKey(level), root);
}
}

class $modify(FrameWindowMarkerLayer, PlayLayer) {
    struct Fields {
        std::vector<Marker> markers;
        std::vector<CCNode*> markerNodes;
        std::vector<CCLabelBMFont*> summaryLabels;
        std::array<bool, 11> keyWasDown{};
        bool deleteWasDown = false;
        bool toggleWasDown = false;
        bool startWasDown = false;
        bool endWasDown = false;
        bool analysisVisible = false;
        bool measurementActive = false;
        uint64_t gameTick = 0;
        uint64_t measurementStartTick = 0;
        CCPoint measurementStartPosition{0.f, 0.f};
        CCLabelBMFont* status = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        m_fields->markers = loadMarkers(level);
        m_fields->status = CCLabelBMFont::create("F6 START / F7 END / F8 ANALYSIS", "bigFont.fnt");
        m_fields->status->setScale(.32f);
        m_fields->status->setOpacity(150);
        m_fields->status->setAnchorPoint({0.f, 1.f});
        m_fields->status->setPosition({6.f, CCDirector::sharedDirector()->getWinSize().height - 6.f});
        m_fields->status->setID("fwm-status"_spr);
        this->addChild(m_fields->status, 1000);
        rebuildAnalysis();
        return true;
    }

    void update(float dt) {
        PlayLayer::update(dt);
        ++m_fields->gameTick;
#ifdef GEODE_IS_WINDOWS
        bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        for (int frames = 1; frames <= 10; ++frames) {
            int vk = frames == 10 ? '0' : ('0' + frames);
            bool down = alt && ((GetAsyncKeyState(vk) & 0x8000) != 0);
            if (down && !m_fields->keyWasDown[frames]) placeMarker(frames);
            m_fields->keyWasDown[frames] = down;
        }

        bool deleteDown = alt && ((GetAsyncKeyState(VK_DELETE) & 0x8000) != 0);
        if (deleteDown && !m_fields->deleteWasDown) deleteNearest();
        m_fields->deleteWasDown = deleteDown;

        bool toggleDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (toggleDown && !m_fields->toggleWasDown) toggleAnalysis();
        m_fields->toggleWasDown = toggleDown;

        bool startDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (startDown && !m_fields->startWasDown) startMeasurement();
        m_fields->startWasDown = startDown;

        bool endDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (endDown && !m_fields->endWasDown) endMeasurement();
        m_fields->endWasDown = endDown;
#endif

        if (m_fields->measurementActive && m_fields->status) {
            auto frames = currentMeasurementFrames();
            m_fields->status->setString(fmt::format("MARKING: {}F  [F7 END]", frames).c_str());
            m_fields->status->setColor({255, 220, 60});
            m_fields->status->setOpacity(255);
        }
    }

    int currentMeasurementFrames() {
        if (!m_fields->measurementActive) return 0;
        return static_cast<int>(m_fields->gameTick - m_fields->measurementStartTick + 1);
    }

    void startMeasurement() {
        if (!m_player1) {
            showStatus("Cannot start: no player", {255, 100, 100});
            return;
        }
        m_fields->measurementActive = true;
        m_fields->measurementStartTick = m_fields->gameTick;
        m_fields->measurementStartPosition = m_player1->getPosition();
        showStatus("MARKING START: 1F", {255, 220, 60});
    }

    void endMeasurement() {
        if (!m_fields->measurementActive) {
            showStatus("Press F6 first", {255, 100, 100});
            return;
        }

        int frames = currentMeasurementFrames();
        auto position = m_fields->measurementStartPosition;
        m_fields->measurementActive = false;

        if (frames < 1 || frames > 10) {
            showStatus(fmt::format("Not saved: {}F (allowed 1-10F)", frames), {255, 100, 100});
            return;
        }
        saveMarkerAt(position, frames, true);
    }

    void showStatus(std::string const& text, ccColor3B color = {255, 255, 255}) {
        if (!m_fields->status) return;
        m_fields->status->setString(text.c_str());
        m_fields->status->setColor(color);
        m_fields->status->stopAllActions();
        m_fields->status->setOpacity(255);
        m_fields->status->runAction(CCFadeTo::create(1.2f, 150));
    }

    void placeMarker(int frames) {
        if (!m_player1) return;
        saveMarkerAt(m_player1->getPosition(), frames, false);
    }

    void saveMarkerAt(CCPoint position, int frames, bool measured) {

        Marker* nearest = nullptr;
        float nearestDistance = 32.f;
        for (auto& marker : m_fields->markers) {
            float distance = std::abs(marker.x - position.x);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearest = &marker;
            }
        }

        if (nearest) {
            nearest->x = position.x;
            nearest->y = position.y;
            nearest->frames = frames;
            showStatus(fmt::format("{} changed: {}F", measured ? "Measured marker" : "Marker", frames), {255, 235, 80});
        } else {
            m_fields->markers.push_back({position.x, position.y, frames});
            showStatus(fmt::format("{} saved: {}F", measured ? "Measured marker" : "Marker", frames), {100, 255, 120});
        }
        saveMarkers(m_level, m_fields->markers);
        rebuildAnalysis();
    }

    void deleteNearest() {
        if (!m_player1 || m_fields->markers.empty()) return;
        auto playerX = m_player1->getPositionX();
        auto best = m_fields->markers.end();
        float nearestDistance = 80.f;
        for (auto it = m_fields->markers.begin(); it != m_fields->markers.end(); ++it) {
            float distance = std::abs(it->x - playerX);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                best = it;
            }
        }
        if (best == m_fields->markers.end()) {
            showStatus("No marker nearby", {255, 100, 100});
            return;
        }
        int frames = best->frames;
        m_fields->markers.erase(best);
        saveMarkers(m_level, m_fields->markers);
        rebuildAnalysis();
        showStatus(fmt::format("Deleted: {}F", frames), {255, 100, 100});
    }

    ccColor3B markerColor(int frames) {
        if (frames == 1) return {255, 70, 60};
        if (frames == 2) return {255, 155, 35};
        if (frames == 3) return {255, 225, 55};
        if (frames == 4) return {245, 245, 245};
        if (frames <= 6) return {105, 240, 115};
        if (frames <= 8) return {85, 205, 255};
        return {130, 115, 255};
    }

    void toggleAnalysis() {
        m_fields->analysisVisible = !m_fields->analysisVisible;
        rebuildAnalysis();
        showStatus(
            m_fields->analysisVisible ? "Frame analysis: ON" : "Frame analysis: OFF",
            m_fields->analysisVisible ? ccColor3B{100, 255, 120} : ccColor3B{220, 220, 220}
        );
    }

    void rebuildAnalysis() {
        for (auto* node : m_fields->markerNodes) {
            if (node) node->removeFromParent();
        }
        m_fields->markerNodes.clear();
        for (auto* label : m_fields->summaryLabels) {
            if (label) label->removeFromParent();
        }
        m_fields->summaryLabels.clear();

        if (!m_fields->analysisVisible) return;

        for (auto const& marker : m_fields->markers) {
            auto* node = CCNode::create();
            node->setPosition({marker.x, marker.y + 38.f});
            node->setID("fwm-marker"_spr);

            auto* ring = CCDrawNode::create();
            ring->drawDot({0.f, 0.f}, 18.f, {1.f, 1.f, 1.f, .95f});
            ring->drawDot({0.f, 0.f}, 13.f, {0.05f, 0.05f, 0.05f, 1.f});
            auto color = markerColor(marker.frames);
            ring->drawDot({0.f, 0.f}, 10.f, {
                color.r / 255.f, color.g / 255.f, color.b / 255.f, 1.f
            });
            node->addChild(ring);

            auto* label = CCLabelBMFont::create(fmt::format("{}", marker.frames).c_str(), "bigFont.fnt");
            label->setScale(.45f);
            label->setPosition({0.f, 0.f});
            node->addChild(label, 2);
            m_objectLayer->addChild(node, 1000);
            m_fields->markerNodes.push_back(node);
        }

        std::array<int, 11> counts{};
        for (auto const& marker : m_fields->markers) counts[marker.frames]++;
        struct Row { char const* name; int count; int colorFrame; };
        std::array<Row, 7> rows{{
            {"9-10", counts[9] + counts[10], 9},
            {"7-8", counts[7] + counts[8], 7},
            {"5-6", counts[5] + counts[6], 5},
            {"4", counts[4], 4},
            {"3", counts[3], 3},
            {"2", counts[2], 2},
            {"1", counts[1], 1}
        }};

        auto win = CCDirector::sharedDirector()->getWinSize();
        float y = win.height - 24.f;
        for (auto const& row : rows) {
            auto* label = CCLabelBMFont::create(
                fmt::format("{}:  {}", row.name, row.count).c_str(), "bigFont.fnt"
            );
            label->setScale(.45f);
            label->setAnchorPoint({0.f, .5f});
            label->setPosition({7.f, y});
            label->setColor(markerColor(row.colorFrame));
            label->setID("fwm-summary"_spr);
            this->addChild(label, 1001);
            m_fields->summaryLabels.push_back(label);
            y -= 23.f;
        }
    }
};
