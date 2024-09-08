/* C standard 99 */
#include <dirent.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

/* SDL2 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_ttf.h>

/* Using tsoding arena for memory allocation */
#define ARENA_IMPLEMENTATION
#include "arena.h"

#define GFX_PATH "gfx/tileset.png"
#define FONT_PATH "font"
#define CLIP_PATH "clip"
#define TILE_SIZE 32
#define FONT_SIZES 12
#define FONT_SIZE_FACTOR 1.2
#define WINDOW_SIZE 372
#define BOARD_SIZE 3
#define BORDER 4

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

enum win_state_type
{
  WIN_STATE_LOSE = -1,
  WIN_STATE_TIE = 0,
  WIN_STATE_WIN = 1,
  WIN_STATE_NONE = 2,
};

static const char *
win_state_type_to_string (enum win_state_type type)
{
  switch (type)
    {
    case WIN_STATE_LOSE:
      return "Lose";
    case WIN_STATE_TIE:
      return "Tie";
    case WIN_STATE_WIN:
      return "Win";
    case WIN_STATE_NONE:
      return "None";
    }
}

enum game_state
{
  GAME_STATE_NULL,
  GAME_STATE_MENU,
  GAME_STATE_PLAY,
  GAME_STATE_OVER
};

struct mouse
{
  SDL_Point coordinates;
  SDL_Texture *cursor;
};

struct board_data
{
  uint8_t tiles[3][3];

  /// To encode the status of each tile in the tiles array, we use the
  /// following bitwise scheme: 0b00000000 = Empty 0b00000001 =  X 0b00000010 =
  /// O 0b00000100 = The tile is hovered 0b00001000 = The tile is clicked
  /// 0b00010000 = The tile is a winning tile
  /// 0b00100000 = The tile is a losing tile
  /// 0b01000000 = The tile is a draw tile
  /// 0b10000000 = Reserved
  /// This scheme is represented in the BitMeaning enum.
};

struct board
{
  SDL_Texture *image;
  struct board_data *data;
};

struct AI_response
{
  int32_t score;

  SDL_bool has_move;
  SDL_Point move;
};

enum bit_meaning_type
{
  EMPTY = 0x01, // 0b00000001
  X = 0x02,     // 0b00000010
  O = 0x04,     // 0b00000100
  HOVER = 0x08, // 0b00001000
  CLICK = 0x10, // 0b00010000
  WIN = 0x20,   // 0b00100000
  LOSE = 0x40,  // 0b01000000
  DRAW = 0x80   // 0b10000000
};

struct game
{
  enum game_state game_state;
  SDL_Texture *logo;
  SDL_Texture *Os;
  SDL_Texture *Xs;
  struct mouse mouse;
  struct board *board;
};

struct app
{
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture **images;
  int image_max;
  TTF_Font ***fonts;
  int font_max;
  SDL_Texture **text_images;
  Mix_Music **musics;
  int music_max;
  struct game *game;
};

static void *context_alloc (size_t size);
static int32_t load_fonts (struct app *app);
static int32_t load_images (struct app *app);
static int32_t make_board (struct app *app, SDL_Surface *surface[]);
static int32_t load_clips (struct app *app);
static SDL_Surface *rotate_surface (SDL_Surface *surface, int increment_count);
static SDL_Surface *surface_mirror_y (SDL_Surface *surface);
static SDL_Surface *surface_mirror_x (SDL_Surface *surface);
static Uint32 get_pixel (SDL_Surface *surface, int x, int y);
static void put_pixel (SDL_Surface *surface, int x, int y, Uint32 pixelValue);
static int32_t make_border (SDL_Surface *surfaces[], SDL_Surface *board,
                            int col, int row);
static int32_t make_gui (struct app *app, SDL_Surface *surfaces[]);
static double sine_wave (double x, double freq, double amplitude,
                         double phaseOffset);
static void draw_board (struct app *app);
static void computer_move (struct app *app, int col, int row);
static void player_move (struct app *app, int col, int row);
static SDL_bool check_if_bit (struct board_data *board,
                              enum bit_meaning_type bit, int col, int row);
static void place (struct board_data *board_data, uint8_t side, int col,
                   int row);
static enum win_state_type game_over (struct board_data *board_data);
static SDL_bool check_board_full (struct board_data *board_data);
static void generate_moves_bad (struct app *app);
static struct AI_response minimax (struct board_data *last_board,
                                   enum bit_meaning_type simulated_side);
static void generate_moves (struct app *app);

static Arena default_arena = { 0 };
static Arena *context_arena = &default_arena;

int
main (int argc, char *argv[])
{
  /* The game begins by initializing SDL2 with various flags */
  Uint32 init_flags = SDL_INIT_EVERYTHING;
  Uint32 window_flags = SDL_WINDOW_SHOWN;
  Uint32 renderer_flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
  int image_flags = IMG_INIT_PNG;
  int mixer_flags = MIX_INIT_MP3 | MIX_INIT_OGG;

  // Memory allocation for app
  struct app *app = (struct app *)context_alloc (sizeof (struct app));

  if (app == NULL)
    {
      printf ("Failed to allocate memory for app\n");
      return 1;
    }

  app->game = (struct game *)context_alloc (sizeof (struct game));

  if (app->game == NULL)
    {
      printf ("Failed to allocate memory for game\n");
      return 1;
    }
  app->game->game_state = GAME_STATE_NULL;

  // SDL2 init
  if (SDL_Init (init_flags) < 0)
    {
      printf ("SDL_Init Error: %s\n", SDL_GetError ());
    }

  if (TTF_Init () < 0)
    {
      printf ("TTF_Init Error: %s\n", TTF_GetError ());
      return 1;
    }

  if (IMG_Init (image_flags) != image_flags)
    {
      printf ("IMG_Init Error: %s\n", IMG_GetError ());
      return 1;
    }

  if (Mix_Init (mixer_flags) != mixer_flags)
    {
      printf ("Mix_Init Error: %s\n", Mix_GetError ());
      return 1;
    }

  if (Mix_OpenAudio (44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0)
    {
      printf ("Mix_OpenAudio Error: %s\n", Mix_GetError ());
      return 1;
    }

  app->window = SDL_CreateWindow ("Tic-Tac-Toe", SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, WINDOW_SIZE,
                                  WINDOW_SIZE, window_flags);

  if (app->window == NULL)
    {
      printf ("SDL_CreateWindow Error: %s\n", SDL_GetError ());
      return 1;
    }

  app->renderer = SDL_CreateRenderer (app->window, -1, renderer_flags);

  if (app->renderer == NULL)
    {
      printf ("SDL_CreateRenderer Error: %s\n", SDL_GetError ());
      return 1;
    }

  SDL_ShowCursor (SDL_DISABLE);

  // Load the fonts
  load_fonts (app);

  // Load the tileset
  load_images (app);

  // Load the music
  load_clips (app);

  printf ("Initialization complete!\n");

  double x = 0.0;
  double freq = .1;
  double amplitude = 2.5;
  double phaseOffset = 0.5;

  SDL_Rect mouse_rect = { 0, 0, TILE_SIZE * 3, TILE_SIZE * 3 };

  app->game->game_state = GAME_STATE_MENU;

  Mix_VolumeMusic (20);
  Mix_PlayMusic (app->musics[0], -1);

  SDL_Event event;
  while ((app->game->game_state != GAME_STATE_OVER) == SDL_TRUE)
    {
      if (SDL_PollEvent (&event) != 0)
        {
          if (event.type == SDL_QUIT)
            {
              break;
            }
          if (event.type == SDL_MOUSEMOTION)
            {
              app->game->mouse.coordinates.x = event.motion.x;
              app->game->mouse.coordinates.y = event.motion.y;
            }
          if (event.type == SDL_MOUSEBUTTONDOWN)
            {
              if (app->game->game_state == GAME_STATE_MENU)
                {
                  app->game->game_state = GAME_STATE_PLAY;
                }
              else
                {
                  player_move (app, event.button.x, event.button.y);
                  generate_moves (app);
                }
            }
        }

      double y = sine_wave (x, freq, amplitude, phaseOffset);
      x += 0.1;

      mouse_rect.x = app->game->mouse.coordinates.x;
      mouse_rect.y = app->game->mouse.coordinates.y;

      SDL_SetRenderDrawColor (app->renderer, 0, 0, 0, 255);
      SDL_RenderClear (app->renderer);
      SDL_RenderCopy (app->renderer, app->game->board->image, NULL, NULL);
      if (app->game->game_state == GAME_STATE_MENU)
        {
          SDL_RenderCopy (app->renderer, app->game->logo, NULL,
                          &(SDL_Rect){ 60, 60 + (int)y, 270, 180 });
        }
      draw_board (app);
      SDL_RenderCopy (app->renderer, app->game->mouse.cursor, NULL,
                      &mouse_rect);
      SDL_RenderPresent (app->renderer);
    }

  arena_free (&default_arena);
  return 0;
}

static void *
context_alloc (size_t size)
{
  assert (context_arena);
  void *mem = arena_alloc (context_arena, size);
  memset (mem, 0, size);
  return mem;
}

static int32_t
load_fonts (struct app *app)
{

  int32_t result = 0;

  // Directory access
  DIR *cur_dir = NULL;
  struct dirent *entry = NULL;
  char file_path[256];

  // Start by opening the font directory
  cur_dir = opendir (FONT_PATH);

  if (!cur_dir)
    {
      printf ("Error: could not open directory\n");
      return 1;
    }

  // Loop through the directory once
  // to count the number of fonts
  while ((entry = readdir (cur_dir)) != NULL)
    {
      sprintf (file_path, "%s/%s", FONT_PATH, entry->d_name);

      struct stat st;
      stat (file_path, &st);

      if (S_ISREG (st.st_mode)
          && (strstr (strlwr (entry->d_name), ".ttf") != NULL))
        {
          printf ("Processing font: %s\n", file_path);
          app->font_max++;
        }
    }
  closedir (cur_dir);

  // Allocate memory for the fonts
  app->fonts = (TTF_Font ***)context_alloc ((size_t)app->font_max
                                            * sizeof (TTF_Font **));

  cur_dir = opendir (FONT_PATH);
  int f = 0; // current file index
  while ((entry = readdir (cur_dir)) != NULL)
    {
      sprintf (file_path, "%s/%s", FONT_PATH, entry->d_name);

      struct stat st;
      stat (file_path, &st);

      if (S_ISREG (st.st_mode)
          && (strstr (strlwr (entry->d_name), ".ttf") != NULL))
        {
          printf ("Loading font: %s\n", file_path);

          // Allocate memory for the font and its size variants
          *(app->fonts + f) = (TTF_Font **)context_alloc (
              (size_t)FONT_SIZES * sizeof (TTF_Font *));

          int current_size = 8;
          // Load the font at different sizes
          for (int i = 0; i < FONT_SIZES; i++)
            {
              // The font size is multiplied by the FONT_SIZE_FACTOR
              (*(app->fonts + f))[i] = TTF_OpenFont (file_path, current_size);

              if ((*(app->fonts + f))[i] == NULL)
                {
                  printf ("TTF_OpenFont Error: %s\n", TTF_GetError ());
                  return 1;
                }
              else
                {
                  printf ("Font '%s' at size %d loaded successfully!\n",
                          entry->d_name, current_size);
                  current_size = (int)((float)current_size * FONT_SIZE_FACTOR);
                }
            }
          f++;
        }
    }
  closedir (cur_dir);

  return result;
}

static int32_t
cleanup_images (SDL_Point surface_dim, SDL_Surface **surface_pieces)
{
  for (int row = 0; row < surface_dim.y; row++)
    {
      for (int col = 0; col < surface_dim.x; col++)
        {
          SDL_FreeSurface (surface_pieces[row * surface_dim.x + col]);
        }
    }
}

static int32_t
load_images (struct app *app)
{

  printf ("Loading images...\n");

  SDL_Surface *tileset = IMG_Load (GFX_PATH);

  if (tileset == NULL)
    {
      printf ("Error while loading tileset image: %s\n", IMG_GetError ());
      return 1;
    }

  // Calculate the tileset's dimensions
  int tileset_w, tileset_h;
  SDL_QueryTexture (SDL_CreateTextureFromSurface (app->renderer, tileset),
                    NULL, NULL, &tileset_w, &tileset_h);
  int cols = tileset_w / TILE_SIZE;
  int rows = tileset_h / TILE_SIZE;

  printf ("Tileset loaded. Dimensions: %d x %d\n", tileset_w, tileset_h);

  app->image_max = cols * rows;
  SDL_Surface **surface_pieces = (SDL_Surface **)context_alloc (
      (size_t)app->image_max * sizeof (SDL_Surface *));
  if (surface_pieces == NULL)
    {
      printf ("Error while allocating memory for the surface pieces: %s\n",
              SDL_GetError ());
      cleanup_images ((SDL_Point){ cols, rows }, surface_pieces);
      return 1;
    }

  for (int row = 0; row < rows; row++)
    {
      for (int col = 0; col < cols; col++)
        {
          int current_id = col + (row * cols);
          printf ("Blitting extracted of surface to surface piece %d!\n",
                  current_id);

          SDL_Rect extract_rect = (SDL_Rect){ .w = TILE_SIZE,
                                              .h = TILE_SIZE,
                                              .x = col * TILE_SIZE,
                                              .y = row * TILE_SIZE };
          // Allocate and initialize the surface
          surface_pieces[current_id] = SDL_CreateRGBSurfaceWithFormat (
              0, TILE_SIZE, TILE_SIZE, 32, SDL_PIXELFORMAT_RGBA32);
          SDL_BlitSurface (tileset, &extract_rect, surface_pieces[current_id],
                           NULL);
        }
    }

  make_board (app, surface_pieces);

  make_gui (app, surface_pieces);

  cleanup_images ((SDL_Point){ cols, rows }, surface_pieces);
  return 0;
}

static int32_t
make_board (struct app *app, SDL_Surface *surfaces[])
{
  printf ("Making board...\n");

  // Alloc memory for the board struct within game

  app->game->board = (struct board *)context_alloc (sizeof (struct board));

  SDL_Surface *board = SDL_CreateRGBSurfaceWithFormat (
      0, (TILE_SIZE * BOARD_SIZE) + BORDER, (TILE_SIZE * BOARD_SIZE) + BORDER,
      32, SDL_PIXELFORMAT_RGBA32);

  if (board == NULL)
    {
      printf ("Error while creating board surface: %s\n", SDL_GetError ());
      SDL_FreeSurface (board);
      return 1;
    }

  app->game->board->data
      = (struct board_data *)context_alloc (sizeof (struct board_data));
  if (app->game->board->data == NULL)
    {
      return 1;
    }

  memset (app->game->board->data->tiles, 0x1, sizeof (uint8_t) * 9);

  for (int col = 0; col < 3; col++)
    {
      for (int row = 0; row < 3; row++)
        {
          SDL_Rect dest = (SDL_Rect){ .w = TILE_SIZE,
                                      .h = TILE_SIZE,
                                      .x = (col * TILE_SIZE) + BORDER / 2,
                                      .y = (row * TILE_SIZE) + BORDER / 2 };

          // Place background tile regardless
          SDL_BlitSurface (surfaces[0], NULL, board, &dest);
        }
    }
  for (int col = 0; col < 3; col++)
    {
      for (int row = 0; row < 3; row++)
        {
          make_border (surfaces, board, col, row);
        }
    }

  app->game->board->image
      = SDL_CreateTextureFromSurface (app->renderer, board);

  if (app->game->board == NULL)
    {
      printf ("Error while creating board texture: %s\n", SDL_GetError ());
      SDL_FreeSurface (board);
      return 1;
    }

  return 0;
}

static int32_t
make_border (SDL_Surface *surfaces[], SDL_Surface *board, int col, int row)
{
  int32_t result = 0;
  switch (col)
    {
    case 0:
      switch (row)
        {
        case 0:
          {
            // tile 0,0
            SDL_Rect border_dest1
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER,
                              .y = (row * TILE_SIZE) - TILE_SIZE + BORDER };
            SDL_Rect border_dest2
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE),
                              .y = (row * TILE_SIZE) + BORDER };
            SDL_Rect corner_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) - TILE_SIZE + BORDER,
                              .y = (row * TILE_SIZE) - TILE_SIZE + BORDER };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER / 2,
                              .y = (row * TILE_SIZE) + BORDER / 2 };
            SDL_BlitSurface (surfaces[1], NULL, board, &center_grid_dest);
            SDL_BlitSurface (surfaces[5], NULL, board, &border_dest1);
            SDL_BlitSurface (surfaces[11], NULL, board, &border_dest2);
            SDL_BlitSurface (surface_mirror_x (surfaces[6]), NULL, board,
                             &corner_dest);
            break;
          }
        case 1:
          {
            // tile 1,0
            SDL_Rect border_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE),
                              .y = (row * TILE_SIZE) + BORDER };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER / 2,
                              .y = (row * TILE_SIZE) + BORDER / 2 };
            SDL_BlitSurface (surfaces[2], NULL, board, &center_grid_dest);
            SDL_BlitSurface (surfaces[11], NULL, board, &border_dest);
            break;
          }
        case 2:
          {
            // tile 2,0
            SDL_Rect border_dest1
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER,
                              .y = (row * TILE_SIZE) + BORDER };
            SDL_Rect border_dest2
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE),
                              .y = (row * TILE_SIZE) + BORDER };
            SDL_Rect corner_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) - TILE_SIZE + BORDER,
                              .y = (row * TILE_SIZE) + TILE_SIZE };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER / 2,
                              .y = (row * TILE_SIZE) + BORDER / 2 };
            SDL_BlitSurface (surface_mirror_y (surfaces[1]), NULL, board,
                             &center_grid_dest);
            SDL_BlitSurface (surfaces[3], NULL, board, &border_dest1);
            SDL_BlitSurface (surfaces[18], NULL, board, &border_dest2);
            SDL_BlitSurface (rotate_surface (surfaces[4], 2), NULL, board,
                             &corner_dest);
            break;
          }
        default:
          break;
        }
      break;
    case 1:
      switch (row)
        {
        case 0:
          {
            // tile 0,1
            SDL_Rect border_dest1
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER,
                              .y = (row * TILE_SIZE) - TILE_SIZE + BORDER };

            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER / 2,
                              .y = (row * TILE_SIZE) + BORDER / 2 };
            SDL_BlitSurface (rotate_surface (surfaces[2], 1), NULL, board,
                             &center_grid_dest);
            SDL_BlitSurface (surfaces[5], NULL, board, &border_dest1);
            break;
          }
        case 1:
          // tile 1,1
          break;
        case 2:
          {
            // tile 2,1
            SDL_Rect border_dest1
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER,
                              .y = (row * TILE_SIZE) + BORDER };

            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER / 2,
                              .y = (row * TILE_SIZE) + BORDER / 2 };
            SDL_BlitSurface (rotate_surface (surfaces[2], 3), NULL, board,
                             &center_grid_dest);
            SDL_BlitSurface (surfaces[5], NULL, board, &border_dest1);
            break;
          }
        default:
          break;
        }
      break;
    case 2:
      switch (row)
        {
        case 0:
          {
            // tile 0,2
            SDL_Rect border_dest1
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER,
                              .y = (row * TILE_SIZE) - TILE_SIZE + BORDER };
            SDL_Rect border_dest2
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + TILE_SIZE,
                              .y = (row * TILE_SIZE) + BORDER };
            SDL_Rect corner_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + TILE_SIZE,
                              .y = (row * TILE_SIZE) - TILE_SIZE + BORDER };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER / 2,
                              .y = (row * TILE_SIZE) + BORDER / 2 };
            SDL_BlitSurface (surface_mirror_x (surfaces[1]), NULL, board,
                             &center_grid_dest);
            SDL_BlitSurface (surfaces[3], NULL, board, &border_dest1);
            SDL_BlitSurface (surfaces[18], NULL, board, &border_dest2);
            SDL_BlitSurface (surfaces[4], NULL, board, &corner_dest);
            break;
          }
        case 1:
          {
            // tile 1,2

            SDL_Rect border_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + TILE_SIZE,
                              .y = (row * TILE_SIZE) + BORDER };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER / 2,
                              .y = (row * TILE_SIZE) + BORDER / 2 };
            SDL_BlitSurface (surface_mirror_x (surfaces[2]), NULL, board,
                             &center_grid_dest);
            SDL_BlitSurface (surfaces[11], NULL, board, &border_dest);
            break;
          }
        case 2:
          {
            // tile 2,0
            SDL_Rect border_dest1
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER,
                              .y = (row * TILE_SIZE) + BORDER };
            SDL_Rect border_dest2
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + TILE_SIZE,
                              .y = (row * TILE_SIZE) + BORDER };
            SDL_Rect corner_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + TILE_SIZE,
                              .y = (row * TILE_SIZE) + TILE_SIZE };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = TILE_SIZE,
                              .h = TILE_SIZE,
                              .x = (col * TILE_SIZE) + BORDER / 2,
                              .y = (row * TILE_SIZE) + BORDER / 2 };
            SDL_BlitSurface (surface_mirror_y (surface_mirror_x (surfaces[1])),
                             NULL, board, &center_grid_dest);
            SDL_BlitSurface (surfaces[5], NULL, board, &border_dest1);
            SDL_BlitSurface (surfaces[11], NULL, board, &border_dest2);
            SDL_BlitSurface (surface_mirror_y (surfaces[6]), NULL, board,
                             &corner_dest);
            break;
          }
        default:
          break;
        }
      break;
    default:
      break;
    }
  return result;
}

static int32_t
make_gui (struct app *app, SDL_Surface *surfaces[])
{
  SDL_Surface *logo = SDL_CreateRGBSurfaceWithFormat (
      0, (TILE_SIZE * 3), (TILE_SIZE * 2), 32, SDL_PIXELFORMAT_RGBA32);

  if (!logo)
    {
      printf ("Error: could not create logo surface\n");
      return 1;
    }

  int x = 0;
  int y = 0;
  int tileIndices[] = { 7, 8, 9, 14, 15, 16 };
  for (int i = 0; i < 6; i++)
    {
      int tileIndex = tileIndices[i];
      SDL_Rect src = (SDL_Rect){ 0, 0, TILE_SIZE, TILE_SIZE };
      SDL_Rect dest = (SDL_Rect){ .w = TILE_SIZE,
                                  .h = TILE_SIZE,
                                  .x = (x * TILE_SIZE),
                                  .y = (y * TILE_SIZE) };
      SDL_BlitSurface (surfaces[tileIndex], &src, logo, &dest);

      x++;
      if (x >= 3)
        {
          x = 0;
          y++;
        }
    }

  SDL_Texture *texture = SDL_CreateTextureFromSurface (app->renderer, logo);
  if (!texture)
    {
      printf ("Error: could not create texture from logo surface: %s\n",
              SDL_GetError ());
      SDL_FreeSurface (logo);
      return 1;
    }

  app->game->logo = texture;

  app->game->mouse.cursor
      = SDL_CreateTextureFromSurface (app->renderer, surfaces[26]);
  app->game->Os = SDL_CreateTextureFromSurface (app->renderer, surfaces[19]);
  app->game->Xs = SDL_CreateTextureFromSurface (app->renderer, surfaces[20]);

  return 0;
}

static int32_t
load_clips (struct app *app)
{
  int32_t result = 0;
  // Directory access
  DIR *cur_dir = NULL;
  struct dirent *entry = NULL;
  char file_path[256];
  // Start by opening the music directory
  cur_dir = opendir (CLIP_PATH);
  if (!cur_dir)
    {
      printf ("Error: could not open directory\n");
      return 1;
    }

  // Loop once through the directory to count the files
  while ((entry = readdir (cur_dir)) != NULL)
    {
      sprintf (file_path, "%s/%s", CLIP_PATH, entry->d_name);

      struct stat st;
      stat (file_path, &st);

      if (S_ISREG (st.st_mode) && strstr (entry->d_name, ".mp3") != NULL)
        {

          printf ("Processing %s\n", file_path);

          // Increment the music counter
          app->music_max++;
        }
    }
  closedir (cur_dir);

  // Allocate memory for the music pointers
  // based on the number of files found
  app->musics = (Mix_Music **)context_alloc ((size_t)app->music_max
                                             * sizeof (Mix_Music *));

  // Loop through the directory again to load the music
  cur_dir = opendir (CLIP_PATH);
  int m = 0; // current file index
  while ((entry = readdir (cur_dir)) != NULL)
    {
      sprintf (file_path, "%s/%s", CLIP_PATH, entry->d_name);

      struct stat st;
      stat (file_path, &st);

      if (S_ISREG (st.st_mode)
          && (strstr (strlwr (entry->d_name), ".mp3") != NULL))
        {
          // Load the music file
          app->musics[m] = Mix_LoadMUS (file_path);
          if (app->musics[m] == NULL)
            {
              printf ("Mix_LoadMUS Error: %s\n", Mix_GetError ());
              return 1;
            }
          else
            {
              printf ("Music titled '%s' loaded successfully!\n", file_path);
              m++;
            }
        }
    }

  closedir (cur_dir);
  return result;
}

static SDL_Surface *
rotate_surface (SDL_Surface *surface, int increment_count)
{
  // calculate the final width and height after rotation
  int newWidth = surface->w;
  int newHeight = surface->h;
  if (increment_count % 2 != 0)
    {
      newWidth = surface->h;
      newHeight = surface->w;
    }
  if (increment_count < 0)
    {
      increment_count += 4;
    }

  // create a new surface with the final dimensions
  SDL_Surface *newSurface = SDL_CreateRGBSurface (
      0, newWidth, newHeight, surface->format->BitsPerPixel,
      surface->format->Rmask, surface->format->Gmask, surface->format->Bmask,
      surface->format->Amask);

  // copy pixels from original surface to new surface with rotated coordinates
  for (int x = 0; x < surface->w; x++)
    {
      for (int y = 0; y < surface->h; y++)
        {
          Uint32 pixel = get_pixel (surface, x, y);
          int newX, newY;
          switch (increment_count)
            {
            case 0:
              newX = x;
              newY = y;
              break;
            case 1:
              newX = surface->h - y - 1;
              newY = x;
              break;
            case 2:
              newX = surface->w - x - 1;
              newY = surface->h - y - 1;
              break;
            case 3:
              newX = y;
              newY = surface->w - x - 1;
              break;
            default:
              printf ("There was an error while rotating the surface. \n");
              break;
            }
          put_pixel (newSurface, newX, newY, pixel);
        }
    }

  return newSurface;
}

static Uint32
get_pixel (SDL_Surface *surface, int x, int y)
{
  Uint8 *pixelData = (Uint8 *)surface->pixels;
  int index = y * surface->pitch + x * (int)sizeof (Uint32);
  return *((Uint32 *)(pixelData + index));
}

static void
put_pixel (SDL_Surface *surface, int x, int y, Uint32 pixelValue)
{
  Uint8 *pixelData = (Uint8 *)surface->pixels;
  int index = y * surface->pitch + x * (int)sizeof (Uint32);
  *((Uint32 *)(pixelData + index)) = pixelValue;
}

static SDL_Surface *
surface_mirror_x (SDL_Surface *surface)
{
  // Get the dimensions of the surface
  int width = surface->w;
  int height = surface->h;

  // Create a new surface with the same dimensions and format as the original
  SDL_Surface *mirrored_surface = SDL_CreateRGBSurface (
      0, width, height, surface->format->BitsPerPixel, surface->format->Rmask,
      surface->format->Gmask, surface->format->Bmask, surface->format->Amask);

  // Iterate through each row of pixels in the original surface
  for (int y = 0; y < height; y++)
    {
      // Iterate through each pixel in the row, from right to left
      for (int x = width - 1; x >= 0; x--)
        {
          // Copy the color of the pixel to the corresponding position in the
          // mirrored surface
          Uint32 pixel = ((Uint32 *)surface->pixels)[y * width + x];
          ((Uint32 *)mirrored_surface->pixels)[y * width + (width - 1 - x)]
              = pixel;
        }
    }

  return mirrored_surface;
}

static SDL_Surface *
surface_mirror_y (SDL_Surface *surface)
{
  // Get the dimensions of the surface
  int width = surface->w;
  int height = surface->h;

  // Create a new surface with the same dimensions and format as the original
  SDL_Surface *mirrored_surface = SDL_CreateRGBSurface (
      0, width, height, surface->format->BitsPerPixel, surface->format->Rmask,
      surface->format->Gmask, surface->format->Bmask, surface->format->Amask);

  // Iterate through each column of pixels in the original surface
  for (int x = 0; x < width; x++)
    {
      // Iterate through each pixel in the column, from bottom to top
      for (int y = height - 1; y >= 0; y--)
        {
          // Copy the color of the pixel to the corresponding position in the
          // mirrored surface
          Uint32 pixel = ((Uint32 *)surface->pixels)[y * width + x];
          ((Uint32 *)mirrored_surface->pixels)[(height - 1 - y) * width + x]
              = pixel;
        }
    }

  return mirrored_surface;
}

static double
sine_wave (double x, double freq, double amplitude, double phaseOffset)
{
  double y = amplitude * sin (2 * M_PI * freq * x + phaseOffset)
             - (amplitude / 2.0);
  return y;
}

static void
player_move (struct app *app, int col, int row)
{

  int tile_x = -1;
  int tile_y = -1;

  if (col < WINDOW_SIZE / 3)
    {
      // Player clicked in the left third of the window
      tile_x = 0;
    }
  else if (col < WINDOW_SIZE * 2 / 3)
    {
      // Player clicked in the middle third of the window
      tile_x = 1;
    }
  else
    {
      // Player clicked in the right third of the window
      tile_x = 2;
    }

  if (row < WINDOW_SIZE / 3)
    {
      // Player clicked in the top third of the window
      tile_y = 0;
    }
  else if (row < WINDOW_SIZE * 2 / 3)
    {
      // Player clicked in the middle third of the window
      tile_y = 1;
    }
  else
    {
      // Player clicked in the bottom third of the window
      tile_y = 2;
    }
  if (check_if_bit (app->game->board->data, EMPTY, tile_x, tile_y))
    {
      place (app->game->board->data, 0, tile_x, tile_y);
    }
}

static void
computer_move (struct app *app, int col, int row)
{
  if (check_if_bit (app->game->board->data, EMPTY, col, row))
    {
      place (app->game->board->data, 1, col, row);
    }
}

static void
draw_board (struct app *app)
{

  for (int col = 0; col < 3; col++)
    {
      for (int row = 0; row < 3; row++)
        {

          SDL_Rect dest = (SDL_Rect){ .w = TILE_SIZE * 4,
                                      .h = TILE_SIZE * 4,
                                      .x = (col * TILE_SIZE * 4) - BORDER / 2,
                                      .y = (row * TILE_SIZE * 4) - BORDER };

          // Draw the board square at (x, y)
          if ((app->game->board->data->tiles[col][row] & EMPTY) == EMPTY)
            {
            }
          else
            {
              if ((app->game->board->data->tiles[col][row] & X) == X)
                {
                  SDL_RenderCopy (app->renderer, app->game->Xs, NULL, &dest);
                }
              else if ((app->game->board->data->tiles[col][row] & O) == O)
                {
                  SDL_RenderCopy (app->renderer, app->game->Os, NULL, &dest);
                }
            }
        }
    }
}

static SDL_bool
check_if_bit (struct board_data *board, enum bit_meaning_type bit, int col,
              int row)
{
  if ((board->tiles[col][row] & bit) == bit)
    {
      /* The square is not empty */
      return SDL_TRUE;
    }
  else
    {
      /* The square is empty */
      return SDL_FALSE;
    }
}

/**
 * This function places the current player's symbol in the selected square. In
 * a board_data (either the real board or a predicted future).
 * @param board_data Board affected by the function
 * @param side Side to place on the board
 * @param col X coordinate for the square
 * @param row Y coordinate for the square
 */
static void
place (struct board_data *board_data, uint8_t side, int col, int row)
{
  // Check to see if the space is empty
  if ((board_data->tiles[col][row] & EMPTY) == EMPTY)
    {
      // If it is empty, mark it as not empty
      board_data->tiles[col][row] &= ~EMPTY;
    }
  // Mark the space with the current player's piece
  board_data->tiles[col][row] |= (X << side);
}

static void
generate_moves_bad (struct app *app)
{
  SDL_bool finished = SDL_FALSE;
  for (int row = 0; row < 3; row++)
    {
      if (finished == SDL_TRUE)
        {
          break;
        }
      for (int col = 0; col < 3; col++)
        {
          if (check_if_bit (app->game->board->data, EMPTY, col, row)
              == SDL_TRUE)
            {
              computer_move (app, col, row);
              finished = SDL_TRUE;
              break;
            }
        }
    }

  game_over (app->game->board->data);
}

static SDL_bool
check_board_full (struct board_data *board_data)
{
  SDL_bool is_full = SDL_TRUE;
  for (uint8_t col = 0; col < BOARD_SIZE; col++)
    {
      for (uint8_t row = 0; row < BOARD_SIZE; row++)
        {
          if (check_if_bit (board_data, EMPTY, col, row) == SDL_TRUE)
            {
              is_full = SDL_FALSE;
            }
        }
    }
  return is_full;
}

/**
 * Checks the three possible win condition for a side.
 * @param board_data Allows flexibility by checking any future board
 * @param side Side (expecting the bit_meaning_type X or O)
 * @return SDL_TRUE (win) or SDL_FALSE (lose)
 */
static SDL_bool
check_side_win_state (struct board_data *board_data,
                      enum bit_meaning_type side)
{
  /* Check columns */
  SDL_bool column_win;
  for (uint8_t row = 0; row < BOARD_SIZE; row++)
    {
      column_win = SDL_TRUE;
      for (uint8_t col = 0; col < BOARD_SIZE && column_win; col++)
        {
          if (check_if_bit (board_data, side, col, row) == SDL_FALSE)
            {
              column_win = SDL_FALSE;
            }
        }
      if (column_win)
        {
          return SDL_TRUE;
        }
    }
  /* Check rows */
  /* Same as above except col-first order */
  SDL_bool row_win;
  for (uint8_t col = 0; col < BOARD_SIZE; col++)
    {
      row_win = SDL_TRUE;
      for (uint8_t row = 0; row < BOARD_SIZE && row_win; row++)
        {
          if (check_if_bit (board_data, side, col, row) == SDL_FALSE)
            {
              row_win = SDL_FALSE;
            }
        }
      if (row_win)
        {
          return SDL_TRUE;
        }
    }

  /* Check diagonals */
  SDL_bool diag_win = SDL_TRUE;
  for (uint8_t dia = 0; dia < BOARD_SIZE; dia++)
    {
      if (check_if_bit (board_data, side, dia, dia) == SDL_FALSE)
        {
          diag_win = SDL_FALSE;
        }
    }

  if (diag_win)
    {
      return SDL_TRUE;
    }

  diag_win = SDL_TRUE;
  for (uint8_t dia = 0; dia < BOARD_SIZE; dia++)
    {
      /* Order on column value start from the end of the board for diagonal 2.
       */
      if (check_if_bit (board_data, side, BOARD_SIZE - (dia + 1), dia)
          == SDL_FALSE)
        {
          diag_win = SDL_FALSE;
        }
    }

  if (diag_win)
    {
      return SDL_TRUE;
    }

  return SDL_FALSE;
}

/**
 *
 * @param board_data
 * @return
 */
static enum win_state_type
game_over (struct board_data *board_data)
{
  if (check_side_win_state (board_data, X) == SDL_TRUE)
    {
      return WIN_STATE_WIN;
    }
  if (check_side_win_state (board_data, O) == SDL_TRUE)
    {
      return WIN_STATE_LOSE;
    }
  if (check_board_full (board_data) == SDL_TRUE)
    {
      return WIN_STATE_TIE;
    }

  return WIN_STATE_NONE;
}

static Arena move_arena = { 0 };

static struct AI_response
minimax (struct board_data *last_board, enum bit_meaning_type simulated_side)
{
  struct AI_response new_response;
  /* Checks if the game is over (someone won or the board is full) */
  enum win_state_type board_win_state = game_over (last_board);

  /* A None state would mean the game is still on-going*/
  if (board_win_state != WIN_STATE_NONE)
    {
      SDL_LogInfo (SDL_LOG_CATEGORY_APPLICATION,
                   "AI found a terminal move of type: %s",
                   win_state_type_to_string (board_win_state));
      new_response.score = (int32_t)board_win_state;
      new_response.has_move = SDL_FALSE;
      return new_response;
    }

  /* Set the best score depending on which side is simulated. */
  int32_t best_score = simulated_side == O ? INT32_MIN : INT32_MAX;
  SDL_Point best_move;
  SDL_bool move_found = SDL_FALSE;

  for (uint8_t col = 0; col < 3; col++)
    {
      for (uint8_t row = 0; row < 3; row++)
        {
          if (check_if_bit (last_board, EMPTY, col, row) == SDL_TRUE)
            {
              struct board_data *new_board
                  = (struct board_data *)context_alloc (
                      sizeof (struct board_data));
              if (new_board == NULL)
                {
                  // TODO Error message
                  context_arena = &default_arena;
                  arena_free (&move_arena);
                  exit (0);
                }
              memcpy (new_board->tiles, last_board->tiles,
                      sizeof (uint8_t) * BOARD_SIZE * BOARD_SIZE);

              place (new_board, (uint8_t)simulated_side, col, row);

              struct AI_response simulated_response
                  = minimax (new_board, simulated_side == O ? X : O);

              if (simulated_side == O)
                {
                  if (simulated_response.score > best_score)
                    {
                      best_score = simulated_response.score;
                      best_move.x = col;
                      best_move.y = row;
                      move_found = SDL_TRUE;
                    }
                }
              else if (simulated_side == X)
                {
                  if (simulated_response.score < best_score)
                    {
                      best_score = simulated_response.score;
                      best_move.x = col;
                      best_move.y = row;
                      move_found = SDL_TRUE;
                    }
                }
            }
        }
    }
  new_response.score = best_score;
  new_response.has_move = move_found;
  if (move_found)
    {
      new_response.move = best_move;
    }
  return new_response;
}

/**
 * Plays the AI move.
 * @param current_board the last board
 * @return
 */
static void
generate_moves (struct app *app)
{
  context_arena = &move_arena;

  struct AI_response response = minimax (app->game->board->data, O);
  computer_move (app, response.move.x, response.move.y);

  game_over (app->game->board->data);

  context_arena = &default_arena;
  arena_free (&move_arena);
}
