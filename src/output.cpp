#include "framekeyboard/output.hpp"

#include <algorithm>
#include <exception>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <utility>

namespace framekeyboard {
namespace {
constexpr auto waiting_status =
    "Waiting for compositor text delivery. Try the key again when it completes.";
}
OutputController::~OutputController() {
    try {
        cancel();
    } catch (...) {
    }
}
void OutputController::status(std::string message) {
    if (status_ != message) {
        status_ = std::move(message);
        status_changed_ = true;
    }
}
std::optional<std::string> OutputController::take_status() {
    if (std::exchange(status_changed_, false)) {
        return status_;
    }
    return {};
}
bool OutputController::admit(bool produces_output, double now) {
    now_ = now;
    if (!produces_output || !enabled_) {
        return true;
    }
    drain();
    if (failed_ && retry_.empty()) {
        failed_ = false; // A new deliberate press can follow a discarded native action.
    }
    if (failed_) {
        status("Text not sent. Check the target, then press Enter to retry. Later input was discarded.");
        return false;
    }
    // A cancelled request may still have a Wayland sync outstanding. It has no
    // owned result to consume or retry; keep B12/B14's explicit rejection.
    if (queue_.empty() && target_.output_blocked()) {
        status(waiting_status);
        return false;
    }
    if (queue_.size() >= capacity || active_.size() >= capacity) {
        status("Input queue full. Wait for delivery, then try the key again.");
        return false;
    }
    return true;
}
void OutputController::set_enabled(bool value) {
    if (enabled_ && !value) {
        cancel();
    }
    enabled_ = value;
}
void OutputController::send(int code, int value) {
    if (value == 0) {
        if (auto held = direct_.find(code); held != direct_.end()) {
            held->second->held = false;
            release_codes(*held->second);
            direct_.erase(held);
        }
    } else if (value == 1 && enabled_ && !direct_.contains(code) && admit(true)) {
        auto entry = std::make_shared<Entry>();
        entry->codes = {code};
        entry->held = true;
        direct_[code] = entry;
        queue_.push_back(entry);
        drain();
    }
}
void OutputController::native_press(unsigned pointer, const std::vector<int>& codes, int repeat_code,
                                    double now) {
    now_ = now;
    if (!enabled_ || codes.empty()) {
        return;
    }
    auto entry = std::make_shared<Entry>();
    entry->codes = codes;
    entry->repeat_code = repeat_code;
    entry->held = true;
    active_[pointer] = entry;
    queue_.push_back(entry);
    drain();
}
bool OutputController::enqueue_text(const std::string& text, std::shared_ptr<Entry> entry) {
    if (text.empty()) {
        return true;
    }
    if (!enabled_ || failed_ || queue_.size() >= capacity || text.find('\0') != std::string::npos ||
        !g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr) ||
        g_utf8_strlen(text.c_str(), -1) > 32) {
        status("Text not queued. Check the target and input queue.");
        return false;
    }
    if (!entry) {
        entry = std::make_shared<Entry>();
    }
    entry->text = text;
    queue_.push_back(entry);
    drain();
    return true; // Ownership transferred, including to the failure retry buffer.
}
bool OutputController::text_press(unsigned pointer, const std::string& text, double now) {
    now_ = now;
    auto entry = std::make_shared<Entry>();
    entry->held = true;
    if (!text.empty() && enabled_) {
        active_[pointer] = entry;
    }
    return enqueue_text(text, entry);
}
bool OutputController::commit_text(const std::string& text) {
    if (failed_) {
        return false;
    }
    if (queue_.empty() && target_.output_blocked()) {
        status(waiting_status);
        return false;
    }
    const bool idle = queue_.empty();
    const bool accepted = enqueue_text(text);
    if (accepted && idle && failed_ && retry_.size() == 1 && retry_.front()->text == text) {
        // A synchronous preedit rejection has not transferred ownership. The
        // editor retains it and can attempt another explicit commit, as before.
        retry_.clear();
        failed_ = false;
        status_.clear();
        status_changed_ = false;
        return false;
    }
    return accepted;
}
std::string OutputController::retry_text() const {
    std::string text;
    for (const auto& entry : retry_) {
        text += entry->text;
    }
    return text;
}
bool OutputController::retry() {
    if (!enabled_ || retry_.empty()) {
        return false;
    }
    if (target_.output_blocked()) {
        status(waiting_status);
        return false;
    }
    failed_ = false;
    queue_ = std::move(retry_);
    retry_.clear();
    drain();
    return !failed_;
}
void OutputController::release_codes(Entry& entry, std::optional<int> only) {
    for (auto it = entry.acquired.rbegin(); it != entry.acquired.rend(); ++it) {
        const int code = *it;
        if (only && code != *only) {
            continue;
        }
        if (auto held = delivered_.find(code); held != delivered_.end()) {
            if (--held->second == 0) {
                delivered_.erase(held);
                target_.deliver_key(code, 0);
            }
        }
    }
    if (only) {
        std::erase(entry.acquired, *only);
    } else {
        entry.acquired.clear();
    }
}
void OutputController::release(unsigned pointer) {
    if (auto found = active_.find(pointer); found != active_.end()) {
        auto& entry = *found->second;
        entry.held = false;
        // A release never needs queue capacity or waits behind unrelated text.
        release_codes(entry);
        active_.erase(found);
    }
}
void OutputController::release_modifier(int code) {
    for (auto& [pointer, entry] : active_) {
        (void)pointer;
        entry->released_modifiers.insert(code);
        release_codes(*entry, code);
    }
}
void OutputController::drop_holds() {
    for (auto& [pointer, entry] : active_) {
        (void)pointer;
        entry->held = false;
    }
    active_.clear();
    for (auto& [code, entry] : direct_) {
        (void)code;
        entry->held = false;
    }
    direct_.clear();
    // Non-modifiers before modifiers. Attempt every release even if one fails.
    std::exception_ptr error;
    for (bool modifiers : {false, true}) {
        for (const auto& [code, count] : delivered_) {
            (void)count;
            if (is_modifier(code) != modifiers) {
                continue;
            }
            try {
                target_.deliver_key(code, 0);
            } catch (...) {
                error = std::current_exception();
            }
        }
    }
    delivered_.clear();
    if (error) {
        std::rethrow_exception(error);
    }
}
void OutputController::cancel() {
    queue_.clear();
    retry_.clear();
    failed_ = false;
    target_.cancel_delivery();
    drop_holds();
    status("");
}
void OutputController::fail() {
    retry_.clear();
    bool boundary = false, discarded = false;
    for (auto& entry : queue_) {
        if (entry->text.empty()) {
            boundary = true;
        }
        if (!boundary && (!entry->repeat || entry == queue_.front())) {
            auto saved = std::make_shared<Entry>();
            saved->text = entry->text;
            retry_.push_back(saved);
        } else {
            discarded = true;
        }
    }
    queue_.clear();
    failed_ = true;
    drop_holds();
    status(retry_.empty() ? "Input not sent. Queued input was discarded."
           : discarded    ? "Text not sent. Check the target, then press Enter to retry. "
                            "Queued actions and later text were discarded."
                          : "Text not sent. Check the target, then press Enter to retry.");
}
void OutputController::drain() {
    if (!enabled_ || failed_) {
        return;
    }
    while (!queue_.empty()) {
        auto entry = queue_.front();
        if (!entry->started && entry->repeat) {
            auto owner = entry->owner.lock();
            if (!owner || !owner->held) {
                queue_.pop_front();
                continue;
            }
        }
        if (!entry->started && target_.output_blocked()) {
            return;
        }
        if (!entry->text.empty()) {
            // Never attempt a Unicode commit while a physical chord is held.
            if (!delivered_.empty()) {
                return;
            }
            entry->started = true;
            if (!target_.commit_text(entry->text)) {
                if (target_.text_pending()) {
                    return;
                }
                fail();
                return;
            }
        } else {
            while (entry->next < entry->codes.size()) {
                const int code = entry->codes[entry->next];
                if (!entry->waiting) {
                    entry->started = true;
                    entry->acquired.push_back(code);
                    if (delivered_[code]++ != 0) {
                        ++entry->next;
                        continue;
                    }
                }
                const auto result = target_.deliver_key(code, entry->repeat ? 2 : 1);
                if (result == Delivery::Waiting) {
                    entry->waiting = true;
                    return;
                }
                entry->waiting = false;
                if (result == Delivery::Failure) {
                    fail();
                    return;
                }
                ++entry->next;
            }
        }
        queue_.pop_front();
        if (entry->held) {
            entry->repeat_at = now_ + .5;
            for (int code : entry->released_modifiers) {
                release_codes(*entry, code);
            }
        } else {
            release_codes(*entry);
        }
        if (auto owner = entry->owner.lock()) {
            owner->repeat_at = now_ + .04;
        }
    }
    if (status_ != waiting_status || !target_.output_blocked()) {
        status("");
    }
}
bool OutputController::tick(double now) {
    now_ = now;
    drain();
    if (queue_.empty() && !failed_ && enabled_) {
        if (status_ == waiting_status && !target_.output_blocked()) {
            status("");
        }
        std::set<int> repeated;
        for (const auto& [pointer, held] : active_) {
            (void)pointer;
            if (!held->started || !held->held || now < held->repeat_at ||
                (held->text.empty() && !held->repeat_code)) {
                continue;
            }
            if (held->text.empty() && !repeated.insert(held->repeat_code).second) {
                continue;
            }
            auto entry = std::make_shared<Entry>();
            entry->text = held->text;
            entry->repeat = true;
            entry->owner = held;
            if (entry->text.empty()) {
                // Repeat borrows the original hold, so it never owns another up.
                const auto result = target_.deliver_key(held->repeat_code, 2);
                if (result == Delivery::Failure) {
                    fail();
                    break;
                }
                if (result == Delivery::Waiting) {
                    entry->codes = {held->repeat_code};
                    entry->started = entry->waiting = true;
                    queue_.push_back(entry);
                }
            } else {
                queue_.push_back(entry);
            }
            held->repeat_at = now + .04;
            if (!queue_.empty()) {
                drain();
                break;
            }
        }
    }
    return status_changed_ || !queue_.empty();
}
} // namespace framekeyboard
