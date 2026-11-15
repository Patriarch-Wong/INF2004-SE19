// Host tests for lora_text: payload format, retries until a receipt, and
// showing each text once.
#include <cstring>
#include "lora_text.h"
#include "test_check.h"

using test_check::check;
using namespace lora_text;

namespace
{
void test_format()
{
    uint8_t payload[32];
    size_t size = 0;
    message m = {};
    check(build_text(7, "Hello from Board 1", payload, sizeof(payload),
              &size) &&
            size == 2 + 18,
        "text builds");
    check(parse(payload, size, &m) && m.kind == kind_text && m.number == 7 &&
            m.text_size == 18 &&
            std::memcmp(m.text, "Hello from Board 1", 18) == 0,
        "text parses");
    check(build_receipt(7, payload, sizeof(payload), &size) && size == 2,
        "receipt builds");
    check(parse(payload, size, &m) && m.kind == kind_receipt &&
            m.number == 7 && m.text == nullptr,
        "receipt parses");

    check(!build_text(0, "x", payload, sizeof(payload), &size),
        "number 0 refused");
    check(!build_text(1, "", payload, sizeof(payload), &size),
        "empty text refused");
    check(!build_text(1, "Hello", payload, 6, &size), "too long refused");

    const uint8_t empty_text[] = {kind_text, 3};
    const uint8_t long_receipt[] = {kind_receipt, 3, 'x'};
    const uint8_t zero_number[] = {kind_text, 0, 'x'};
    const uint8_t unknown[] = {9, 3, 'x'};
    const uint8_t plain[] = {'H', 'i'};  // An ESP-NOW-style plain text.
    check(!parse(empty_text, sizeof(empty_text), &m), "empty text rejected");
    check(!parse(long_receipt, sizeof(long_receipt), &m),
        "receipt with text rejected");
    check(!parse(zero_number, sizeof(zero_number), &m), "number 0 rejected");
    check(!parse(unknown, sizeof(unknown), &m), "unknown kind rejected");
    check(!parse(plain, sizeof(plain), &m), "plain text rejected");
    check(!parse(payload, 1, &m), "short payload rejected");
}

void test_delivered_first_try()
{
    sender s;
    s.begin(3, 1500, 1000, 42);
    const uint8_t n = s.start(100);
    check(n == 1, "first number is 1");
    check(s.poll(100) == event::send, "first try at once");
    check(s.poll(150) == event::none, "nothing more until the retry");
    check(s.receipt(n) == event::delivered, "receipt delivers");
    check(!s.pending(), "no longer pending");
    check(s.poll(5000) == event::none, "no retry after delivery");
    check(s.receipt(n) == event::none, "a second receipt is ignored");
}

void test_retries_then_failed()
{
    sender s;
    s.begin(3, 1500, 1000, 7);
    s.start(0);
    unsigned sends = 0;
    uint32_t at[3] = {};
    uint32_t failed_at = 0;
    for (uint32_t t = 0; t <= 10000; t += 10)
    {
        const event e = s.poll(t);
        if (e == event::send)
        {
            at[sends < 3 ? sends : 2] = t;
            ++sends;
        }
        else if (e == event::failed)
        {
            failed_at = t;
            break;
        }
    }
    check(sends == 3, "three tries");
    check(at[1] - at[0] >= 1500 && at[1] - at[0] < 2510 &&
            at[2] - at[1] >= 1500 && at[2] - at[1] < 2510,
        "tries 1.5 s plus at most 1 s apart");
    check(failed_at >= at[2] + 1500 && failed_at < at[2] + 1510,
        "failed 1.5 s after the last try");
    check(!s.pending(), "not pending after failing");
}

void test_late_and_replaced()
{
    sender s;
    s.begin(3, 1500, 0, 1);
    const uint8_t first = s.start(0);
    s.poll(0);
    const uint8_t second = s.start(1000);  // A new press replaces it.
    check(second != first, "new number");
    check(s.receipt(first) == event::none,
        "a receipt for the replaced text is ignored");
    check(s.poll(1000) == event::send, "the new text goes at once");
    check(s.receipt(second) == event::delivered, "and is delivered");

    s.start(2000);
    s.poll(2000);
    s.cancel();
    check(s.poll(10000) == event::none && !s.pending(),
        "cancel stops the tries");
}

void test_number_wraps()
{
    sender s;
    s.begin(1, 100, 0, 1);
    uint8_t n = 0;
    bool never_zero = true;
    for (unsigned i = 0; i < 600; ++i)
    {
        n = s.start(i);
        never_zero = never_zero && n != 0;
    }
    check(never_zero, "numbers skip 0 when wrapping");
}

void test_receiver()
{
    receiver r;
    check(r.is_new(1), "first text shown");
    check(!r.is_new(1), "a repeated try is not shown again");
    check(r.is_new(2), "the next text is shown");
    r.reset();
    check(r.is_new(2), "after a new connection the same number is new");
}

// Lossy exchange: texts and receipts both lost now and then, and every
// text still shows once and ends as delivered.
void test_lossy_exchange()
{
    sender s;
    s.begin(3, 1500, 1000, 99);
    receiver r;
    unsigned shown = 0;
    unsigned delivered = 0;
    unsigned drop = 0;
    for (unsigned text = 0; text < 20; ++text)
    {
        const uint32_t start = text * 10000;
        s.start(start);
        for (uint32_t t = start; t < start + 9000; t += 10)
        {
            const event e = s.poll(t);
            if (e == event::send)
            {
                // Lose the first copy of every other text, and the receipt
                // of the first copy of every third.
                const bool lose_text = text % 2 == 0 && s.tries_sent() == 1;
                const bool lose_receipt = text % 3 == 0 &&
                    s.tries_sent() == 1;
                if (lose_text)
                {
                    ++drop;
                    continue;
                }
                if (r.is_new(s.number()))
                {
                    ++shown;
                }
                if (!lose_receipt && s.receipt(s.number()) == event::delivered)
                {
                    ++delivered;
                }
            }
            check(e != event::failed, "no text fails with one loss");
        }
    }
    check(drop == 10, "losses happened");
    check(shown == 20, "each text shown once");
    check(delivered == 20, "each text delivered");
}
}

int main()
{
    test_format();
    test_delivered_first_try();
    test_retries_then_failed();
    test_late_and_replaced();
    test_number_wraps();
    test_receiver();
    test_lossy_exchange();
    return test_check::finish("LoRa text");
}
