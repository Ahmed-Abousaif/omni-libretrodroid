#pragma once

#include "omni_text_input.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <string>

// The core may request input from its CPU thread while Android edits on the UI
// thread. No callback into a core is retained across unload or called by JNI.
class TextInput {
public:
    struct Request {
        uint64_t session;
        std::u16string text;
        uint32_t maxLength;
    };

    static TextInput& instance() { static TextInput value; return value; }

    static void show(uint64_t session, const uint16_t* text, uint32_t length, uint32_t maxLength) {
        auto& self = instance();
        std::lock_guard lock(self.mutex);
        maxLength = std::min(maxLength, uint32_t(OMNI_TEXT_INPUT_CAPACITY - 1));
        length = std::min(length, maxLength);
        std::u16string initial;
        for (uint32_t i = 0; text && i < length; ++i) initial.push_back(text[i]);
        self.request = Request{session, std::move(initial), maxLength};
        self.pending = false;
        self.confirmed = false;
        self.response.clear();
    }

    static void hide(uint64_t session) {
        auto& self = instance();
        std::lock_guard lock(self.mutex);
        if (self.request && self.request->session == session) self.resetLocked();
    }

    static bool poll(uint64_t session, uint16_t* text, uint32_t capacity, uint32_t* length, bool* confirmed) {
        auto& self = instance();
        std::lock_guard lock(self.mutex);
        if (!self.request || self.request->session != session || !self.pending ||
            !text || !length || !confirmed || capacity < self.response.size()) return false;
        std::copy(self.response.begin(), self.response.end(), text);
        *length = uint32_t(self.response.size());
        *confirmed = self.confirmed;
        self.pending = false;
        return true;
    }

    std::optional<Request> getRequest() {
        std::lock_guard lock(mutex);
        return request;
    }

    bool update(uint64_t session, std::u16string text, bool submit) {
        std::lock_guard lock(mutex);
        if (!request || request->session != session || confirmed) return false;
        if (text.size() > request->maxLength) text.resize(request->maxLength);
        // Never pass an unpaired high surrogate after truncation to the game.
        if (!text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) text.pop_back();
        response = std::move(text);
        request->text = response;
        pending = true;
        confirmed = submit;
        return true;
    }

    void reset() { std::lock_guard lock(mutex); resetLocked(); }

private:
    void resetLocked() { request.reset(); response.clear(); pending = confirmed = false; }
    std::mutex mutex;
    std::optional<Request> request;
    std::u16string response;
    bool pending = false;
    bool confirmed = false;
};
