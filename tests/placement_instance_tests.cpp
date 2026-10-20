#include "framekeyboard/app.hpp"
#include "framekeyboard/feedback.hpp"
#include "framekeyboard/grip.hpp"
#include "framekeyboard/placement_store.hpp"
#include <fstream>

#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fk = framekeyboard;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void near(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-8, message);
}
struct TemporaryDirectory {
    fk::fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/fk-instance-XXXXXX";
        if (!mkdtemp(name.data())) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fk::fs::remove_all(path, ignored);
    }
};
fk::Transform head() {
    return {{{1, 0, 0, 1}, {0, 1, 0, 1.7}, {0, 0, 1, 2}}};
}
void dashboard_tab_tests() {
    fk::DashboardAnchor tracker;
    const fk::Transform main{{{1, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 1, -1}}};
    const fk::Transform bar{{{1, 0, 0, 0}, {0, 1, 0, .85}, {0, 0, 1, -.8}}};
    const fk::Transform moved_bar{{{0, 0, 1, 2}, {0, 1, 0, .9}, {-1, 0, 0, -3}}};
    require(tracker.update(main, bar) == main, "visible Steam tab calibrates bottom-center mount");
    auto expected = fk::move_with_dashboard(main, bar, moved_bar);
    auto followed = tracker.update({}, moved_bar);
    require(followed.has_value(), "app tabs retain a live dashboard anchor");
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near((*followed)[r][c], expected[r][c], "hidden main follows bar translation and yaw");
        }
    }
    require(!tracker.update({}, {}), "missing visible overlays supply no stale anchor");
    require(tracker.update({}, moved_bar) == followed, "temporary hide retains calibrated placement");
    fk::DashboardAnchor reopened;
    reopened.restore(followed, tracker.bar());
    auto returned = reopened.update({}, bar);
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near((*returned)[r][c], main[r][c], "reopening an app tab uses saved bar reference");
        }
    }
    fk::DashboardAnchor fresh;
    require(fresh.update({}, moved_bar) == moved_bar, "first app launch uses live bar fallback");
    require(fresh.update(main, bar) == bar, "tab switches keep the existing bar-relative mount");

    const double angle = 35 * std::numbers::pi / 180;
    const fk::Transform tilt{{{1, 0, 0, 0},
                              {0, std::cos(angle), -std::sin(angle), .4},
                              {0, std::sin(angle), std::cos(angle), 0}}};
    auto tilt_pose = [&](fk::Transform pose) {
        const auto original = pose;
        for (std::size_t col = 0; col < 4; ++col) {
            pose[1][col] = std::cos(angle) * original[1][col] - std::sin(angle) * original[2][col];
            pose[2][col] = std::sin(angle) * original[1][col] + std::cos(angle) * original[2][col];
        }
        pose[1][3] += .4;
        return pose;
    };
    const auto tilted_bar = tilt_pose(bar);
    fk::DashboardAnchor rotating;
    rotating.update(main, bar);
    auto tilted_mount = rotating.update({}, tilted_bar);
    const auto expected_mount = tilt_pose(main);
    fk::PanelPlacement keyboard;
    keyboard.recenter(head(), &main);
    const auto followed_keyboard = fk::move_with_dashboard(keyboard.transform(), main, *tilted_mount);
    const auto expected_keyboard = tilt_pose(keyboard.transform());
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near((*tilted_mount)[r][c], expected_mount[r][c], "app-tab anchor follows dashboard pitch");
            near(followed_keyboard[r][c], expected_keyboard[r][c],
                 "dashboard pitch carries keyboard rotation and positional offset together");
        }
    }
    require(rotating.update(expected_mount, tilted_bar) == tilted_mount,
            "switching to Steam preserves full-rotation reference");

    // The previous release saved a level bar, even though its mesh was tilted.
    auto old_bar = bar;
    old_bar[1][3] = tilted_bar[1][3];
    old_bar[2][3] = tilted_bar[2][3];
    fk::DashboardAnchor migrated;
    migrated.restore(main, old_bar, false);
    require(migrated.update({}, tilted_bar) == main, "legacy saves do not jump to the bar mesh tilt");
    const auto tilted_again = tilt_pose(tilted_bar);
    const auto after_migration = migrated.update({}, tilted_again);
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near((*after_migration)[r][c], expected_mount[r][c],
                 "migrated saves follow subsequent pitch");
        }
    }
    auto scaled_tilt = tilt;
    for (auto& row : scaled_tilt) {
        row[0] *= .7;
        row[1] *= .5;
        row[2] *= .3;
    }
    const auto normalized = fk::dashboard_anchor(scaled_tilt);
    require(normalized.has_value(), "scaled tilted dashboard remains usable");
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near((*normalized)[r][c], tilt[r][c], "scale removal preserves dashboard pitch");
        }
    }
}
void placement_tests() {
    fk::PanelPlacement resized;
    resized.set_width(.7);
    require(!resized.ready(), "restoring size alone must not make an old tracking pose usable");
    resized.recenter(head());
    near(resized.width(), .7, "recenter in a new tracking space preserves the restored size");
    fk::PanelPlacement panel;
    panel.recenter(head());
    near(panel.width(), .95, "default width is four 5 cm steps below 1.15 m");
    auto pose = panel.transform();
    near(pose[1][1], std::cos(50 * std::numbers::pi / 180),
         "default surface is tilted 50 degrees from upright");
    near(pose[1][2], std::sin(50 * std::numbers::pi / 180), "typing surface faces upward");
    require(pose[2][1] < 0, "top edge slopes away from the viewer");
    near(pose[0][3], 1, "centered horizontally");
    near(pose[1][3], 1.05, "fallback is 65 cm below eye level");
    near(pose[2][3], 1.15, "in front of headset");
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            double dot = 0;
            for (int row = 0; row < 3; ++row) {
                dot += pose[row][a] * pose[row][b];
            }
            near(dot, a == b ? 1 : 0, "recenter rotation is orthonormal");
        }
    }
    panel.adjust(fk::PlacementAction::Smaller);
    near(panel.width(), .9, "smaller removes 5 cm");
    panel.adjust(fk::PlacementAction::Larger);
    near(panel.width(), .95, "larger adds 5 cm");
    require(panel.transform() == pose, "resizing preserves the entire pose");
    for (int i = 0; i < 100; ++i) {
        panel.adjust(fk::PlacementAction::Smaller);
    }
    near(panel.width(), .45, "minimum width");
    for (int i = 0; i < 100; ++i) {
        panel.adjust(fk::PlacementAction::Larger);
    }
    near(panel.width(), 2, "maximum width");
    // A straight-down headset must retain the default desk tilt, not add head pitch.
    const fk::Transform looking_down{{{1, 0, 0, 0}, {0, 0, 1, 1.7}, {0, -1, 0, 0}}};
    panel.recenter(looking_down);
    pose = panel.transform();
    near(pose[1][1], std::cos(50 * std::numbers::pi / 180), "recenter ignores head pitch");
    near(pose[2][3], -.85, "vertical gaze uses stable horizontal heading");
    near(panel.width(), 2, "recenter preserves chosen size");
    const fk::Transform turned{{{0, 0, 1, 3}, {0, 1, 0, 1.7}, {-1, 0, 0, 4}}};
    panel.recenter(turned);
    pose = panel.transform();
    near(pose[0][3], 2.15, "recenter uses current heading");
    near(pose[2][3], 4, "recenter clears old position offsets");
    auto mount = head();
    mount[0][3] = .3;
    mount[1][3] = .55;
    mount[2][3] = -1;
    // Steam's parented overlay transform may include nonuniform scale.
    mount[0][0] = .7;
    mount[1][1] = .5;
    mount[2][2] = .3;
    const auto anchor = fk::dashboard_anchor(mount);
    require(anchor.has_value(), "scaled dashboard has a usable anchor");
    panel.recenter(head(), &*anchor);
    pose = panel.transform();
    near(pose[0][3], .3, "centered beneath the dashboard");
    near(pose[1][3] + panel.width() * .375 / 2 * pose[1][1], .55 - .06,
         "top edge leaves 6 cm below dashboard at any width");
    near(pose[2][3], -.76, "center is 24 cm toward viewer from dashboard bottom");
    near(pose[0][0], 1, "dashboard scale is not inherited");
    near(pose[1][2], std::sin(50 * std::numbers::pi / 180), "dashboard gets requested pitch");
    near(panel.width(), 2, "dashboard recenter preserves chosen width");
    const fk::Transform moved_dashboard{{{0, 0, 1, 5}, {0, 1, 0, 1}, {-1, 0, 0, -4}}};
    const auto followed = fk::move_with_dashboard(pose, *anchor, moved_dashboard);
    near(followed[0][3], 5.24, "dashboard translation and yaw carry keyboard offset");
    near(followed[2][3], -4, "keyboard stays centered after walking across room");
    near(followed[1][3] - 1, pose[1][3] - .55, "vertical custom offset is preserved");
    const auto returned = fk::move_with_dashboard(followed, moved_dashboard, *anchor);
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near(returned[r][c], pose[r][c], "dashboard movement preserves full relative pose");
        }
    }
    mount[0][3] = std::numeric_limits<double>::quiet_NaN();
    require(!fk::dashboard_anchor(mount), "invalid dashboard is rejected");
    require(!fk::dashboard_anchor({}), "missing dashboard cannot anchor at room origin");
}
void drag_tests() {
    fk::PanelPlacement panel;
    panel.recenter(head());
    panel.adjust(fk::PlacementAction::Larger);
    const auto original = panel.transform();
    fk::PanelDrag drag;
    drag.begin(head(), original);
    const auto no_jump = drag.update(head());
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near(no_jump[r][c], original[r][c], "grab does not jump");
        }
    }
    auto moved = head();
    moved[0][3] += .2;
    moved[2][3] -= .3;
    panel.set_transform(drag.update(moved));
    near(panel.transform()[0][3], original[0][3] + .2, "controller translation moves panel");
    near(panel.transform()[2][3], original[2][3] - .3, "controller depth moves panel");
    near(panel.width(), 1.0, "drag retains size");
    // A 90-degree controller turn rotates the original offset around that hand.
    moved = {{{0, 0, 1, 1}, {0, 1, 0, 1.7}, {-1, 0, 0, 2}}};
    const auto turned = drag.update(moved);
    near(turned[0][3], .15, "controller yaw rotates laser distance around hand");
    near(turned[2][3], 2, "rotation preserves grab distance");
    panel.set_transform(turned);
    panel.recenter(head());
    near(panel.transform()[0][3], 1, "recenter resets dragged position");
    near(panel.transform()[1][1], std::cos(50 * std::numbers::pi / 180),
         "recenter restores desk tilt after dragging");
}
void horizon_tests() {
    // A level pose with 30-degree yaw and 20-degree desk tilt, independent of
    // the default recenter angle. Horizon assistance must preserve both.
    const double yaw = 30 * std::numbers::pi / 180, pitch = -20 * std::numbers::pi / 180;
    const fk::Transform level{
        {{std::cos(yaw), std::sin(yaw) * std::sin(pitch), std::sin(yaw) * std::cos(pitch), 1},
         {0, std::cos(pitch), -std::sin(pitch), 1.05},
         {-std::sin(yaw), std::cos(yaw) * std::sin(pitch), std::cos(yaw) * std::cos(pitch), 1.15}}};
    auto rolled = [&](double degrees) {
        auto pose = level;
        const double angle = degrees * std::numbers::pi / 180;
        for (int r = 0; r < 3; ++r) {
            pose[r][0] = level[r][0] * std::cos(angle) + level[r][1] * std::sin(angle);
            pose[r][1] = -level[r][0] * std::sin(angle) + level[r][1] * std::cos(angle);
        }
        return pose;
    };
    auto roll_degrees = [](const fk::Transform& pose) {
        return std::atan2(pose[1][0], pose[1][1]) * 180 / std::numbers::pi;
    };
    for (double degrees : {-5., -2., 2., 5.}) {
        fk::HorizonAlignment horizon;
        const auto raw = rolled(degrees);
        near(roll_degrees(horizon.update(raw, 10)), degrees, "alignment starts without a jump");
        near(roll_degrees(horizon.update(raw, 10.125)), degrees * .84375, "500 ms smoothstep eases in");
        require(horizon.animating(), "horizon requests fast updates during easing");
        near(roll_degrees(horizon.update(raw, 10.25)), degrees * .5, "halfway roll");
        const auto aligned = horizon.update(raw, 10.5);
        require(!horizon.animating(), "completed easing permits idle polling");
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                near(aligned[r][c], level[r][c], "alignment preserves heading, pitch and position");
            }
        }
        near(roll_degrees(horizon.update(raw, 12)), 0, "stationary pose stays level after release");
        // Regrab the displayed pose, then cancel assistance. Neither may expose
        // the original raw roll or accumulate another rotation correction.
        fk::PanelDrag drag;
        drag.begin(head(), aligned);
        horizon.reset();
        near(roll_degrees(horizon.update(drag.update(head()), 13)), 0, "regrab stays level");
    }
    fk::HorizonAlignment grabbed;
    grabbed.update(rolled(4), 1);
    grabbed.update(rolled(4), 1.25);
    require(grabbed.update(rolled(3), 2, true) == rolled(3),
            "grab bypasses an existing alignment animation");
    require(grabbed.update(rolled(-4), 12, true) == rolled(-4),
            "long grab preserves exact hand pose within snapping threshold");
    require(!grabbed.animating(), "grab does not advance horizon easing");
    near(roll_degrees(grabbed.update(rolled(-4), 13)), -4, "release begins without a snap");
    near(roll_degrees(grabbed.update(rolled(-4), 13.25)), -2, "release eases halfway after 250 ms");
    near(roll_degrees(grabbed.update(rolled(-4), 13.5)), 0, "release levels after 500 ms");
    fk::HorizonAlignment disabled;
    disabled.update(rolled(4), 1);
    const auto visible = disabled.update(rolled(4), 1.25);
    near(roll_degrees(visible), 2, "toggle test starts during partial leveling");
    // The VR layer adopts this displayed pose when the setting changes.
    require(disabled.update(visible, 1.25, false, false) == visible &&
                disabled.update(visible, 12, false, false) == visible && !disabled.animating(),
            "disabling leveling freezes the visible pose and stops animation");
    require(disabled.update(rolled(-4), 13, true, false) == rolled(-4) &&
                disabled.update(rolled(-4), 14, false, false) == rolled(-4),
            "disabled leveling preserves exact hand and release angles");
    near(roll_degrees(disabled.update(rolled(-4), 15)), -4,
         "reenabling starts without stale correction or a jump");
    near(roll_degrees(disabled.update(rolled(-4), 15.5)), 0,
         "reenabling restores the normal release correction");
    fk::HorizonAlignment horizon;
    near(roll_degrees(horizon.update(rolled(5.01), 20)), 5.01, "outside range is untouched");
    horizon.update(rolled(4), 21);
    const auto partial = horizon.update(rolled(4), 21.25);
    near(roll_degrees(partial), 2, "partial correction");
    near(roll_degrees(horizon.update(rolled(6), 21.25)), 4,
         "leaving range retains correction initially");
    near(roll_degrees(horizon.update(rolled(6), 21.5)), 5, "leaving range eases correction away");
    near(roll_degrees(horizon.update(rolled(6), 21.75)), 6, "larger deliberate lean is restored");
    horizon.update(rolled(-4), 23);
    near(roll_degrees(horizon.update(rolled(-4), 23.5)), 0, "reentry aligns opposite roll");
    horizon.reset();
    near(roll_degrees(horizon.update(partial, 25)), 2, "cancel freezes visible intermediate pose");
    const fk::Transform flat{{{1, 0, 0, 0}, {0, 0, 1, 1}, {0, -1, 0, 2}}};
    require(horizon.update(flat, 26) == flat, "horizontal panel has no horizon roll");
    require(horizon.update(rolled(4), std::numeric_limits<double>::quiet_NaN()) == rolled(4),
            "invalid timing does not corrupt placement");
}
void stick_depth_tests() {
    // Tilt the component's neutral frame, as Frame's left/right models do.
    auto component = [](double x, double y) {
        const double a = y * 20 * std::numbers::pi / 180;
        const double b = x * 20 * std::numbers::pi / 180;
        fk::Transform pose{{{std::cos(b), 0, std::sin(b), 0},
                            {std::sin(a) * std::sin(b), std::cos(a), -std::sin(a) * std::cos(b), 0},
                            {-std::cos(a) * std::sin(b), std::sin(a), std::cos(a) * std::cos(b), 0}}};
        const double c = std::cos(.6), d = std::sin(.6);
        for (std::size_t col = 0; col < 3; ++col) {
            const double first = pose[0][col], second = pose[1][col];
            pose[0][col] = c * first - d * second;
            pose[1][col] = d * first + c * second;
        }
        return pose;
    };
    fk::ComponentAxis axis;
    require(axis.calibrate(component(0, 0), component(0, 1)), "stick calibration");
    for (double x : {-1., -.5, 0., .5, 1.}) {
        for (double y : {-1., -.5, 0., .5, 1.}) {
            near(axis.read(component(x, y)), y, "signed Y independent of sideways tilt");
        }
    }
    require(!axis.calibrate(component(0, 0), component(0, 0)), "static component refuses calibration");
    near(axis.read(component(0, 1)), 0, "failed calibration gives no motion");
    fk::PanelPlacement panel;
    panel.recenter(head());
    fk::PanelDrag drag;
    const auto original = panel.transform();
    drag.begin(head(), original);
    drag.move_depth(.19, .05);
    near(drag.update(head())[2][3], original[2][3], "stick dead zone has no drift");
    for (int i = 0; i < 20; ++i) {
        drag.move_depth(1, .05);
    }
    near(drag.update(head())[2][3], original[2][3] - .65, "up moves farther at time-based speed");
    for (int i = 0; i < 20; ++i) {
        drag.move_depth(-1, .05);
    }
    near(drag.update(head())[2][3], original[2][3], "down moves closer");
    for (int i = 0; i < 200; ++i) {
        drag.move_depth(-1, .05);
    }
    near(head()[2][3] - drag.update(head())[2][3], .2, "near limit prevents crossing the controller");
    for (int i = 0; i < 200; ++i) {
        drag.move_depth(1, .05);
    }
    near(head()[2][3] - drag.update(head())[2][3], 3, "far limit");
    drag.begin(head(), original);
    drag.move_depth(1, 100);
    near(drag.update(head())[2][3], original[2][3] - .0325, "stalled frame cannot jump depth");
    drag.begin(head(), original, {1, 0, 0});
    drag.move_depth(1, .05);
    near(drag.update(head())[0][3], original[0][3] + .0325, "depth follows calibrated laser direction");
    near(drag.update(head())[2][3], original[2][3], "sideways laser does not move along controller Z");
    near(drag.update(head())[1][1], original[1][1], "stick leaves orientation unchanged");
}
void persistence_tests() {
    TemporaryDirectory temp;
    const auto path = temp.path / "placement.json";
    require(!fk::load_placement(path), "first launch has no saved placement");
    fk::PanelPlacement original;
    // A dragged pose has arbitrary heading/tilt/roll and must survive resize
    // and save/restore without being reconstructed from the recenter defaults.
    const fk::Transform dragged{{{.8, -.6, 0, 1.2}, {0, 0, 1, 1.1}, {-.6, -.8, 0, 2.3}}};
    original.set_transform(dragged);
    original.adjust(fk::PlacementAction::Larger);
    require(original.transform() == dragged, "resizing preserves a dragged pose");
    fk::save_placement(path, {original.transform(), original.width(), 18446744073709551615ULL});
    const auto saved = fk::load_placement(path);
    require(saved && saved->universe == 18446744073709551615ULL, "universe ID retains all bits");
    fk::PanelPlacement reopened;
    reopened.restore(saved->transform, saved->width);
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            near(reopened.transform()[r][c], original.transform()[r][c], "reopen preserves pose");
        }
    }
    near(reopened.width(), original.width(), "reopen preserves width");
    reopened.recenter(head());
    near(reopened.transform()[1][1], std::cos(50 * std::numbers::pi / 180),
         "explicit relaunch restores default desk tilt");
    near(reopened.width(), original.width(), "recenter retains saved size");
    require(!saved->dashboard, "legacy world-space placement still loads");
    auto anchored = *saved;
    anchored.dashboard_full_rotation = true;
    anchored.dashboard = head();
    anchored.dashboard_bar = head();
    (*anchored.dashboard_bar)[1][3] -= .15;
    fk::save_placement(path, anchored);
    require(fk::load_placement(path)->dashboard == anchored.dashboard,
            "dashboard anchor survives close and reopen");
    require(fk::load_placement(path)->dashboard_bar == anchored.dashboard_bar,
            "dashboard bar reference survives close and reopen");
    require(fk::load_placement(path)->dashboard_full_rotation, "full-rotation marker persists");
    auto bad_anchor = anchored;
    (*bad_anchor.dashboard)[0][0] = 2;
    bool bad_anchor_rejected = false;
    try {
        fk::save_placement(path, bad_anchor);
    } catch (const std::exception&) {
        bad_anchor_rejected = true;
    }
    require(bad_anchor_rejected, "scaled saved dashboard anchor is rejected");
    auto invalid = *saved;
    invalid.transform[0][0] *= 2;
    bool rejected = false;
    try {
        fk::save_placement(path, invalid);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "scaled transform rejected");
    near(fk::load_placement(path)->transform[0][0], saved->transform[0][0],
         "failed save preserves prior file");
    for (const std::string bad : {"{", "{}", "{\"schema_version\":2}"}) {
        {
            std::ofstream file(path);
            file << bad;
        }
        rejected = false;
        try {
            fk::load_placement(path);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "corrupt or unsupported placement rejected");
    }
    fk::save_placement(path, *saved);
    require(fk::load_placement(path).has_value(), "fresh valid save replaces corrupt placement");
}
void key_feedback_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    fk::NullSink sink;
    fk::App app(options, sink);
    require(!app.down(0, -1, -1, 0), "background press has no key feedback");
    require(!app.down(0, 50, 33, 0), "toolbar press has no key feedback");
    require(!app.up(0, -1, -1), "toolbar release has no key feedback");
    app.cancel();
    const auto view = app.view();
    for (int y = 96; y < fk::panel_height; y += 2) {
        for (int x = 0; x < fk::panel_width; x += 2) {
            const auto* key = app.renderer.hit_key(view, x, y);
            if (!key || key->id != "KeyA") {
                continue;
            }
            require(app.move(0, x, y), "entering a key requests hover feedback");
            require(!app.move(0, x, y), "stationary hover does not buzz");
            require(app.move(1, x, y), "each pointer tracks hover independently");
            require(app.down(0, x, y, 0), "key press requests feedback even in input-disabled preview");
            require(!app.down(0, x, y, .01), "duplicate down does not repeat feedback");
            require(!app.move(0, -1, -1), "leaving a key does not pulse");
            require(!app.move(0, x, y), "held pointer does not generate hover pulses");
            require(app.up(0, -1, -1), "release outside captured key still clicks");
            require(!app.up(0, x, y), "duplicate release does not click");
            require(app.down(0, x, y, .02), "next press requests new feedback");
            app.set_dragging(true);
            require(!app.up(0, x, y), "canceled press has no release click");
            require(!app.move(1, x, y), "dragging suppresses hover feedback");
            require(!app.down(1, x, y, .03), "dragging suppresses key feedback");
            return;
        }
    }
    throw std::runtime_error("A key hit region missing");
}
void haptic_routing_tests() {
    fk::KeyHaptics feedback;
    using Pulse = std::pair<unsigned, fk::KeyFeedback>;
    feedback.hover(2);
    feedback.press(0, 1);
    feedback.hover(1);
    require(feedback.take(1) ==
                std::vector<Pulse>{{1, fk::KeyFeedback::Press}, {2, fk::KeyFeedback::Hover}},
            "click wins only on its controller; other hand keeps its hover");
    feedback.hover(1);
    require(feedback.take(1.01).empty(), "hover cannot extend a click into the next frame");
    feedback.hover(1);
    require(feedback.take(1.04) == std::vector<Pulse>{{1, fk::KeyFeedback::Hover}},
            "hover resumes after click");
    feedback.press(1, 2);
    feedback.release(0);
    require(feedback.take(2) ==
                std::vector<Pulse>{{1, fk::KeyFeedback::Release}, {2, fk::KeyFeedback::Press}},
            "release belongs to original press hand while other hand presses");
    feedback.release(0);
    require(feedback.take(3).empty(), "duplicate release cannot vibrate");
    feedback.press(0, 1);
    feedback.press(1, 2);
    feedback.cancel_pointer(0);
    require(feedback.take(3.5) == std::vector<Pulse>{{2, fk::KeyFeedback::Press}},
            "one controller leaving preserves the other controller's feedback");
    feedback.release(0);
    require(feedback.take(3.6).empty(), "canceled controller cannot emit a release pulse");
    feedback.cancel();
    feedback.release(1);
    require(feedback.take(4).empty(), "canceled key has no release pulse");
}
void grip_tests() {
    const auto released = head();
    auto pressed = released;
    const double radians = 9.5 * std::numbers::pi / 180;
    pressed[0][0] = std::cos(radians);
    pressed[0][1] = -std::sin(radians);
    pressed[1][0] = std::sin(radians);
    pressed[1][1] = std::cos(radians);
    pressed[2][3] += 10; // Component pivot/translation must not alter squeeze travel.
    near(fk::component_rotation_degrees(released, pressed), 9.5, "grip angular travel");
    near(fk::component_rotation_degrees(pressed, pressed), 0, "neutral orientation");
    // The reference orientation need not be axis-aligned in the controller model.
    near(fk::component_rotation_degrees(pressed, released), 9.5, "relative rotation");
    fk::GripLatch grip;
    require(!grip.update(9.5).held, "startup while squeezed cannot start a drag");
    require(!grip.update(0).held, "released sample arms grip");
    require(!grip.update(.8).held, "small travel cannot start a drag");
    auto state = grip.update(1.1);
    require(state.held && state.pressed, "squeeze emits a single rising edge");
    state = grip.update(.8);
    require(state.held && !state.pressed, "hysteresis keeps grip held near press threshold");
    require(grip.update(.55).held, "release dead zone avoids chatter");
    require(!grip.update(.4).held, "release ends capture");
    require(grip.update(2).pressed, "new squeeze captures again");
    require(!grip.update(std::nullopt).held, "missing input releases grip");
    require(!grip.update(2).held, "reconnect while held must wait for release");
    grip.update(0);
    require(grip.update(2).pressed, "release after reconnect re-arms grip");
    require(!grip.update(std::numeric_limits<double>::quiet_NaN()).held,
            "invalid component pose releases grip");
}
void trigger_release_tests() {
    fk::TriggerReleaseGuard release;
    release.begin(10);
    for (double now : {10., 10.05, 10.14, 10.16, 10.19}) {
        require(!release.update(0, now), "quick release waits for grace and repeated rest samples");
    }
    require(release.update(0, 10.22), "lost mouse-up recovers without ever seeing the trigger held");

    release.begin(20);
    require(!release.update(0, 20) && !release.update(0, 20.04),
            "stale initial rest samples cannot cancel a new capture");
    require(!release.update(12.5, 20.06) && !release.update(12.5, 30),
            "a real hold survives both startup and a long movement");
    require(release.update(0, 30.01), "observed hold retains immediate release recovery");
    release.begin(40);
    require(!release.update(12.5, 40.01) && release.update(0, 40.02),
            "an observed quick click releases even inside startup grace");

    release.begin(50);
    require(!release.update(0, 50.16) && !release.update(0, 50.19) && !release.update(.75, 50.2) &&
                !release.update(0, 50.21) && !release.update(0, 50.24) && release.update(0, 50.27),
            "partially pulled trigger interrupts the released-sample window");

    release.begin(60);
    require(!release.update(0, 60.16) && !release.update(std::nullopt, 60.19) &&
                !release.update(0, 60.22) &&
                !release.update(std::numeric_limits<double>::quiet_NaN(), 60.24) &&
                !release.update(0, 60.27) && !release.update(0, 60.29) && release.update(0, 60.33),
            "missing and invalid samples cannot count as continuously released input");

    release.begin(70);
    require(!release.update(0, 70.16) && !release.update(0, 71) && !release.update(0, 71.02) &&
                release.update(0, 71.06),
            "a polling gap requires a fresh sequence of released samples");

    release.begin(80);
    for (int i = 0; i < 60; ++i) {
        require(!release.update(std::nullopt, 80 + i / 60.),
                "unavailable trigger data never manufactures release");
    }
    release.update(12.5, 81);
    release.begin(90);
    require(!release.update(0, 90.01), "a new capture clears the preceding held state");
}
void instance_tests() {
    TemporaryDirectory temp;
    const auto runtime = temp.path / "runtime";
    {
        fk::VrInstance owner(runtime);
        require(owner.is_owner(), "first launch owns instance");
        {
            fk::VrInstance background(runtime, false);
            require(!background.is_owner() && !owner.poll_recenter(),
                    "duplicate boot startup leaves the existing panel hidden");
        }
        for (int repeat = 0; repeat < 3; ++repeat) {
            auto launch = std::async(std::launch::async, [&] {
                fk::VrInstance second(runtime);
                return second.is_owner();
            });
            bool received = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            do {
                received |= owner.poll_recenter();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (launch.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
                     std::chrono::steady_clock::now() < deadline);
            require(!launch.get(), "second launch never becomes a VR owner");
            require(received, "second launch delivers recenter request");
            require(fk::fs::exists(runtime / "vr.sock"), "client destruction leaves owner's socket");
        }
    }
    require(!fk::fs::exists(runtime / "vr.sock"), "owner removes endpoint at normal exit");
    require(fk::fs::exists(runtime / "vr.lock"), "lock inode must remain stable across launches");
    // Simulate abrupt process death. The next launch must recover the stale socket
    // using the kernel-released file lock, without killing or trusting a saved PID.
    const pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("fork failed");
    }
    if (child == 0) {
        try {
            fk::VrInstance crashed(runtime);
            _exit(crashed.is_owner() ? 0 : 2);
        } catch (...) {
            _exit(3);
        }
    }
    int status = 0;
    waitpid(child, &status, 0);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "crashed owner setup");
    require(fk::fs::exists(runtime / "vr.sock"), "abrupt exit leaves a stale endpoint");
    fk::VrInstance recovered(runtime);
    require(recovered.is_owner(), "new launch recovers after abrupt exit");
}
struct RecordingSink : fk::KeySink {
    std::vector<std::pair<int, int>> events;
    bool ready{true}, recoverable{}, reset{};
    bool can_resume() const override { return recoverable; }
    bool take_input_reset() override { return std::exchange(reset, false); }
    void send(int code, int value) override { events.emplace_back(code, value); }
    bool pump() override { return ready; }
};
void type_a(fk::App& app, bool release = true, const std::string& id = "KeyA") {
    // Find a real rendered key hit region, not a hardcoded toolbar coordinate.
    const auto view = app.view();
    for (int y = 96; y < fk::panel_height; y += 2) {
        for (int x = 0; x < fk::panel_width; x += 2) {
            auto* key = app.renderer.hit_key(view, x, y);
            if (key && key->id == id) {
                app.down(0, x, y, 1);
                if (release) {
                    app.up(0, x, y);
                }
                return;
            }
        }
    }
    throw std::runtime_error("A key hit region missing");
}
void typing_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    options.mode = fk::Mode::Vr;
    options.input =
        fk::InputBackend::Uinput; // Exercise the explicit physical-key backend and its keymap guard.
    options.target_language = "en-us";
    options.start_enabled = true;
    RecordingSink sink;
    fk::App app(options, sink);
    auto type_key = [&] { type_a(app); };
    type_key();
    require(sink.events == std::vector<std::pair<int, int>>{{30, 1}, {30, 0}},
            "typing launcher delivers key events");
    app.summon();
    type_key();
    require(sink.events.size() == 4, "relaunch preserves typing choice");
    app.reload();
    type_key();
    require(sink.events.size() == 6, "profile reload keeps an enabled matching backend usable");
    app.apply({"international-full", "de-de", "midnight"});
    type_key();
    require(sink.events.size() == 6, "mismatched language cannot type");
    app.apply({"en-us-full", "en-us", "midnight"});
    type_key();
    require(sink.events.size() == 8, "matching selection resumes without a pause button");
    app.set_dragging(true);
    type_key();
    require(sink.events.size() == 8, "drag capture suppresses keys");
    app.set_dragging(false);
    type_key();
    require(sink.events.size() == 10, "ending a drag restores typing");
    app.report_status("Tracking recovered");
    app.dirty = false;
    app.set_dragging(false);
    require(app.view().status == "Tracking recovered" && !app.dirty,
            "repeated drag cleanup leaves status and rendering alone");
    type_a(app, false);
    app.set_dragging(false);
    require(sink.events.size() == 11 && sink.events.back() == std::pair<int, int>{30, 1},
            "repeated drag cleanup cannot cancel a newly held key");
    app.cancel();
    sink.ready = false;
    app.tick(2);
    type_key();
    require(sink.events.size() == 12, "lost backend disables typing");
    require(app.view().status.find("connection lost") != std::string::npos, "lost backend is visible");
}
void connection_recovery_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    options.mode = fk::Mode::Vr;
    options.input =
        fk::InputBackend::Uinput; // Exercise the explicit physical-key backend and its keymap guard.
    options.target_language = "en-us";
    options.start_enabled = true;
    RecordingSink sink;
    fk::App app(options, sink);
    app.set_interaction_active(false);
    sink.ready = false;
    app.tick(1);
    app.set_interaction_active(true);
    require(app.view().status.find("connection lost") != std::string::npos,
            "disconnect while hidden reports connection loss instead of keymap mismatch");
    sink.recoverable = true;
    app.tick(2);
    require(app.view().status.find("Waiting for the compositor") != std::string::npos,
            "temporary pause has a distinct status");
    sink.ready = true;
    sink.reset = true;
    app.tick(3);
    require(app.view().status.empty(), "resume clears the pause status");
    type_a(app, false);
    require(sink.events.size() == 1, "fresh press works after resume");
    // A pause and resume delivered together must still discard the old UI hold.
    sink.reset = true;
    app.tick(4);
    require(!app.pointer_pressed(0) && sink.events.size() == 2,
            "same-batch interruption clears stale key state");
    app.tick(100);
    require(sink.events.size() == 2, "resuming cannot replay a stale repeat");
    app.set_interaction_active(false);
    sink.ready = false;
    app.tick(101);
    sink.ready = true;
    sink.reset = true;
    app.tick(102);
    type_a(app);
    require(sink.events.size() == 2, "resume while hidden cannot enable typing");
    app.set_interaction_active(true);
    type_a(app);
    require(sink.events.size() == 4, "visible typing recovers after a hidden pause");
}
void hidden_input_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    options.mode = fk::Mode::Vr;
    options.input =
        fk::InputBackend::Uinput; // Exercise the explicit physical-key backend and its keymap guard.
    options.target_language = "en-us";
    options.start_enabled = true;
    RecordingSink sink;
    fk::App app(options, sink);
    type_a(app, true, "ControlLeft");
    type_a(app, false);
    require(sink.events == std::vector<std::pair<int, int>>{{29, 1}, {30, 1}},
            "modified key held before hiding");
    app.set_interaction_active(false);
    require(sink.events.back() == std::pair{29, 0}, "hide releases modifier before disabling gate");
    type_a(app);
    app.summon();
    app.reload();
    app.apply({"en-us-full", "en-us", "graphite"});
    type_a(app);
    app.tick(20);
    require(sink.events.size() == 4, "hidden dashboard blocks clicks, repeat, reload and relaunch");
    app.set_interaction_active(true);
    type_a(app, false);
    require(sink.events.back() == std::pair{30, 1}, "reopening permits fresh press without stale Ctrl");
    app.set_interaction_active(false);
    require(sink.events.back() == std::pair{30, 0}, "hide releases ordinary held key");
    const auto count = sink.events.size();
    app.tick(50);
    require(sink.events.size() == count, "hidden held key cannot repeat");
    sink.ready = false;
    app.set_interaction_active(true);
    type_a(app);
    require(sink.events.size() == count, "dashboard reopening cannot revive disconnected input");
}
void language_recovery_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    options.mode = fk::Mode::Vr;
    options.input =
        fk::InputBackend::Uinput; // Exercise the explicit physical-key backend and its keymap guard.
    options.target_language = "en-us";
    options.start_enabled = true;
    RecordingSink sink;
    {
        fk::App first(options, sink);
        first.apply({"international-full", "de-de", "graphite"});
    }
    fk::App reopened(options, sink);
    type_a(reopened);
    require(sink.events.empty(), "saved mismatch opens without emitting input");
    reopened.show_settings();
    require(reopened.view().settings, "mismatched saved language leaves Settings reachable");
    reopened.apply({"en-us-full", "en-us", "graphite"});
    reopened.summon();
    type_a(reopened);
    require(sink.events.size() == 2, "matching selection recovers after reopening");

    const std::string original = R"PROFILE({
  "schema_version": 1,
  "id": "en-us",
  "name": "English (US)",
  "locale": "en-US",
  "keymap": {
    "rules": "evdev",
    "model": "pc105",
    "layout": "us",
    "variant": "",
    "options": []
  },
  "legends": {
    "source": "keymap",
    "overrides": {}
  },
  "font_families": [
    "Noto Sans",
    "sans-serif"
  ]
}
)PROFILE";
    auto changed = original;
    const auto offset = changed.find("\"layout\": \"us\"");
    require(offset != std::string::npos, "language fixture has a US keymap");
    changed.replace(offset, std::string("\"layout\": \"us\"").size(), "\"layout\": \"de\"");
    fk::fs::create_directories(temp.path / "languages");
    const auto path = temp.path / "languages/en-us.json";
    {
        std::ofstream file(path);
        file << changed;
    }
    reopened.reload();
    require(reopened.view().language->keymap == "de", "same-ID edit loads for preview");
    type_a(reopened);
    reopened.apply({"en-us-full", "en-us", "midnight"});
    type_a(reopened);
    require(sink.events.size() == 2, "Reload and Apply cannot redefine the launch target keymap");
    reopened.flush_settings(); // The following Reload exercises idle profile preparation.
    {
        std::ofstream file(path);
        file << original;
    }
    reopened.reload();
    type_a(reopened);
    require(sink.events.size() == 4, "restoring the declared keymap restores typing");

    options.start_enabled = false;
    fk::App preview(options, sink);
    preview.reload();
    type_a(preview);
    require(sink.events.size() == 4, "recovery never enables a launch without explicit opt-in");
}
void ui_tests() {
    TemporaryDirectory temp;
    fk::Options options;
    options.config_dir = temp.path;
    fk::NullSink sink;
    fk::App app(options, sink);
    require(!app.view().settings, "keyboard remains visible with resize controls");
    for (const auto& control : app.view().controls) {
        require(control.id != "position" && control.id != "input" && control.id != "release",
                "removed toolbar controls absent");
    }
    for (const auto* id : {"size-smaller", "size-larger"}) {
        bool clicked = false;
        for (const auto& control : app.view().controls) {
            if (control.id != id) {
                continue;
            }
            require(control.icon != fk::Icon::None, "resize controls have icons");
            const double x = control.bounds.x + control.bounds.width / 2;
            const double y = control.bounds.y + control.bounds.height / 2;
            app.down(0, x, y, 1);
            app.up(0, x, y);
            clicked = true;
        }
        require(clicked, "resize control exists on main view");
    }
    require(app.take_placement_actions() ==
                std::vector{fk::PlacementAction::Smaller, fk::PlacementAction::Larger},
            "main-view icons queue resize actions");
    app.summon();
    require(!app.view().settings && app.take_recenter(),
            "summon returns to keyboard and requests recenter");
    require(!app.take_recenter(), "recenter request consumed once");
}
} // namespace
int main() {
    try {
        placement_tests();
        dashboard_tab_tests();
        drag_tests();
        horizon_tests();
        stick_depth_tests();
        grip_tests();
        trigger_release_tests();
        persistence_tests();
        key_feedback_tests();
        haptic_routing_tests();
        instance_tests();
        ui_tests();
        typing_tests();
        hidden_input_tests();
        connection_recovery_tests();
        language_recovery_tests();
        std::cout << "Placement, repeated-launch IPC, stale-owner recovery and placement UI passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
