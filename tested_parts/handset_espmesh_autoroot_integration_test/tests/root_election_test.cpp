// Host tests for root_election: the ESP-MESH root ranking. Every board
// must agree on one best board whenever they all hear each other's scores,
// and an idle board in the middle must beat the talking boards.
#include "root_election.h"
#include "test_check.h"

using test_check::check;
using root_election::score;

namespace
{
constexpr uint8_t boards = 5;

score make(uint8_t neighbours, bool busy)
{
    score value;
    value.known = true;
    value.neighbours = neighbours;
    value.busy = busy;
    return value;
}

// Each board's rank when every board hears every other's score.
void ranks(const score *scores, const bool *present, uint32_t *out)
{
    for (uint8_t id = 1; id <= boards; ++id)
    {
        out[id] = present[id] ?
            root_election::rank(id, scores[id], scores, present, boards) :
            99;
    }
}

// Exactly one present board has rank 0, and it is `expected`.
void check_single_winner(const score *scores, const bool *present,
    uint8_t expected, const char *description)
{
    uint32_t place[boards + 1] = {};
    ranks(scores, present, place);
    unsigned zeros = 0;
    for (uint8_t id = 1; id <= boards; ++id)
    {
        if (present[id] && place[id] == 0)
        {
            ++zeros;
        }
    }
    check(zeros == 1, description);
    check(present[expected] && place[expected] == 0, description);
}

void test_lowest_id_on_a_tie()
{
    score scores[boards + 1] = {};
    bool present[boards + 1] = {false, true, true, true, false, false};
    for (uint8_t id = 1; id <= 3; ++id)
    {
        scores[id] = make(2, false);
    }
    check_single_winner(scores, present, 1, "tie: lowest ID wins");
    uint32_t place[boards + 1] = {};
    ranks(scores, present, place);
    check(place[2] == 1 && place[3] == 2, "tie: then ID order");
}

void test_middle_board_wins()
{
    // 1 - 3 - 2 in a line: 3 hears both ends, the ends hear only 3.
    score scores[boards + 1] = {};
    bool present[boards + 1] = {false, true, true, true, false, false};
    scores[1] = make(1, false);
    scores[2] = make(1, false);
    scores[3] = make(2, false);
    check_single_winner(scores, present, 3, "middle board wins");
}

void test_idle_beats_busy()
{
    // 1 and 2 are talking; 3 hears fewer boards but is unused.
    score scores[boards + 1] = {};
    bool present[boards + 1] = {false, true, true, true, true, false};
    scores[1] = make(3, true);
    scores[2] = make(3, true);
    scores[3] = make(1, false);
    scores[4] = make(2, false);
    check_single_winner(scores, present, 4,
        "idle board with most neighbours wins over busy ones");
    uint32_t place[boards + 1] = {};
    ranks(scores, present, place);
    check(place[3] == 1, "the other idle board comes next");
    check(place[1] == 2 && place[2] == 3, "busy boards last, by ID");
}

void test_all_busy_falls_back_to_neighbours()
{
    score scores[boards + 1] = {};
    bool present[boards + 1] = {false, true, true, false, false, false};
    scores[1] = make(1, true);
    scores[2] = make(2, true);
    check_single_winner(scores, present, 2,
        "all busy: most neighbours wins");
}

void test_absent_boards_ignored()
{
    // Board 1 would win but has gone (no longer a candidate).
    score scores[boards + 1] = {};
    bool present[boards + 1] = {false, false, true, true, false, false};
    scores[1] = make(4, false);
    scores[2] = make(1, false);
    scores[3] = make(1, false);
    check_single_winner(scores, present, 2, "a board gone isn't counted");
}

void test_unknown_score_uses_id()
{
    score mine = make(4, false);
    score unknown;
    check(root_election::better(1, unknown, 2, mine),
        "unknown score: lower ID ranks ahead");
    check(!root_election::better(3, unknown, 2, mine),
        "unknown score: higher ID ranks behind");
}

void test_out_of_mesh_skipped()
{
    // Board 1 would win but has left the mesh: board 2 must not wait on it.
    score scores[boards + 1] = {};
    bool present[boards + 1] = {false, true, true, false, false, false};
    scores[1] = make(1, false);
    scores[1].out = true;
    scores[2] = make(1, false);
    check(root_election::rank(2, scores[2], scores, present, boards) == 0,
        "a board out of the mesh isn't waited on");
}

void test_ping_flags_round_trip()
{
    score value = make(3, true);
    value.out = true;
    const score back = root_election::from_ping(value.neighbours,
        root_election::flags(value));
    check(back.known && back.neighbours == 3 && back.busy && back.out,
        "ping flags round trip");
    const score idle = root_election::from_ping(2, 0);
    check(!idle.busy && !idle.out, "no flags: idle and in the mesh");
}

void test_never_counts_self()
{
    score scores[boards + 1] = {};
    bool present[boards + 1] = {false, true, false, false, false, false};
    scores[1] = make(0, true);
    check(root_election::rank(1, scores[1], scores, present, boards) == 0,
        "alone: rank 0");
}
}

int main()
{
    test_lowest_id_on_a_tie();
    test_middle_board_wins();
    test_idle_beats_busy();
    test_all_busy_falls_back_to_neighbours();
    test_absent_boards_ignored();
    test_unknown_score_uses_id();
    test_out_of_mesh_skipped();
    test_ping_flags_round_trip();
    test_never_counts_self();
    return test_check::finish("root election");
}
