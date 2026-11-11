#include "../src/main/cpp/textinput/textinput.h"
#include <cassert>
#include <iostream>
#include <thread>

int main() {
    auto& input = TextInput::instance();
    const uint16_t initial[] = {'L', 'i', 'n', 'k'};
    TextInput::show(1, initial, 4, 12);
    assert(input.getRequest()->text == u"Link");
    assert(input.getRequest()->maxLength == 12);
    uint16_t result[OMNI_TEXT_INPUT_CAPACITY]{};
    uint32_t length = 0;
    bool confirmed = false;
    assert(!TextInput::poll(1, result, OMNI_TEXT_INPUT_CAPACITY, &length, &confirmed));

    // IME replacement/composition sends whole strings, including deletion/Unicode.
    assert(input.update(1, u"\u0628\u0648\u0628", false));
    assert(TextInput::poll(1, result, OMNI_TEXT_INPUT_CAPACITY, &length, &confirmed));
    assert(std::u16string(result, result + length) == u"\u0628\u0648\u0628");
    assert(!confirmed);
    assert(input.update(1, u"", false));
    assert(TextInput::poll(1, result, OMNI_TEXT_INPUT_CAPACITY, &length, &confirmed));
    assert(length == 0 && !confirmed);

    // Coalescing must preserve Done and reject late editor events after submission.
    assert(input.update(1, u"Zelda", false));
    assert(input.update(1, u"Zelda", true));
    assert(!input.update(1, u"late", false));
    assert(TextInput::poll(1, result, OMNI_TEXT_INPUT_CAPACITY, &length, &confirmed));
    assert(confirmed && std::u16string(result, result + length) == u"Zelda");

    // Old dialogs cannot change or hide a newer game's request.
    TextInput::show(2, initial, 4, 2);
    assert(input.getRequest()->text == u"Li");
    assert(!input.update(1, u"stale", true));
    TextInput::hide(1);
    assert(input.getRequest()->session == 2);
    assert(input.update(2, u"A\U0001f600", false));
    assert(input.getRequest()->text == u"A"); // don't leave half an emoji at limit
    assert(!TextInput::poll(2, result, 0, &length, &confirmed));
    assert(TextInput::poll(2, result, OMNI_TEXT_INPUT_CAPACITY, &length, &confirmed));
    TextInput::hide(2);
    assert(!input.getRequest());
    assert(!input.update(2, u"closed", true));

    // UI edits and core polling can run concurrently without dangling pointers.
    TextInput::show(3, initial, 4, 12);
    std::thread ui([&] { for (int i = 0; i < 10000; ++i) input.update(3, u"Player", false); });
    for (int i = 0; i < 10000; ++i) {
        input.getRequest();
        TextInput::poll(3, result, OMNI_TEXT_INPUT_CAPACITY, &length, &confirmed);
    }
    ui.join();
    input.reset();
    assert(!input.getRequest());
    assert(!TextInput::poll(3, result, OMNI_TEXT_INPUT_CAPACITY, &length, &confirmed));
    std::cout << "PASS: initial text, IME edits, Unicode, Done, stale sessions, limits, reset, concurrency\n";
}
