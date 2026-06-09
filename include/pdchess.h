/*
 * pdchess - small instance-based chess engine for Playdate C projects
 *
 * Engine design adapted from mcu-max by Gissio and micro-Max by H.G. Muller.
 * See THIRD_PARTY_NOTICES.md. This adaptation is distributed under the MIT
 * license.
 */

#ifndef PDCHESS_H
#define PDCHESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDCHESS_VERSION "0.1.0"
#define PDCHESS_MAX_LEGAL_MOVES 256u
#define PDCHESS_STATE_BYTES 256u
#define PDCHESS_SQUARE_NONE 0xffu

typedef uint8_t pdchess_square;

typedef enum {
    PDCHESS_COLOR_WHITE = 0,
    PDCHESS_COLOR_BLACK = 1
} pdchess_color;

typedef enum {
    PDCHESS_PIECE_NONE = 0,
    PDCHESS_PIECE_PAWN,
    PDCHESS_PIECE_KNIGHT,
    PDCHESS_PIECE_BISHOP,
    PDCHESS_PIECE_ROOK,
    PDCHESS_PIECE_QUEEN,
    PDCHESS_PIECE_KING
} pdchess_piece_type;

typedef struct {
    pdchess_piece_type type;
    pdchess_color color;
} pdchess_piece;

typedef enum {
    PDCHESS_MOVE_NONE = 0,
    PDCHESS_MOVE_CAPTURE = 1u << 0,
    PDCHESS_MOVE_EN_PASSANT = 1u << 1,
    PDCHESS_MOVE_CASTLE = 1u << 2,
    PDCHESS_MOVE_PROMOTION = 1u << 3
} pdchess_move_flags;

typedef struct {
    pdchess_square from;
    pdchess_square to;
    pdchess_piece_type promotion;
    uint8_t flags;
} pdchess_move;

typedef enum {
    PDCHESS_GAME_PLAYING = 0,
    PDCHESS_GAME_CHECK,
    PDCHESS_GAME_CHECKMATE,
    PDCHESS_GAME_STALEMATE
} pdchess_game_status;

typedef enum {
    PDCHESS_STOP_DEPTH = 0,
    PDCHESS_STOP_NODE_LIMIT,
    PDCHESS_STOP_CANCELLED,
    PDCHESS_STOP_NO_LEGAL_MOVE
} pdchess_stop_reason;

typedef bool (*pdchess_poll_fn)(void *userdata);

typedef struct {
    uint32_t node_limit;
    uint8_t depth_limit;
    pdchess_poll_fn poll;
    void *userdata;
    uint32_t poll_interval;
} pdchess_search_limits;

typedef struct {
    pdchess_move best_move;
    int32_t score_cp;
    uint32_t nodes;
    uint8_t completed_depth;
    pdchess_stop_reason stop_reason;
    bool has_move;
} pdchess_search_result;

/*
 * Opaque, caller-owned storage. It may be stack allocated, embedded in another
 * object, or kept statically. Do not inspect storage directly.
 */
typedef union {
    uint64_t alignment;
    uint8_t storage[PDCHESS_STATE_BYTES];
} pdchess_state;

/*
 * Squares are numbered a1=0 through h8=63:
 * square = rank * 8 + file, with file/rank in the range 0..7.
 */
pdchess_square pdchess_make_square(uint8_t file, uint8_t rank);
uint8_t pdchess_square_file(pdchess_square square);
uint8_t pdchess_square_rank(pdchess_square square);
bool pdchess_square_is_valid(pdchess_square square);

void pdchess_init(pdchess_state *state);
bool pdchess_set_fen(pdchess_state *state, const char *fen);
bool pdchess_get_fen(const pdchess_state *state, char *buffer, size_t size);

pdchess_piece pdchess_get_piece(const pdchess_state *state,
                                pdchess_square square);
pdchess_color pdchess_side_to_move(const pdchess_state *state);
uint16_t pdchess_halfmove_clock(const pdchess_state *state);
uint16_t pdchess_fullmove_number(const pdchess_state *state);
bool pdchess_is_in_check(const pdchess_state *state);
pdchess_game_status pdchess_get_status(pdchess_state *state);

size_t pdchess_generate_legal_moves(pdchess_state *state,
                                    pdchess_move *moves,
                                    size_t capacity);
bool pdchess_is_legal_move(pdchess_state *state, pdchess_move move);
bool pdchess_apply_move(pdchess_state *state, pdchess_move move);

pdchess_search_result pdchess_search(pdchess_state *state,
                                     pdchess_search_limits limits);

const char *pdchess_status_name(pdchess_game_status status);
const char *pdchess_stop_reason_name(pdchess_stop_reason reason);
bool pdchess_move_to_uci(pdchess_move move, char output[6]);
bool pdchess_move_from_uci(const char *text, pdchess_move *move);

size_t pdchess_state_size(void);

#ifdef __cplusplus
}
#endif

#endif
