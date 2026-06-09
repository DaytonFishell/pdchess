#include "pdchess.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                    __FILE__, __LINE__, #condition);                         \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static pdchess_move move_from(const char *uci)
{
    pdchess_move move;
    CHECK(pdchess_move_from_uci(uci, &move));
    return move;
}

static bool has_move(pdchess_state *state, const char *uci)
{
    pdchess_move wanted = move_from(uci);
    pdchess_move moves[PDCHESS_MAX_LEGAL_MOVES];
    size_t count = pdchess_generate_legal_moves(
        state, moves, PDCHESS_MAX_LEGAL_MOVES);
    size_t i;
    for (i = 0; i < count; ++i)
        if (moves[i].from == wanted.from && moves[i].to == wanted.to)
            return true;
    return false;
}

static void test_initial_and_instances(void)
{
    pdchess_state first;
    pdchess_state second;
    char fen[128];

    pdchess_init(&first);
    pdchess_init(&second);
    CHECK(pdchess_generate_legal_moves(&first, NULL, 0) == 20);
    CHECK(pdchess_apply_move(&first, move_from("e2e4")));
    CHECK(pdchess_side_to_move(&first) == PDCHESS_COLOR_BLACK);
    CHECK(pdchess_apply_move(&first, move_from("e7e5")));
    CHECK(pdchess_fullmove_number(&first) == 2);
    CHECK(pdchess_side_to_move(&second) == PDCHESS_COLOR_WHITE);
    CHECK(pdchess_get_fen(&second, fen, sizeof(fen)));
    CHECK(strcmp(fen,
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") == 0);
}

static void test_fen_round_trip(void)
{
    static const char fen[] =
        "r3k2r/ppp2ppp/2n5/3pp3/8/2N2N2/PPP2PPP/R3K2R b KQkq e3 17 42";
    pdchess_state state;
    char output[128];

    CHECK(pdchess_set_fen(&state, fen));
    CHECK(pdchess_get_fen(&state, output, sizeof(output)));
    CHECK(strcmp(fen, output) == 0);
    CHECK(pdchess_halfmove_clock(&state) == 17);
    CHECK(pdchess_fullmove_number(&state) == 42);
    CHECK(!pdchess_set_fen(&state, "not a fen"));
}

static void test_special_moves(void)
{
    pdchess_state state;
    pdchess_piece piece;

    CHECK(pdchess_set_fen(
        &state, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"));
    CHECK(has_move(&state, "e1g1"));
    CHECK(has_move(&state, "e1c1"));
    CHECK(pdchess_apply_move(&state, move_from("e1g1")));
    piece = pdchess_get_piece(&state, pdchess_make_square(5, 0));
    CHECK(piece.type == PDCHESS_PIECE_ROOK);

    CHECK(pdchess_set_fen(
        &state, "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1"));
    CHECK(has_move(&state, "e5d6"));
    CHECK(pdchess_apply_move(&state, move_from("e5d6")));
    CHECK(pdchess_get_piece(
        &state, pdchess_make_square(3, 4)).type == PDCHESS_PIECE_NONE);

    CHECK(pdchess_set_fen(
        &state, "7k/P7/8/8/8/8/8/7K w - - 0 1"));
    CHECK(has_move(&state, "a7a8q"));
    CHECK(pdchess_apply_move(&state, move_from("a7a8q")));
    CHECK(pdchess_get_piece(
        &state, pdchess_make_square(0, 7)).type == PDCHESS_PIECE_QUEEN);
}

static void test_legality_and_status(void)
{
    pdchess_state state;

    pdchess_init(&state);
    CHECK(!pdchess_apply_move(&state, move_from("e2e5")));
    CHECK(pdchess_get_status(&state) == PDCHESS_GAME_PLAYING);

    CHECK(pdchess_set_fen(
        &state, "7k/6Q1/6K1/8/8/8/8/8 b - - 0 1"));
    CHECK(pdchess_is_in_check(&state));
    CHECK(pdchess_get_status(&state) == PDCHESS_GAME_CHECKMATE);

    CHECK(pdchess_set_fen(
        &state, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"));
    CHECK(!pdchess_is_in_check(&state));
    CHECK(pdchess_get_status(&state) == PDCHESS_GAME_STALEMATE);
}

typedef struct {
    unsigned calls;
    unsigned cancel_after;
} poll_state;

static bool poll_search(void *userdata)
{
    poll_state *state = userdata;
    ++state->calls;
    return state->calls < state->cancel_after;
}

static void test_search(void)
{
    pdchess_state first;
    pdchess_state second;
    pdchess_search_limits limits = {50000, 3, NULL, NULL, 0};
    pdchess_search_result a;
    pdchess_search_result b;
    poll_state poll = {0, 2};
    char before[128];
    char after[128];

    pdchess_init(&first);
    pdchess_init(&second);
    CHECK(pdchess_get_fen(&first, before, sizeof(before)));
    a = pdchess_search(&first, limits);
    CHECK(pdchess_get_fen(&first, after, sizeof(after)));
    CHECK(strcmp(before, after) == 0);
    b = pdchess_search(&second, limits);
    CHECK(a.has_move && b.has_move);
    CHECK(a.best_move.from == b.best_move.from);
    CHECK(a.best_move.to == b.best_move.to);
    CHECK(a.completed_depth == 3);
    CHECK(a.stop_reason == PDCHESS_STOP_DEPTH);
    CHECK(pdchess_is_legal_move(&first, a.best_move));

    limits.node_limit = 20;
    limits.depth_limit = 6;
    a = pdchess_search(&first, limits);
    CHECK(a.has_move);
    CHECK(a.stop_reason == PDCHESS_STOP_NODE_LIMIT);
    CHECK(a.nodes >= 20);

    limits.node_limit = 0;
    limits.poll = poll_search;
    limits.userdata = &poll;
    limits.poll_interval = 1;
    a = pdchess_search(&first, limits);
    CHECK(a.has_move);
    CHECK(a.stop_reason == PDCHESS_STOP_CANCELLED);
    CHECK(pdchess_is_legal_move(&first, a.best_move));
}

int main(void)
{
    test_initial_and_instances();
    test_fen_round_trip();
    test_special_moves();
    test_legality_and_status();
    test_search();

    printf("pdchess state payload: %zu bytes; public storage: %u bytes\n",
           pdchess_state_size(), (unsigned)PDCHESS_STATE_BYTES);
    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all pdchess tests passed");
    return EXIT_SUCCESS;
}
