#include "pd_api.h"
#include "pdchess.h"

#include <stdio.h>
#include <string.h>

#define BOARD_SIZE 240
#define CELL_SIZE 30
#define PANEL_X 248

typedef enum
{
    SCREEN_SETUP,
    SCREEN_BOARD
} screen_mode;

typedef struct
{
    const char *name;
    const char *fen;
} lab_position;

static const lab_position positions[] = {
    {"Start", NULL},
    {"Castling", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"},
    {"En passant", "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1"},
    {"Promotion", "7k/P7/8/8/8/8/8/7K w - - 0 1"},
    {"Mate", "7k/6Q1/6K1/8/8/8/8/8 b - - 0 1"},
    {"Stalemate", "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"},
};

static PlaydateAPI *pd;
static pdchess_state game;
static screen_mode screen = SCREEN_SETUP;
static pdchess_color human_side = PDCHESS_COLOR_WHITE;
static int difficulty = 1;
static int position_index;
static pdchess_square cursor;
static pdchess_square selected = PDCHESS_SQUARE_NONE;
static pdchess_move legal_moves[PDCHESS_MAX_LEGAL_MOVES];
static size_t legal_count;
static int crank_index;
static pdchess_move last_move;
static bool has_last_move;
static bool ai_pending;
static bool ai_thinking;
static pdchess_search_result last_search;
static uint32_t last_search_ms;
static uint32_t search_deadline;
static LCDFont *font;
static PDMenuItem *position_menu;

static const uint32_t node_limits[] = {2500, 15000, 75000};
static const uint8_t depth_limits[] = {3, 5, 7};
static const uint32_t time_limits_ms[] = {250, 1000, 3000};
static const char *difficulty_names[] = {"Easy", "Medium", "Hard"};

static void refresh_legal_moves(void)
{
    legal_count = pdchess_generate_legal_moves(
        &game, legal_moves, PDCHESS_MAX_LEGAL_MOVES);
}

static void load_position(int index)
{
    position_index = index;
    if (positions[index].fen)
        pdchess_set_fen(&game, positions[index].fen);
    else
        pdchess_init(&game);
    selected = PDCHESS_SQUARE_NONE;
    has_last_move = false;
    memset(&last_search, 0, sizeof(last_search));
    last_search_ms = 0;
    cursor = human_side == PDCHESS_COLOR_WHITE
                 ? pdchess_make_square(4, 1)
                 : pdchess_make_square(4, 6);
    refresh_legal_moves();
    ai_pending = positions[index].fen == NULL &&
                 pdchess_side_to_move(&game) != human_side;
}

static void reset_menu(void *userdata)
{
    (void)userdata;
    load_position(position_index);
}

static void next_position_menu(void *userdata)
{
    (void)userdata;
    load_position((position_index + 1) %
                  (int)(sizeof(positions) / sizeof(positions[0])));
}

static void position_option_changed(void *userdata)
{
    (void)userdata;
    if (position_menu)
        load_position(pd->system->getMenuItemValue(position_menu));
}

static bool search_poll(void *userdata)
{
    PlaydateAPI *api = userdata;
    return api->system->getCurrentTimeMilliseconds() < search_deadline;
}

static int display_file(int column)
{
    return human_side == PDCHESS_COLOR_WHITE ? column : 7 - column;
}

static int display_rank(int row)
{
    return human_side == PDCHESS_COLOR_WHITE ? 7 - row : row;
}

static void square_to_screen(pdchess_square square, int *x, int *y)
{
    int file = pdchess_square_file(square);
    int rank = pdchess_square_rank(square);
    int column = human_side == PDCHESS_COLOR_WHITE ? file : 7 - file;
    int row = human_side == PDCHESS_COLOR_WHITE ? 7 - rank : rank;
    *x = column * CELL_SIZE;
    *y = row * CELL_SIZE;
}

static bool is_legal_target(pdchess_square square)
{
    size_t i;
    if (selected == PDCHESS_SQUARE_NONE)
        return false;
    for (i = 0; i < legal_count; ++i)
        if (legal_moves[i].from == selected &&
            legal_moves[i].to == square)
            return true;
    return false;
}

static void draw_piece(pdchess_square square, pdchess_piece piece)
{
    static const char letters[] = " PNBRQK";
    char text[2] = {letters[piece.type], '\0'};
    int x;
    int y;
    int width;
    square_to_screen(square, &x, &y);

    if (piece.color == PDCHESS_COLOR_BLACK)
    {
        pd->graphics->fillEllipse(x + 5, y + 5, 20, 20, 0, 360,
                                  kColorBlack);
        pd->graphics->setDrawMode(kDrawModeFillWhite);
    }
    else
    {
        pd->graphics->fillEllipse(x + 5, y + 5, 20, 20, 0, 360,
                                  kColorWhite);
        pd->graphics->drawEllipse(x + 5, y + 5, 20, 20, 2, 0, 360,
                                  kColorBlack);
        pd->graphics->setDrawMode(kDrawModeCopy);
    }
    width = pd->graphics->getTextWidth(font, text, 1, kASCIIEncoding, 0);
    pd->graphics->drawText(text, 1, kASCIIEncoding,
                           x + (CELL_SIZE - width) / 2, y + 7);
    pd->graphics->setDrawMode(kDrawModeCopy);
}

static void draw_board(void)
{
    int row;
    int column;
    for (row = 0; row < 8; ++row)
    {
        for (column = 0; column < 8; ++column)
        {
            int x = column * CELL_SIZE;
            int y = row * CELL_SIZE;
            int file = display_file(column);
            int rank = display_rank(row);
            pdchess_square square = pdchess_make_square(
                (uint8_t)file, (uint8_t)rank);
            pdchess_piece piece = pdchess_get_piece(&game, square);

            pd->graphics->fillRect(x, y, CELL_SIZE, CELL_SIZE,
                                   ((file + rank) & 1) ? kColorBlack : kColorWhite);
            if (is_legal_target(square))
            {
                pd->graphics->fillEllipse(x + 11, y + 11, 8, 8,
                                          0, 360, kColorXOR);
            }
            if (has_last_move &&
                (last_move.from == square || last_move.to == square))
                pd->graphics->drawRect(x + 2, y + 2,
                                       CELL_SIZE - 4, CELL_SIZE - 4,
                                       kColorXOR);
            if (selected == square)
                pd->graphics->drawRect(x + 3, y + 3,
                                       CELL_SIZE - 6, CELL_SIZE - 6,
                                       kColorXOR);
            if (piece.type != PDCHESS_PIECE_NONE)
                draw_piece(square, piece);
            if (cursor == square)
                pd->graphics->drawRect(x + 1, y + 1,
                                       CELL_SIZE - 2, CELL_SIZE - 2,
                                       kColorXOR);
        }
    }
    pd->graphics->drawRect(0, 0, BOARD_SIZE, BOARD_SIZE, kColorBlack);
}

static void draw_line(const char *text, int line)
{
    pd->graphics->drawText(text, strlen(text), kASCIIEncoding,
                           PANEL_X, 8 + line * 17);
}

static void draw_panel(void)
{
    char text[64];
    char move_text[6];
    pdchess_game_status status = pdchess_get_status(&game);

    draw_line("pdchess lab", 0);
    snprintf(text, sizeof(text), "%s / %s",
             human_side == PDCHESS_COLOR_WHITE ? "White" : "Black",
             difficulty_names[difficulty]);
    draw_line(text, 1);
    snprintf(text, sizeof(text), "Position: %s",
             positions[position_index].name);
    draw_line(text, 2);
    snprintf(text, sizeof(text), "Turn: %s",
             pdchess_side_to_move(&game) == PDCHESS_COLOR_WHITE
                 ? "White"
                 : "Black");
    draw_line(text, 3);
    snprintf(text, sizeof(text), "State: %s",
             pdchess_status_name(status));
    draw_line(text, 4);

    if (has_last_move && pdchess_move_to_uci(last_move, move_text))
    {
        snprintf(text, sizeof(text), "Move: %s", move_text);
        draw_line(text, 5);
    }
    if (ai_thinking)
    {
        draw_line("AI: thinking...", 7);
    }
    else if (last_search.has_move)
    {
        snprintf(text, sizeof(text), "Score: %ld cp",
                 (long)last_search.score_cp);
        draw_line(text, 7);
        snprintf(text, sizeof(text), "Nodes: %lu",
                 (unsigned long)last_search.nodes);
        draw_line(text, 8);
        snprintf(text, sizeof(text), "Depth: %u / %lu ms",
                 last_search.completed_depth,
                 (unsigned long)last_search_ms);
        draw_line(text, 9);
        snprintf(text, sizeof(text), "Stop: %s",
                 pdchess_stop_reason_name(last_search.stop_reason));
        draw_line(text, 10);
    }
    draw_line("D-pad cursor", 12);
    draw_line("A select/move", 13);
    draw_line("B cancel", 14);
    draw_line("Crank targets", 15);
    draw_line("Menu: positions", 16);
}

static void draw_setup(void)
{
    char text[80];
    pd->graphics->drawText("pdchess Developer Lab", 21, kASCIIEncoding,
                           95, 45);
    pd->graphics->drawText("Reusable engine capability demo", 31,
                           kASCIIEncoding, 75, 72);
    snprintf(text, sizeof(text), "Up/Down side: %s",
             human_side == PDCHESS_COLOR_WHITE ? "White" : "Black");
    pd->graphics->drawText(text, strlen(text), kASCIIEncoding, 105, 112);
    snprintf(text, sizeof(text), "Left/Right AI: %s",
             difficulty_names[difficulty]);
    pd->graphics->drawText(text, strlen(text), kASCIIEncoding, 105, 136);
    pd->graphics->drawText("A: open interactive lab", 23,
                           kASCIIEncoding, 110, 178);
}

static void move_cursor(int dx, int dy)
{
    int file = pdchess_square_file(cursor);
    int rank = pdchess_square_rank(cursor);
    if (human_side == PDCHESS_COLOR_BLACK)
    {
        dx = -dx;
        dy = -dy;
    }
    file += dx;
    rank -= dy;
    if (file < 0)
        file = 0;
    if (file > 7)
        file = 7;
    if (rank < 0)
        rank = 0;
    if (rank > 7)
        rank = 7;
    cursor = pdchess_make_square((uint8_t)file, (uint8_t)rank);
}

static void select_or_move(void)
{
    size_t i;
    pdchess_piece piece;
    pdchess_color controlled_side = position_index == 0
                                        ? human_side
                                        : pdchess_side_to_move(&game);
    if ((position_index == 0 &&
         pdchess_side_to_move(&game) != human_side) ||
        pdchess_get_status(&game) >= PDCHESS_GAME_CHECKMATE)
        return;
    if (selected != PDCHESS_SQUARE_NONE)
    {
        for (i = 0; i < legal_count; ++i)
        {
            if (legal_moves[i].from == selected &&
                legal_moves[i].to == cursor)
            {
                last_move = legal_moves[i];
                pdchess_apply_move(&game, last_move);
                has_last_move = true;
                selected = PDCHESS_SQUARE_NONE;
                refresh_legal_moves();
                ai_pending = pdchess_get_status(&game) <
                                 PDCHESS_GAME_CHECKMATE &&
                             pdchess_side_to_move(&game) != human_side;
                return;
            }
        }
    }
    piece = pdchess_get_piece(&game, cursor);
    if (piece.type != PDCHESS_PIECE_NONE &&
        piece.color == controlled_side)
    {
        selected = cursor;
        crank_index = -1;
    }
}

static void cycle_crank_target(int direction)
{
    pdchess_square targets[32];
    int count = 0;
    size_t i;
    if (selected == PDCHESS_SQUARE_NONE)
        return;
    for (i = 0; i < legal_count && count < 32; ++i)
        if (legal_moves[i].from == selected)
            targets[count++] = legal_moves[i].to;
    if (!count)
        return;
    crank_index = (crank_index + direction + count) % count;
    cursor = targets[crank_index];
}

static void run_ai(void)
{
    pdchess_search_limits limits;
    uint32_t started = pd->system->getCurrentTimeMilliseconds();
    memset(&limits, 0, sizeof(limits));
    limits.node_limit = node_limits[difficulty];
    limits.depth_limit = depth_limits[difficulty];
    limits.poll = search_poll;
    limits.userdata = pd;
    limits.poll_interval = 128;
    search_deadline = started + time_limits_ms[difficulty];
    last_search = pdchess_search(&game, limits);
    last_search_ms = pd->system->getCurrentTimeMilliseconds() - started;
    if (last_search.has_move)
    {
        last_move = last_search.best_move;
        pdchess_apply_move(&game, last_move);
        has_last_move = true;
    }
    refresh_legal_moves();
    ai_pending = false;
    ai_thinking = false;
}

static int update(void *userdata)
{
    PDButtons current;
    PDButtons pushed;
    PDButtons released;
    float crank;
    bool run_ai_now = ai_thinking;
    (void)userdata;
    (void)current;
    (void)released;

    pd->system->getButtonState(&current, &pushed, &released);
    pd->graphics->clear(kColorWhite);
    pd->graphics->setFont(font);

    if (screen == SCREEN_SETUP)
    {
        if (pushed & (kButtonUp | kButtonDown))
            human_side = human_side == PDCHESS_COLOR_WHITE
                             ? PDCHESS_COLOR_BLACK
                             : PDCHESS_COLOR_WHITE;
        if (pushed & kButtonLeft)
            difficulty = (difficulty + 2) % 3;
        if (pushed & kButtonRight)
            difficulty = (difficulty + 1) % 3;
        if (pushed & kButtonA)
        {
            screen = SCREEN_BOARD;
            load_position(0);
        }
        draw_setup();
        return 1;
    }

    if (!ai_thinking)
    {
        if (pushed & kButtonLeft)
            move_cursor(-1, 0);
        if (pushed & kButtonRight)
            move_cursor(1, 0);
        if (pushed & kButtonUp)
            move_cursor(0, -1);
        if (pushed & kButtonDown)
            move_cursor(0, 1);
        if (pushed & kButtonA)
            select_or_move();
        if (pushed & kButtonB)
            selected = PDCHESS_SQUARE_NONE;
        crank = pd->system->getCrankChange();
        if (crank >= 8.0f)
            cycle_crank_target(1);
        if (crank <= -8.0f)
            cycle_crank_target(-1);
    }

    if (ai_pending && !ai_thinking)
        ai_thinking = true;
    draw_board();
    draw_panel();
    if (run_ai_now)
        run_ai();
    return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg)
{
    (void)arg;
    if (event == kEventInit)
    {
        static const char *position_names[] = {
            "Start", "Castling", "En passant",
            "Promotion", "Mate", "Stalemate"};
        const char *error;
        pd = playdate;
        font = pd->graphics->loadFont(
            "/System/Fonts/Asheville-Sans-14-Bold.pft", &error);
        if (!font)
            pd->system->error("Could not load font: %s", error);
        pd->system->addMenuItem("Reset position", reset_menu, NULL);
        pd->system->addMenuItem("Next position", next_position_menu, NULL);
        position_menu = pd->system->addOptionsMenuItem(
            "Test position", position_names,
            (int)(sizeof(position_names) / sizeof(position_names[0])),
            position_option_changed, NULL);
        pd->system->setUpdateCallback(update, NULL);
    }
    return 0;
}
