/*
 * pdchess
 *
 * A compact, allocation-free chess engine for Playdate C projects. The small
 * board representation and bounded alpha-beta search are adapted from ideas in
 * mcu-max/micro-Max; the instance API, rules layer, FEN support, and diagnostics
 * are original to this port.
 */

#include "pdchess.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CASTLE_WHITE_K 1u
#define CASTLE_WHITE_Q 2u
#define CASTLE_BLACK_K 4u
#define CASTLE_BLACK_Q 8u
#define SCORE_MATE 30000
#define SCORE_INF 32000

typedef struct
{
    int8_t board[64];
    uint8_t side;
    uint8_t castling;
    uint8_t en_passant;
    uint16_t halfmove;
    uint16_t fullmove;
} engine_state;

typedef struct
{
    uint32_t nodes;
    uint32_t node_limit;
    uint32_t poll_interval;
    uint32_t next_poll;
    uint8_t root_depth;
    bool cancelled;
    bool node_limited;
    pdchess_poll_fn poll;
    void *userdata;
} search_context;

typedef char state_storage_is_large_enough[sizeof(engine_state) <= PDCHESS_STATE_BYTES ? 1 : -1];

static const int16_t piece_values[] = {0, 100, 320, 330, 500, 900, 0};
static const int8_t knight_steps[][2] = {
    {1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
static const int8_t king_steps[][2] = {
    {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
static const int8_t bishop_steps[][2] = {
    {1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
static const int8_t rook_steps[][2] = {
    {1, 0}, {0, 1}, {-1, 0}, {0, -1}};

static engine_state *impl(pdchess_state *state)
{
    return (engine_state *)state->storage;
}

static const engine_state *cimpl(const pdchess_state *state)
{
    return (const engine_state *)state->storage;
}

static int color_sign(pdchess_color color)
{
    return color == PDCHESS_COLOR_WHITE ? 1 : -1;
}

static pdchess_color opposite(pdchess_color color)
{
    return color == PDCHESS_COLOR_WHITE
               ? PDCHESS_COLOR_BLACK
               : PDCHESS_COLOR_WHITE;
}

static int piece_type(int8_t piece)
{
    return piece < 0 ? -piece : piece;
}

static pdchess_color piece_color(int8_t piece)
{
    return piece > 0 ? PDCHESS_COLOR_WHITE : PDCHESS_COLOR_BLACK;
}

pdchess_square pdchess_make_square(uint8_t file, uint8_t rank)
{
    return file < 8 && rank < 8
               ? (pdchess_square)(rank * 8 + file)
               : PDCHESS_SQUARE_NONE;
}

uint8_t pdchess_square_file(pdchess_square square)
{
    return (uint8_t)(square & 7u);
}

uint8_t pdchess_square_rank(pdchess_square square)
{
    return (uint8_t)(square >> 3);
}

bool pdchess_square_is_valid(pdchess_square square)
{
    return square < 64u;
}

static bool on_board(int file, int rank)
{
    return file >= 0 && file < 8 && rank >= 0 && rank < 8;
}

static int8_t fen_piece(char c)
{
    int sign = c >= 'A' && c <= 'Z' ? 1 : -1;
    char lower = c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
    int type = 0;

    switch (lower)
    {
    case 'p':
        type = PDCHESS_PIECE_PAWN;
        break;
    case 'n':
        type = PDCHESS_PIECE_KNIGHT;
        break;
    case 'b':
        type = PDCHESS_PIECE_BISHOP;
        break;
    case 'r':
        type = PDCHESS_PIECE_ROOK;
        break;
    case 'q':
        type = PDCHESS_PIECE_QUEEN;
        break;
    case 'k':
        type = PDCHESS_PIECE_KING;
        break;
    default:
        return 0;
    }

    return (int8_t)(sign * type);
}

static char piece_fen(int8_t piece)
{
    static const char letters[] = " pnbrqk";
    char c = letters[piece_type(piece)];
    return piece > 0 ? (char)(c - ('a' - 'A')) : c;
}

void pdchess_init(pdchess_state *state)
{
    static const int8_t back_rank[] = {
        PDCHESS_PIECE_ROOK, PDCHESS_PIECE_KNIGHT,
        PDCHESS_PIECE_BISHOP, PDCHESS_PIECE_QUEEN,
        PDCHESS_PIECE_KING, PDCHESS_PIECE_BISHOP,
        PDCHESS_PIECE_KNIGHT, PDCHESS_PIECE_ROOK};
    engine_state *s;
    int file;

    if (!state)
        return;

    memset(state, 0, sizeof(*state));
    s = impl(state);
    for (file = 0; file < 8; ++file)
    {
        s->board[file] = back_rank[file];
        s->board[8 + file] = PDCHESS_PIECE_PAWN;
        s->board[48 + file] = -PDCHESS_PIECE_PAWN;
        s->board[56 + file] = (int8_t)-back_rank[file];
    }
    s->side = PDCHESS_COLOR_WHITE;
    s->castling = CASTLE_WHITE_K | CASTLE_WHITE_Q |
                  CASTLE_BLACK_K | CASTLE_BLACK_Q;
    s->en_passant = PDCHESS_SQUARE_NONE;
    s->fullmove = 1;
}

static bool parse_unsigned(const char **cursor, uint16_t *value)
{
    uint32_t parsed = 0;
    const char *p = *cursor;

    if (*p < '0' || *p > '9')
        return false;
    while (*p >= '0' && *p <= '9')
    {
        parsed = parsed * 10u + (uint32_t)(*p - '0');
        if (parsed > UINT16_MAX)
            return false;
        ++p;
    }
    *cursor = p;
    *value = (uint16_t)parsed;
    return true;
}

bool pdchess_set_fen(pdchess_state *state, const char *fen)
{
    pdchess_state parsed_state;
    engine_state *s;
    const char *p = fen;
    int rank = 7;
    int file = 0;
    int kings[2] = {0, 0};

    if (!state || !fen)
        return false;

    memset(&parsed_state, 0, sizeof(parsed_state));
    s = impl(&parsed_state);
    s->en_passant = PDCHESS_SQUARE_NONE;

    while (*p && *p != ' ')
    {
        if (*p == '/')
        {
            if (file != 8 || rank == 0)
                return false;
            --rank;
            file = 0;
        }
        else if (*p >= '1' && *p <= '8')
        {
            file += *p - '0';
            if (file > 8)
                return false;
        }
        else
        {
            int8_t piece = fen_piece(*p);
            if (!piece || file >= 8)
                return false;
            s->board[rank * 8 + file] = piece;
            if (piece_type(piece) == PDCHESS_PIECE_KING)
                ++kings[piece_color(piece)];
            ++file;
        }
        ++p;
    }
    if (rank != 0 || file != 8 || kings[0] != 1 || kings[1] != 1 ||
        *p++ != ' ')
        return false;

    if (*p == 'w')
        s->side = PDCHESS_COLOR_WHITE;
    else if (*p == 'b')
        s->side = PDCHESS_COLOR_BLACK;
    else
        return false;
    ++p;
    if (*p++ != ' ')
        return false;

    if (*p == '-')
    {
        ++p;
    }
    else
    {
        while (*p && *p != ' ')
        {
            uint8_t flag;
            switch (*p++)
            {
            case 'K':
                flag = CASTLE_WHITE_K;
                break;
            case 'Q':
                flag = CASTLE_WHITE_Q;
                break;
            case 'k':
                flag = CASTLE_BLACK_K;
                break;
            case 'q':
                flag = CASTLE_BLACK_Q;
                break;
            default:
                return false;
            }
            if (s->castling & flag)
                return false;
            s->castling |= flag;
        }
    }
    if (*p++ != ' ')
        return false;

    if (*p == '-')
    {
        ++p;
    }
    else
    {
        int ep_file;
        int ep_rank;
        if (p[0] < 'a' || p[0] > 'h' ||
            (p[1] != '3' && p[1] != '6'))
            return false;
        ep_file = p[0] - 'a';
        ep_rank = p[1] - '1';
        s->en_passant = pdchess_make_square((uint8_t)ep_file,
                                            (uint8_t)ep_rank);
        p += 2;
    }
    if (*p++ != ' ' || !parse_unsigned(&p, &s->halfmove) || *p++ != ' ' ||
        !parse_unsigned(&p, &s->fullmove) || s->fullmove == 0)
        return false;
    while (*p == ' ')
        ++p;
    if (*p != '\0')
        return false;

    *state = parsed_state;
    return true;
}

static bool append_char(char **out, size_t *remaining, char c)
{
    if (*remaining <= 1)
        return false;
    **out = c;
    ++*out;
    --*remaining;
    **out = '\0';
    return true;
}

static bool append_text(char **out, size_t *remaining, const char *text)
{
    size_t length = strlen(text);
    if (length >= *remaining)
        return false;
    memcpy(*out, text, length + 1);
    *out += length;
    *remaining -= length;
    return true;
}

bool pdchess_get_fen(const pdchess_state *state, char *buffer, size_t size)
{
    const engine_state *s;
    char *out = buffer;
    size_t remaining = size;
    char number[16];
    int rank;

    if (!state || !buffer || size == 0)
        return false;
    buffer[0] = '\0';
    s = cimpl(state);

    for (rank = 7; rank >= 0; --rank)
    {
        int empty = 0;
        int file;
        for (file = 0; file < 8; ++file)
        {
            int8_t piece = s->board[rank * 8 + file];
            if (!piece)
            {
                ++empty;
                continue;
            }
            if (empty && !append_char(&out, &remaining, (char)('0' + empty)))
                return false;
            empty = 0;
            if (!append_char(&out, &remaining, piece_fen(piece)))
                return false;
        }
        if (empty && !append_char(&out, &remaining, (char)('0' + empty)))
            return false;
        if (rank && !append_char(&out, &remaining, '/'))
            return false;
    }

    if (!append_text(&out, &remaining,
                     s->side == PDCHESS_COLOR_WHITE ? " w " : " b "))
        return false;
    if (!s->castling)
    {
        if (!append_char(&out, &remaining, '-'))
            return false;
    }
    else
    {
        if ((s->castling & CASTLE_WHITE_K) &&
            !append_char(&out, &remaining, 'K'))
            return false;
        if ((s->castling & CASTLE_WHITE_Q) &&
            !append_char(&out, &remaining, 'Q'))
            return false;
        if ((s->castling & CASTLE_BLACK_K) &&
            !append_char(&out, &remaining, 'k'))
            return false;
        if ((s->castling & CASTLE_BLACK_Q) &&
            !append_char(&out, &remaining, 'q'))
            return false;
    }
    if (!append_char(&out, &remaining, ' '))
        return false;
    if (s->en_passant == PDCHESS_SQUARE_NONE)
    {
        if (!append_char(&out, &remaining, '-'))
            return false;
    }
    else
    {
        if (!append_char(&out, &remaining,
                         (char)('a' + pdchess_square_file(s->en_passant))) ||
            !append_char(&out, &remaining,
                         (char)('1' + pdchess_square_rank(s->en_passant))))
            return false;
    }

    snprintf(number, sizeof(number), " %u %u",
             (unsigned)s->halfmove, (unsigned)s->fullmove);
    return append_text(&out, &remaining, number);
}

pdchess_piece pdchess_get_piece(const pdchess_state *state,
                                pdchess_square square)
{
    pdchess_piece result = {PDCHESS_PIECE_NONE, PDCHESS_COLOR_WHITE};
    int8_t piece;

    if (!state || !pdchess_square_is_valid(square))
        return result;
    piece = cimpl(state)->board[square];
    if (piece)
    {
        result.type = (pdchess_piece_type)piece_type(piece);
        result.color = piece_color(piece);
    }
    return result;
}

pdchess_color pdchess_side_to_move(const pdchess_state *state)
{
    return state ? (pdchess_color)cimpl(state)->side : PDCHESS_COLOR_WHITE;
}

uint16_t pdchess_halfmove_clock(const pdchess_state *state)
{
    return state ? cimpl(state)->halfmove : 0;
}

uint16_t pdchess_fullmove_number(const pdchess_state *state)
{
    return state ? cimpl(state)->fullmove : 0;
}

static bool square_attacked(const engine_state *s,
                            pdchess_square square,
                            pdchess_color attacker)
{
    int file = pdchess_square_file(square);
    int rank = pdchess_square_rank(square);
    int sign = color_sign(attacker);
    int pawn_rank = rank + (attacker == PDCHESS_COLOR_WHITE ? -1 : 1);
    size_t i;

    for (i = 0; i < 2; ++i)
    {
        int pawn_file = file + (i == 0 ? -1 : 1);
        if (on_board(pawn_file, pawn_rank) &&
            s->board[pawn_rank * 8 + pawn_file] ==
                sign * PDCHESS_PIECE_PAWN)
            return true;
    }

    for (i = 0; i < sizeof(knight_steps) / sizeof(knight_steps[0]); ++i)
    {
        int f = file + knight_steps[i][0];
        int r = rank + knight_steps[i][1];
        if (on_board(f, r) &&
            s->board[r * 8 + f] == sign * PDCHESS_PIECE_KNIGHT)
            return true;
    }

    for (i = 0; i < sizeof(king_steps) / sizeof(king_steps[0]); ++i)
    {
        int f = file + king_steps[i][0];
        int r = rank + king_steps[i][1];
        if (on_board(f, r) &&
            s->board[r * 8 + f] == sign * PDCHESS_PIECE_KING)
            return true;
    }

    for (i = 0; i < sizeof(bishop_steps) / sizeof(bishop_steps[0]); ++i)
    {
        int f = file + bishop_steps[i][0];
        int r = rank + bishop_steps[i][1];
        while (on_board(f, r))
        {
            int8_t piece = s->board[r * 8 + f];
            if (piece)
            {
                if (piece == sign * PDCHESS_PIECE_BISHOP ||
                    piece == sign * PDCHESS_PIECE_QUEEN)
                    return true;
                break;
            }
            f += bishop_steps[i][0];
            r += bishop_steps[i][1];
        }
    }

    for (i = 0; i < sizeof(rook_steps) / sizeof(rook_steps[0]); ++i)
    {
        int f = file + rook_steps[i][0];
        int r = rank + rook_steps[i][1];
        while (on_board(f, r))
        {
            int8_t piece = s->board[r * 8 + f];
            if (piece)
            {
                if (piece == sign * PDCHESS_PIECE_ROOK ||
                    piece == sign * PDCHESS_PIECE_QUEEN)
                    return true;
                break;
            }
            f += rook_steps[i][0];
            r += rook_steps[i][1];
        }
    }
    return false;
}

static pdchess_square find_king(const engine_state *s, pdchess_color color)
{
    int target = color_sign(color) * PDCHESS_PIECE_KING;
    int square;
    for (square = 0; square < 64; ++square)
        if (s->board[square] == target)
            return (pdchess_square)square;
    return PDCHESS_SQUARE_NONE;
}

static bool color_in_check(const engine_state *s, pdchess_color color)
{
    pdchess_square king = find_king(s, color);
    return king == PDCHESS_SQUARE_NONE ||
           square_attacked(s, king, opposite(color));
}

bool pdchess_is_in_check(const pdchess_state *state)
{
    return state &&
           color_in_check(cimpl(state), (pdchess_color)cimpl(state)->side);
}

static void add_move(pdchess_move *moves, size_t capacity, size_t *count,
                     int from, int to, uint8_t flags)
{
    if (*count < capacity && moves)
    {
        moves[*count].from = (pdchess_square)from;
        moves[*count].to = (pdchess_square)to;
        moves[*count].flags = flags;
        moves[*count].promotion =
            (flags & PDCHESS_MOVE_PROMOTION)
                ? PDCHESS_PIECE_QUEEN
                : PDCHESS_PIECE_NONE;
    }
    ++*count;
}

static size_t generate_pseudo(const engine_state *s,
                              pdchess_move *moves,
                              size_t capacity)
{
    size_t count = 0;
    int from;

    for (from = 0; from < 64; ++from)
    {
        int8_t piece = s->board[from];
        int type;
        int file;
        int rank;
        if (!piece || piece_color(piece) != (pdchess_color)s->side)
            continue;
        type = piece_type(piece);
        file = from & 7;
        rank = from >> 3;

        if (type == PDCHESS_PIECE_PAWN)
        {
            int direction = s->side == PDCHESS_COLOR_WHITE ? 1 : -1;
            int start_rank = s->side == PDCHESS_COLOR_WHITE ? 1 : 6;
            int promotion_rank = s->side == PDCHESS_COLOR_WHITE ? 7 : 0;
            int next_rank = rank + direction;
            int to = next_rank * 8 + file;
            int delta;
            if (on_board(file, next_rank) && !s->board[to])
            {
                add_move(moves, capacity, &count, from, to,
                         next_rank == promotion_rank
                             ? PDCHESS_MOVE_PROMOTION
                             : PDCHESS_MOVE_NONE);
                if (rank == start_rank &&
                    !s->board[(rank + 2 * direction) * 8 + file])
                    add_move(moves, capacity, &count, from,
                             (rank + 2 * direction) * 8 + file,
                             PDCHESS_MOVE_NONE);
            }
            for (delta = -1; delta <= 1; delta += 2)
            {
                int capture_file = file + delta;
                int capture_to;
                uint8_t flags;
                if (!on_board(capture_file, next_rank))
                    continue;
                capture_to = next_rank * 8 + capture_file;
                if (capture_to == s->en_passant)
                {
                    add_move(moves, capacity, &count, from, capture_to,
                             PDCHESS_MOVE_CAPTURE |
                                 PDCHESS_MOVE_EN_PASSANT);
                }
                else if (s->board[capture_to] &&
                         piece_color(s->board[capture_to]) !=
                             (pdchess_color)s->side)
                {
                    flags = PDCHESS_MOVE_CAPTURE;
                    if (next_rank == promotion_rank)
                        flags |= PDCHESS_MOVE_PROMOTION;
                    add_move(moves, capacity, &count, from, capture_to,
                             flags);
                }
            }
        }
        else if (type == PDCHESS_PIECE_KNIGHT ||
                 type == PDCHESS_PIECE_KING)
        {
            const int8_t (*steps)[2] =
                type == PDCHESS_PIECE_KNIGHT ? knight_steps : king_steps;
            size_t step_count = type == PDCHESS_PIECE_KNIGHT
                                    ? sizeof(knight_steps) /
                                          sizeof(knight_steps[0])
                                    : sizeof(king_steps) /
                                          sizeof(king_steps[0]);
            size_t i;
            for (i = 0; i < step_count; ++i)
            {
                int f = file + steps[i][0];
                int r = rank + steps[i][1];
                int to;
                if (!on_board(f, r))
                    continue;
                to = r * 8 + f;
                if (!s->board[to])
                    add_move(moves, capacity, &count, from, to,
                             PDCHESS_MOVE_NONE);
                else if (piece_color(s->board[to]) !=
                         (pdchess_color)s->side)
                    add_move(moves, capacity, &count, from, to,
                             PDCHESS_MOVE_CAPTURE);
            }

            if (type == PDCHESS_PIECE_KING &&
                !color_in_check(s, (pdchess_color)s->side))
            {
                pdchess_color enemy = opposite((pdchess_color)s->side);
                if (s->side == PDCHESS_COLOR_WHITE && from == 4)
                {
                    if ((s->castling & CASTLE_WHITE_K) &&
                        s->board[7] == PDCHESS_PIECE_ROOK &&
                        !s->board[5] && !s->board[6] &&
                        !square_attacked(s, 5, enemy) &&
                        !square_attacked(s, 6, enemy))
                        add_move(moves, capacity, &count, 4, 6,
                                 PDCHESS_MOVE_CASTLE);
                    if ((s->castling & CASTLE_WHITE_Q) &&
                        s->board[0] == PDCHESS_PIECE_ROOK &&
                        !s->board[1] && !s->board[2] && !s->board[3] &&
                        !square_attacked(s, 3, enemy) &&
                        !square_attacked(s, 2, enemy))
                        add_move(moves, capacity, &count, 4, 2,
                                 PDCHESS_MOVE_CASTLE);
                }
                else if (s->side == PDCHESS_COLOR_BLACK && from == 60)
                {
                    if ((s->castling & CASTLE_BLACK_K) &&
                        s->board[63] == -PDCHESS_PIECE_ROOK &&
                        !s->board[61] && !s->board[62] &&
                        !square_attacked(s, 61, enemy) &&
                        !square_attacked(s, 62, enemy))
                        add_move(moves, capacity, &count, 60, 62,
                                 PDCHESS_MOVE_CASTLE);
                    if ((s->castling & CASTLE_BLACK_Q) &&
                        s->board[56] == -PDCHESS_PIECE_ROOK &&
                        !s->board[57] && !s->board[58] && !s->board[59] &&
                        !square_attacked(s, 59, enemy) &&
                        !square_attacked(s, 58, enemy))
                        add_move(moves, capacity, &count, 60, 58,
                                 PDCHESS_MOVE_CASTLE);
                }
            }
        }
        else
        {
            const int8_t (*steps)[2];
            size_t step_count;
            size_t i;
            if (type == PDCHESS_PIECE_BISHOP)
            {
                steps = bishop_steps;
                step_count = 4;
            }
            else if (type == PDCHESS_PIECE_ROOK)
            {
                steps = rook_steps;
                step_count = 4;
            }
            else
            {
                static const int8_t queen_steps[][2] = {
                    {1, 1}, {-1, 1}, {-1, -1}, {1, -1}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
                steps = queen_steps;
                step_count = 8;
            }
            for (i = 0; i < step_count; ++i)
            {
                int f = file + steps[i][0];
                int r = rank + steps[i][1];
                while (on_board(f, r))
                {
                    int to = r * 8 + f;
                    if (!s->board[to])
                    {
                        add_move(moves, capacity, &count, from, to,
                                 PDCHESS_MOVE_NONE);
                    }
                    else
                    {
                        if (piece_color(s->board[to]) !=
                            (pdchess_color)s->side)
                            add_move(moves, capacity, &count, from, to,
                                     PDCHESS_MOVE_CAPTURE);
                        break;
                    }
                    f += steps[i][0];
                    r += steps[i][1];
                }
            }
        }
    }
    return count;
}

static void update_castling(engine_state *s, int from, int to, int8_t piece)
{
    if (piece == PDCHESS_PIECE_KING)
        s->castling &= 0xfcu;
    else if (piece == -PDCHESS_PIECE_KING)
        s->castling &= 0xf3u;
    if (from == 0 || to == 0)
        s->castling &= 0xfdu;
    if (from == 7 || to == 7)
        s->castling &= 0xfeu;
    if (from == 56 || to == 56)
        s->castling &= 0xf7u;
    if (from == 63 || to == 63)
        s->castling &= 0xfbu;
}

static void apply_unchecked(engine_state *s, pdchess_move move)
{
    int8_t piece = s->board[move.from];
    int8_t captured = s->board[move.to];
    int from_rank = move.from >> 3;
    int to_rank = move.to >> 3;

    update_castling(s, move.from, move.to, piece);
    s->board[move.from] = 0;
    if (move.flags & PDCHESS_MOVE_EN_PASSANT)
    {
        int captured_square = move.to +
                              (s->side == PDCHESS_COLOR_WHITE ? -8 : 8);
        captured = s->board[captured_square];
        s->board[captured_square] = 0;
    }
    s->board[move.to] = piece;
    if (move.flags & PDCHESS_MOVE_PROMOTION)
        s->board[move.to] =
            (int8_t)(color_sign((pdchess_color)s->side) *
                     PDCHESS_PIECE_QUEEN);
    if (move.flags & PDCHESS_MOVE_CASTLE)
    {
        if (move.to == 6)
        {
            s->board[5] = s->board[7];
            s->board[7] = 0;
        }
        else if (move.to == 2)
        {
            s->board[3] = s->board[0];
            s->board[0] = 0;
        }
        else if (move.to == 62)
        {
            s->board[61] = s->board[63];
            s->board[63] = 0;
        }
        else if (move.to == 58)
        {
            s->board[59] = s->board[56];
            s->board[56] = 0;
        }
    }

    s->en_passant = PDCHESS_SQUARE_NONE;
    if (piece_type(piece) == PDCHESS_PIECE_PAWN &&
        (to_rank - from_rank == 2 || from_rank - to_rank == 2))
        s->en_passant = (pdchess_square)((move.from + move.to) / 2);

    if (piece_type(piece) == PDCHESS_PIECE_PAWN || captured)
        s->halfmove = 0;
    else if (s->halfmove < UINT16_MAX)
        ++s->halfmove;
    if (s->side == PDCHESS_COLOR_BLACK && s->fullmove < UINT16_MAX)
        ++s->fullmove;
    s->side = (uint8_t)opposite((pdchess_color)s->side);
}

size_t pdchess_generate_legal_moves(pdchess_state *state,
                                    pdchess_move *moves,
                                    size_t capacity)
{
    pdchess_move pseudo[PDCHESS_MAX_LEGAL_MOVES];
    engine_state *s;
    size_t pseudo_count;
    size_t legal_count = 0;
    size_t i;

    if (!state)
        return 0;
    s = impl(state);
    pseudo_count = generate_pseudo(s, pseudo, PDCHESS_MAX_LEGAL_MOVES);
    if (pseudo_count > PDCHESS_MAX_LEGAL_MOVES)
        pseudo_count = PDCHESS_MAX_LEGAL_MOVES;

    for (i = 0; i < pseudo_count; ++i)
    {
        engine_state next = *s;
        pdchess_color mover = (pdchess_color)s->side;
        apply_unchecked(&next, pseudo[i]);
        if (!color_in_check(&next, mover))
        {
            if (legal_count < capacity && moves)
                moves[legal_count] = pseudo[i];
            ++legal_count;
        }
    }
    return legal_count;
}

static bool same_move(pdchess_move a, pdchess_move b)
{
    return a.from == b.from && a.to == b.to &&
           (!(a.flags & PDCHESS_MOVE_PROMOTION) ||
            b.promotion == PDCHESS_PIECE_NONE ||
            b.promotion == PDCHESS_PIECE_QUEEN);
}

bool pdchess_is_legal_move(pdchess_state *state, pdchess_move move)
{
    pdchess_move moves[PDCHESS_MAX_LEGAL_MOVES];
    size_t count;
    size_t i;
    if (!state)
        return false;
    count = pdchess_generate_legal_moves(state, moves,
                                         PDCHESS_MAX_LEGAL_MOVES);
    for (i = 0; i < count; ++i)
        if (same_move(moves[i], move))
            return true;
    return false;
}

bool pdchess_apply_move(pdchess_state *state, pdchess_move move)
{
    pdchess_move moves[PDCHESS_MAX_LEGAL_MOVES];
    size_t count;
    size_t i;
    if (!state)
        return false;
    count = pdchess_generate_legal_moves(state, moves,
                                         PDCHESS_MAX_LEGAL_MOVES);
    for (i = 0; i < count; ++i)
    {
        if (same_move(moves[i], move))
        {
            apply_unchecked(impl(state), moves[i]);
            return true;
        }
    }
    return false;
}

pdchess_game_status pdchess_get_status(pdchess_state *state)
{
    bool check;
    size_t moves;
    if (!state)
        return PDCHESS_GAME_STALEMATE;
    check = pdchess_is_in_check(state);
    moves = pdchess_generate_legal_moves(state, NULL, 0);
    if (moves)
        return check ? PDCHESS_GAME_CHECK : PDCHESS_GAME_PLAYING;
    return check ? PDCHESS_GAME_CHECKMATE : PDCHESS_GAME_STALEMATE;
}

static int evaluate(const engine_state *s)
{
    int score = 0;
    int square;
    for (square = 0; square < 64; ++square)
    {
        int8_t piece = s->board[square];
        int type;
        int positional = 0;
        if (!piece)
            continue;
        type = piece_type(piece);
        if (type == PDCHESS_PIECE_PAWN)
        {
            int rank = square >> 3;
            positional = piece > 0 ? rank * 6 : (7 - rank) * 6;
        }
        else if (type == PDCHESS_PIECE_KNIGHT ||
                 type == PDCHESS_PIECE_BISHOP)
        {
            int file = square & 7;
            int rank = square >> 3;
            positional = 12 - (file > 3 ? file - 4 : 3 - file) * 3 -
                         (rank > 3 ? rank - 4 : 3 - rank) * 3;
        }
        score += (piece > 0 ? 1 : -1) *
                 (piece_values[type] + positional);
    }
    return s->side == PDCHESS_COLOR_WHITE ? score : -score;
}

static bool search_should_stop(search_context *context)
{
    if (context->cancelled || context->node_limited)
        return true;
    if (context->node_limit && context->nodes >= context->node_limit)
    {
        context->node_limited = true;
        return true;
    }
    if (context->poll && context->nodes >= context->next_poll)
    {
        context->next_poll = context->nodes + context->poll_interval;
        if (!context->poll(context->userdata))
        {
            context->cancelled = true;
            return true;
        }
    }
    return false;
}

static int negamax(engine_state *s, int depth, int ply,
                   int alpha, int beta, search_context *context)
{
    pdchess_state wrapper;
    pdchess_move moves[PDCHESS_MAX_LEGAL_MOVES];
    size_t count;
    size_t i;

    ++context->nodes;
    if (search_should_stop(context))
        return evaluate(s);
    if (depth == 0)
        return evaluate(s);

    memset(&wrapper, 0, sizeof(wrapper));
    *impl(&wrapper) = *s;
    count = pdchess_generate_legal_moves(&wrapper, moves,
                                         PDCHESS_MAX_LEGAL_MOVES);
    if (!count)
        return color_in_check(s, (pdchess_color)s->side)
                   ? -SCORE_MATE + ply
                   : 0;

    for (i = 0; i < count; ++i)
    {
        engine_state child = *s;
        int score;
        apply_unchecked(&child, moves[i]);
        score = -negamax(&child, depth - 1, ply + 1,
                         -beta, -alpha, context);
        if (context->cancelled || context->node_limited)
            return alpha;
        if (score > alpha)
            alpha = score;
        if (alpha >= beta)
            break;
    }
    return alpha;
}

static int root_search(engine_state *s, uint8_t depth,
                       pdchess_move *best_move, search_context *context)
{
    pdchess_state wrapper;
    pdchess_move moves[PDCHESS_MAX_LEGAL_MOVES];
    size_t count;
    size_t i;
    int best_score = -SCORE_INF;

    memset(&wrapper, 0, sizeof(wrapper));
    *impl(&wrapper) = *s;
    count = pdchess_generate_legal_moves(&wrapper, moves,
                                         PDCHESS_MAX_LEGAL_MOVES);
    if (!count)
        return color_in_check(s, (pdchess_color)s->side)
                   ? -SCORE_MATE
                   : 0;

    for (i = 0; i < count; ++i)
    {
        engine_state child = *s;
        int score;
        apply_unchecked(&child, moves[i]);
        score = -negamax(&child, depth - 1, 1,
                         -SCORE_INF, SCORE_INF, context);
        if (context->cancelled || context->node_limited)
            break;
        if (score > best_score)
        {
            best_score = score;
            *best_move = moves[i];
        }
    }
    return best_score;
}

pdchess_search_result pdchess_search(pdchess_state *state,
                                     pdchess_search_limits limits)
{
    pdchess_search_result result;
    search_context context;
    pdchess_move fallback[1];
    size_t legal_count;
    uint8_t depth;

    memset(&result, 0, sizeof(result));
    result.best_move.from = PDCHESS_SQUARE_NONE;
    result.best_move.to = PDCHESS_SQUARE_NONE;
    result.stop_reason = PDCHESS_STOP_NO_LEGAL_MOVE;
    if (!state)
        return result;

    legal_count = pdchess_generate_legal_moves(state, fallback, 1);
    if (!legal_count)
        return result;
    result.best_move = fallback[0];
    result.has_move = true;

    memset(&context, 0, sizeof(context));
    context.node_limit = limits.node_limit;
    context.poll = limits.poll;
    context.userdata = limits.userdata;
    context.poll_interval = limits.poll_interval
                                ? limits.poll_interval
                                : 256u;
    context.next_poll = context.poll_interval;
    if (!limits.depth_limit)
        limits.depth_limit = 1;

    for (depth = 1; depth <= limits.depth_limit; ++depth)
    {
        pdchess_move iteration_best = result.best_move;
        int score;
        context.root_depth = depth;
        score = root_search(impl(state), depth, &iteration_best, &context);
        if (context.cancelled || context.node_limited)
            break;
        result.best_move = iteration_best;
        result.score_cp = score;
        result.completed_depth = depth;
    }

    result.nodes = context.nodes;
    if (context.cancelled)
        result.stop_reason = PDCHESS_STOP_CANCELLED;
    else if (context.node_limited)
        result.stop_reason = PDCHESS_STOP_NODE_LIMIT;
    else
        result.stop_reason = PDCHESS_STOP_DEPTH;
    return result;
}

const char *pdchess_status_name(pdchess_game_status status)
{
    switch (status)
    {
    case PDCHESS_GAME_PLAYING:
        return "playing";
    case PDCHESS_GAME_CHECK:
        return "check";
    case PDCHESS_GAME_CHECKMATE:
        return "checkmate";
    case PDCHESS_GAME_STALEMATE:
        return "stalemate";
    default:
        return "unknown";
    }
}

const char *pdchess_stop_reason_name(pdchess_stop_reason reason)
{
    switch (reason)
    {
    case PDCHESS_STOP_DEPTH:
        return "depth";
    case PDCHESS_STOP_NODE_LIMIT:
        return "node limit";
    case PDCHESS_STOP_CANCELLED:
        return "cancelled";
    case PDCHESS_STOP_NO_LEGAL_MOVE:
        return "no legal move";
    default:
        return "unknown";
    }
}

bool pdchess_move_to_uci(pdchess_move move, char output[6])
{
    if (!output || !pdchess_square_is_valid(move.from) ||
        !pdchess_square_is_valid(move.to))
        return false;
    output[0] = (char)('a' + pdchess_square_file(move.from));
    output[1] = (char)('1' + pdchess_square_rank(move.from));
    output[2] = (char)('a' + pdchess_square_file(move.to));
    output[3] = (char)('1' + pdchess_square_rank(move.to));
    if (move.flags & PDCHESS_MOVE_PROMOTION)
    {
        output[4] = 'q';
        output[5] = '\0';
    }
    else
    {
        output[4] = '\0';
    }
    return true;
}

bool pdchess_move_from_uci(const char *text, pdchess_move *move)
{
    size_t length;
    if (!text || !move)
        return false;
    length = strlen(text);
    if ((length != 4 && length != 5) ||
        text[0] < 'a' || text[0] > 'h' ||
        text[1] < '1' || text[1] > '8' ||
        text[2] < 'a' || text[2] > 'h' ||
        text[3] < '1' || text[3] > '8' ||
        (length == 5 && text[4] != 'q'))
        return false;
    memset(move, 0, sizeof(*move));
    move->from = pdchess_make_square((uint8_t)(text[0] - 'a'),
                                     (uint8_t)(text[1] - '1'));
    move->to = pdchess_make_square((uint8_t)(text[2] - 'a'),
                                   (uint8_t)(text[3] - '1'));
    if (length == 5)
    {
        move->promotion = PDCHESS_PIECE_QUEEN;
        move->flags = PDCHESS_MOVE_PROMOTION;
    }
    return true;
}

size_t pdchess_state_size(void)
{
    return sizeof(engine_state);
}
