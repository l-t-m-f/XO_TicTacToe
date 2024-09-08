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
#include <stdio.h>

#define XO_GFX_PATH "gfx/tileset.png"
#define XO_FONT_PATH "font"
#define XO_CLIP_PATH "clip"
#define XO_TILE_SIZE 32
#define XO_FONT_SIZES 12
#define XO_FONT_SIZE_FACTOR 1.2
#define XO_WINDOW_SIZE 372
#define XO_BOARD_SIZE 3
#define XO_BORDER 4

enum xo_win_state_type
{
  XO_WIN_STATE_LOSE = -1,
  XO_WIN_STATE_TIE = 0,
  XO_WIN_STATE_WIN = 1,
  XO_WIN_STATE_NONE = 2,
};

enum xo_game_state
{
  XO_GAME_STATE_NULL,
  XO_GAME_STATE_MENU,
  XO_GAME_STATE_PLAY,
  XO_GAME_STATE_OVER
};

struct xo_mouse
{
  SDL_Point coordinates;
  SDL_Texture *cursor;
};

struct xo_board_data
{
  uint8_t squares[3][3];

  /* To encode the status of each square in the squares array, we use the
   * following bitwise scheme:
   *
   * 0b00000001 = Empty
   * 0b00000010 = X
   * 0b00000100 = O
   * 0b00001000 = The tile is hovered
   * 0b00010000 = The tile is clicked
   * 0b00100000
   * to
   * 0b10000000 = Reserved
   * This scheme is represented in the bit_meaning_type enum.
   */
};

struct xo_board
{
  SDL_Texture *image;
  struct xo_board_data *data;
};

struct xo_cpu_response
{
  int32_t score;

  SDL_bool has_move;
  SDL_Point move;
};

enum xo_bit_meaning_type
{
  XO_BIT_MEANING_EMPTY = 1 << 0,  /* 0b00000001 */
  XO_BIT_MEANING_SIDE_X = 1 << 1, /* 0b00000010 */
  XO_BIT_MEANING_SIDE_O = 1 << 2, /* 0b00000100 */
  XO_BIT_MEANING_HOVER = 1 << 3,  /* 0b00001000 */
  XO_BIT_MEANING_CLICK = 1 << 4,  /* 0b00010000 */
};

struct xo_game
{
  enum xo_game_state game_state;
  SDL_Texture *logo;
  SDL_Texture *Os;
  SDL_Texture *Xs;
  struct xo_mouse mouse;
  struct xo_board *board;
};

struct xo_app
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
  struct xo_game *game;
};

struct xo_stack
{
  void *bits;
  size_t size;
  size_t offset;
};

/* Debug mode */

#define XO_DEBUG_LOG_NONE 0
#define XO_DEBUG_LOG_BASE 1
#define XO_DEBUG_LOG_ALL 2

#define XO_DEBUG_LOG XO_DEBUG_LOG_ALL

struct xo_stack *generic = { 0 };
struct xo_stack *minimax_stack = { 0 };

/// LOGGING

/**
 * Produces a message in the output console through SDL_LogMessageV()
 * internally.
 * @param db_order The message is only produced if the db_order is higher or
 * equal to the current DEBUG_LOG status value.
 * @param b_is_warn The message is produced as with warning priority if true
 * @param format Message format (stdio format)
 * @param ... Format parameters
 */
static void
xo_log_debug (int32_t db_order, SDL_bool b_is_warn, const char *format, ...)
{
  va_list args;
  va_start (args, format);
  if (XO_DEBUG_LOG >= db_order)
    {
      SDL_LogMessageV (SDL_LOG_CATEGORY_APPLICATION,
                       b_is_warn ? SDL_LOG_PRIORITY_WARN
                                 : SDL_LOG_PRIORITY_INFO,
                       format, args);
    }
  va_end (args);
}

/**
 * Produces a message in the output console through SDL_LogMessageV()
 * internally, automatically placed in the error category & with error
 * priority.
 * @param b_is_critical The message is produced as with critical priority if
 * true
 * @param format Message format (stdio format)
 * @param ... Format parameters
 */
static void
xo_log_error (SDL_bool b_is_critical, const char *format, ...)
{
  va_list args;
  va_start (args, format);
  SDL_LogMessageV (SDL_LOG_CATEGORY_ERROR,
                   b_is_critical ? SDL_LOG_PRIORITY_CRITICAL
                                 : SDL_LOG_PRIORITY_ERROR,
                   format, args);

  va_end (args);
}

/// MEMORY

static struct xo_stack *
xo_stack_new (size_t size)
{
  struct xo_stack *stack
      = (struct xo_stack *)calloc (1, sizeof (struct xo_stack));
  if (stack)
    {
      stack->bits = calloc (1, size);
      stack->size = size;
      stack->offset = 0;
      return stack;
    }
  return NULL;
}

/**
 * Allocate as part of the current context_arena and zeroes the memory. Arena
 * can be switched by simple address assignment, i.e.: context_arena =
 * &temporary_arena; (see tsoding's page for more info --
 * https://github.com/tsoding/arena)
 * @param size Size of the allocation
 * @return Returns a pointer to the allocated memory chunk
 */
static void *
xo_stack_alloc (struct xo_stack *stack, size_t size)
{
  SDL_assert (stack->offset + size <= stack->size && "Stack overflow!");
  void *ptr = (char *)stack->bits + stack->offset;
  stack->offset += size;
  return ptr;
}

void
xo_stack_reset (struct xo_stack *stack)
{
  stack->offset = 0;
}

void
xo_stack_destroy (struct xo_stack *stack)
{
  free (stack->bits);
  free (stack);
}

/// UTILITIES

/**
 * Gets a pixel RGBA value from a specified surface.
 * @param surface
 * @param x Pixel coordinate on x
 * @param y Pixel coordinate on y
 * @return RGBA value of the pixel encoded in a 32 bit integer (8 bits per
 * channel)
 */
static Uint32
xo_get_pixel (SDL_Surface *surface, int x, int y)
{
  Uint8 *pixelData = (Uint8 *)surface->pixels;
  int index = y * surface->pitch + x * (int)sizeof (Uint32);
  return *((Uint32 *)(pixelData + index));
}

/**
 * Changes a pixel RGBA value in the specified surface.
 * @param surface
 * @param x Pixel coordinate on x
 * @param y Pixel coordinate on y
 * @param pixelValue
 */
static void
xo_put_pixel (SDL_Surface *surface, int x, int y, Uint32 pixelValue /* RGBA */)
{
  Uint8 *pixelData = (Uint8 *)surface->pixels;
  int index = y * surface->pitch + x * (int)sizeof (Uint32);
  *((Uint32 *)(pixelData + index)) = pixelValue;
}

/**
 * Rotates an SDL_Surface on the CPU by increments on 90-degrees only, pixel by
 * pixel. This function isn't very fast, so it should only be used at
 * initialization or on specific single-action triggers and never every frame
 * or on quick timers.
 * @param surface Base surface to rotate
 * @param increment_count Number of 90-degrees rotation to perform (positive
 * increments+ is clockwise, negative increments- is counter-clockwise)
 * @return
 */
static SDL_Surface *
xo_rotate_surface (SDL_Surface *surface, int increment_count)
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
          Uint32 pixel = xo_get_pixel (surface, x, y);
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
              xo_log_error (
                  SDL_FALSE,
                  "There was an error while rotating the surface. \n");
              break;
            }
          xo_put_pixel (newSurface, newX, newY, pixel);
        }
    }

  return newSurface;
}

static SDL_Surface *
xo_surface_mirror (SDL_Surface *surface)
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

static double
xo_sine_wave (double x, double freq, double amplitude, double phaseOffset)
{
  double y = amplitude * sin (2 * M_PI * freq * x + phaseOffset)
             - (amplitude / 2.0);
  return y;
}

static SDL_Surface *
xo_surface_flip (SDL_Surface *surface)
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

static const char *
xo_win_state_type_to_string (enum xo_win_state_type type)
{
  switch (type)
    {
    case XO_WIN_STATE_LOSE:
      return "Lose";
    case XO_WIN_STATE_TIE:
      return "Tie";
    case XO_WIN_STATE_WIN:
      return "Win";
    case XO_WIN_STATE_NONE:
      return "None";
    }
}

/// INITIALIZATION CODE

/**
 * Constructs the border part of the board surface using smaller tiles. This
 * function works, but was written long ago. The work is done by CPU blitting.
 * @param surfaces Images
 * @param board Final board surface where the border is blitted to
 * @param col
 * @param row
 * @return
 */
static int32_t
xo_make_border (SDL_Surface *surfaces[], SDL_Surface *board, int col, int row)
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
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER,
                              .y = (row * XO_TILE_SIZE) - XO_TILE_SIZE
                                   + XO_BORDER };
            SDL_Rect border_dest2
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE),
                              .y = (row * XO_TILE_SIZE) + XO_BORDER };
            SDL_Rect corner_dest = (SDL_Rect){
              .w = XO_TILE_SIZE,
              .h = XO_TILE_SIZE,
              .x = (col * XO_TILE_SIZE) - XO_TILE_SIZE + XO_BORDER,
              .y = (row * XO_TILE_SIZE) - XO_TILE_SIZE + XO_BORDER
            };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER / 2,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER / 2 };
            SDL_BlitSurface (surfaces[1], NULL, board, &center_grid_dest);
            SDL_BlitSurface (surfaces[5], NULL, board, &border_dest1);
            SDL_BlitSurface (surfaces[11], NULL, board, &border_dest2);
            SDL_BlitSurface (xo_surface_mirror (surfaces[6]), NULL, board,
                             &corner_dest);
            break;
          }
        case 1:
          {
            // tile 1,0
            SDL_Rect border_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE),
                              .y = (row * XO_TILE_SIZE) + XO_BORDER };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER / 2,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER / 2 };
            SDL_BlitSurface (surfaces[2], NULL, board, &center_grid_dest);
            SDL_BlitSurface (surfaces[11], NULL, board, &border_dest);
            break;
          }
        case 2:
          {
            // tile 2,0
            SDL_Rect border_dest1
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER };
            SDL_Rect border_dest2
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE),
                              .y = (row * XO_TILE_SIZE) + XO_BORDER };
            SDL_Rect corner_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) - XO_TILE_SIZE
                                   + XO_BORDER,
                              .y = (row * XO_TILE_SIZE) + XO_TILE_SIZE };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER / 2,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER / 2 };
            SDL_BlitSurface (xo_surface_flip (surfaces[1]), NULL, board,
                             &center_grid_dest);
            SDL_BlitSurface (surfaces[3], NULL, board, &border_dest1);
            SDL_BlitSurface (surfaces[18], NULL, board, &border_dest2);
            SDL_BlitSurface (xo_rotate_surface (surfaces[4], 2), NULL, board,
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
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER,
                              .y = (row * XO_TILE_SIZE) - XO_TILE_SIZE
                                   + XO_BORDER };

            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER / 2,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER / 2 };
            SDL_BlitSurface (xo_rotate_surface (surfaces[2], 1), NULL, board,
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
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER };

            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER / 2,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER / 2 };
            SDL_BlitSurface (xo_rotate_surface (surfaces[2], 3), NULL, board,
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
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER,
                              .y = (row * XO_TILE_SIZE) - XO_TILE_SIZE
                                   + XO_BORDER };
            SDL_Rect border_dest2
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_TILE_SIZE,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER };
            SDL_Rect corner_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_TILE_SIZE,
                              .y = (row * XO_TILE_SIZE) - XO_TILE_SIZE
                                   + XO_BORDER };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER / 2,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER / 2 };
            SDL_BlitSurface (xo_surface_mirror (surfaces[1]), NULL, board,
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
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_TILE_SIZE,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER / 2,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER / 2 };
            SDL_BlitSurface (xo_surface_mirror (surfaces[2]), NULL, board,
                             &center_grid_dest);
            SDL_BlitSurface (surfaces[11], NULL, board, &border_dest);
            break;
          }
        case 2:
          {
            // tile 2,0
            SDL_Rect border_dest1
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER };
            SDL_Rect border_dest2
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_TILE_SIZE,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER };
            SDL_Rect corner_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_TILE_SIZE,
                              .y = (row * XO_TILE_SIZE) + XO_TILE_SIZE };
            SDL_Rect center_grid_dest
                = (SDL_Rect){ .w = XO_TILE_SIZE,
                              .h = XO_TILE_SIZE,
                              .x = (col * XO_TILE_SIZE) + XO_BORDER / 2,
                              .y = (row * XO_TILE_SIZE) + XO_BORDER / 2 };
            SDL_BlitSurface (xo_surface_flip (xo_surface_mirror (surfaces[1])),
                             NULL, board, &center_grid_dest);
            SDL_BlitSurface (surfaces[5], NULL, board, &border_dest1);
            SDL_BlitSurface (surfaces[11], NULL, board, &border_dest2);
            SDL_BlitSurface (xo_surface_flip (surfaces[6]), NULL, board,
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

/**
 *
 * @param app
 * @param surfaces
 * @return
 */
static int32_t
xo_make_board (struct xo_app *app, SDL_Surface *surfaces[])
{
  xo_log_debug (1, SDL_FALSE, "Making board...\n");

  // Alloc memory for the board struct within game
  app->game->board
      = (struct xo_board *)xo_stack_alloc (generic, sizeof (struct xo_board));

  SDL_Surface *board = SDL_CreateRGBSurfaceWithFormat (
      0, (XO_TILE_SIZE * XO_BOARD_SIZE) + XO_BORDER,
      (XO_TILE_SIZE * XO_BOARD_SIZE) + XO_BORDER, 32, SDL_PIXELFORMAT_RGBA32);

  if (board == NULL)
    {
      xo_log_error (SDL_TRUE, "Error while creating board surface: %s\n",
                    SDL_GetError ());
      SDL_FreeSurface (board);
      return 1;
    }

  app->game->board->data = (struct xo_board_data *)xo_stack_alloc (
      generic, sizeof (struct xo_board_data));
  if (app->game->board->data == NULL)
    {
      return 1;
    }

  memset (app->game->board->data->squares, 0x1, sizeof (uint8_t) * 9);

  for (int col = 0; col < 3; col++)
    {
      for (int row = 0; row < 3; row++)
        {
          SDL_Rect dest
              = (SDL_Rect){ .w = XO_TILE_SIZE,
                            .h = XO_TILE_SIZE,
                            .x = (col * XO_TILE_SIZE) + XO_BORDER / 2,
                            .y = (row * XO_TILE_SIZE) + XO_BORDER / 2 };

          // Place background tile regardless
          SDL_BlitSurface (surfaces[0], NULL, board, &dest);
        }
    }
  for (int col = 0; col < 3; col++)
    {
      for (int row = 0; row < 3; row++)
        {
          xo_make_border (surfaces, board, col, row);
        }
    }

  app->game->board->image
      = SDL_CreateTextureFromSurface (app->renderer, board);

  if (app->game->board == NULL)
    {
      xo_log_error (SDL_TRUE, "Error while creating board texture: %s\n",
                    SDL_GetError ());
      SDL_FreeSurface (board);
      return 1;
    }

  return 0;
}

static int32_t
xo_make_gui (struct xo_app *app, SDL_Surface *surfaces[])
{
  SDL_Surface *logo = SDL_CreateRGBSurfaceWithFormat (
      0, (XO_TILE_SIZE * 3), (XO_TILE_SIZE * 2), 32, SDL_PIXELFORMAT_RGBA32);

  if (!logo)
    {
      xo_log_error (SDL_FALSE, "Error: could not create logo surface\n");
      return 1;
    }

  int x = 0;
  int y = 0;
  int tileIndices[] = { 7, 8, 9, 14, 15, 16 };
  for (int i = 0; i < 6; i++)
    {
      int tileIndex = tileIndices[i];
      SDL_Rect src = (SDL_Rect){ 0, 0, XO_TILE_SIZE, XO_TILE_SIZE };
      SDL_Rect dest = (SDL_Rect){ .w = XO_TILE_SIZE,
                                  .h = XO_TILE_SIZE,
                                  .x = (x * XO_TILE_SIZE),
                                  .y = (y * XO_TILE_SIZE) };
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
      xo_log_error (SDL_TRUE,
                    "Error: could not create texture from logo surface: %s\n",
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
xo_cleanup_images (SDL_Point surface_dim, SDL_Surface **surface_pieces)
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
xo_load_clips (struct xo_app *app)
{
  int32_t result = 0;
  // Directory access
  DIR *cur_dir = NULL;
  struct dirent *entry = NULL;
  char file_path[256];
  // Start by opening the music directory
  cur_dir = opendir (XO_CLIP_PATH);
  if (!cur_dir)
    {
      xo_log_error (SDL_FALSE, "Error: could not open directory\n");
      return 1;
    }

  // Loop once through the directory to count the files
  while ((entry = readdir (cur_dir)) != NULL)
    {
      sprintf (file_path, "%s/%s", XO_CLIP_PATH, entry->d_name);

      struct stat st;
      stat (file_path, &st);

      if (S_ISREG (st.st_mode) && strstr (entry->d_name, ".mp3") != NULL)
        {

          xo_log_debug (2, SDL_FALSE, "Processing %s\n", file_path);

          // Increment the music counter
          app->music_max++;
        }
    }
  closedir (cur_dir);

  // Allocate memory for the music pointers
  // based on the number of files found
  app->musics = (Mix_Music **)xo_stack_alloc (
      generic, (size_t)app->music_max * sizeof (Mix_Music *));

  // Loop through the directory again to load the music
  cur_dir = opendir (XO_CLIP_PATH);
  int m = 0; // current file index
  while ((entry = readdir (cur_dir)) != NULL)
    {
      sprintf (file_path, "%s/%s", XO_CLIP_PATH, entry->d_name);

      struct stat st;
      stat (file_path, &st);

      if (S_ISREG (st.st_mode)
          && (strstr (strlwr (entry->d_name), ".mp3") != NULL))
        {
          // Load the music file
          app->musics[m] = Mix_LoadMUS (file_path);
          if (app->musics[m] == NULL)
            {
              xo_log_error (SDL_FALSE, "Mix_LoadMUS Error: %s\n",
                            Mix_GetError ());
              return 1;
            }
          else
            {
              xo_log_debug (1, SDL_FALSE,
                            "Music titled '%s' loaded successfully!\n",
                            file_path);
              m++;
            }
        }
    }

  closedir (cur_dir);
  return result;
}

static int32_t
xo_load_fonts (struct xo_app *app)
{

  int32_t result = 0;

  // Directory access
  DIR *cur_dir = NULL;
  struct dirent *entry = NULL;
  char file_path[256];

  // Start by opening the font directory
  cur_dir = opendir (XO_FONT_PATH);

  if (!cur_dir)
    {
      xo_log_error (SDL_TRUE, "Error: could not open directory\n");
      return 1;
    }

  // Loop through the directory once
  // to count the number of fonts
  while ((entry = readdir (cur_dir)) != NULL)
    {
      sprintf (file_path, "%s/%s", XO_FONT_PATH, entry->d_name);

      struct stat st;
      stat (file_path, &st);

      if (S_ISREG (st.st_mode)
          && (strstr (strlwr (entry->d_name), ".ttf") != NULL))
        {
          xo_log_debug (1, SDL_FALSE, "Processing font: %s\n", file_path);
          app->font_max++;
        }
    }
  closedir (cur_dir);

  // Allocate memory for the fonts
  app->fonts = (TTF_Font ***)xo_stack_alloc (
      generic, (size_t)app->font_max * sizeof (TTF_Font **));

  cur_dir = opendir (XO_FONT_PATH);
  int f = 0; // current file index
  while ((entry = readdir (cur_dir)) != NULL)
    {
      sprintf (file_path, "%s/%s", XO_FONT_PATH, entry->d_name);

      struct stat st;
      stat (file_path, &st);

      if (S_ISREG (st.st_mode)
          && (strstr (strlwr (entry->d_name), ".ttf") != NULL))
        {
          xo_log_debug (1, SDL_FALSE, "Loading font: %s\n", file_path);

          // Allocate memory for the font and its size variants
          *(app->fonts + f) = (TTF_Font **)xo_stack_alloc (
              generic, (size_t)XO_FONT_SIZES * sizeof (TTF_Font *));

          int current_size = 8;
          // Load the font at different sizes
          for (int i = 0; i < XO_FONT_SIZES; i++)
            {
              // The font size is multiplied by the FONT_SIZE_FACTOR
              (*(app->fonts + f))[i] = TTF_OpenFont (file_path, current_size);

              if ((*(app->fonts + f))[i] == NULL)
                {
                  xo_log_error (SDL_TRUE, "TTF_OpenFont Error: %s\n",
                                TTF_GetError ());
                  return 1;
                }
              else
                {
                  xo_log_debug (1, SDL_FALSE,
                                "Font '%s' at size %d loaded successfully!\n",
                                entry->d_name, current_size);
                  current_size
                      = (int)((float)current_size * XO_FONT_SIZE_FACTOR);
                }
            }
          f++;
        }
    }
  closedir (cur_dir);

  return result;
}

static int32_t
xo_load_images (struct xo_app *app)
{

  xo_log_debug (1, SDL_FALSE, "Loading images...\n");

  SDL_Surface *tileset = IMG_Load (XO_GFX_PATH);

  if (tileset == NULL)
    {
      xo_log_error (SDL_TRUE, "Error while loading tileset image: %s\n",
                    IMG_GetError ());
      return 1;
    }

  // Calculate the tileset's dimensions
  int tileset_w, tileset_h;
  SDL_QueryTexture (SDL_CreateTextureFromSurface (app->renderer, tileset),
                    NULL, NULL, &tileset_w, &tileset_h);
  int cols = tileset_w / XO_TILE_SIZE;
  int rows = tileset_h / XO_TILE_SIZE;

  xo_log_debug (1, SDL_FALSE, "Tileset loaded. Dimensions: %d x %d\n",
                tileset_w, tileset_h);

  app->image_max = cols * rows;
  SDL_Surface **surface_pieces = (SDL_Surface **)xo_stack_alloc (
      generic, (size_t)app->image_max * sizeof (SDL_Surface *));
  if (surface_pieces == NULL)
    {
      xo_log_error (
          SDL_TRUE,
          "Error while allocating memory for the surface pieces: %s\n",
          SDL_GetError ());
      xo_cleanup_images ((SDL_Point){ cols, rows }, surface_pieces);
      return 1;
    }

  for (int row = 0; row < rows; row++)
    {
      for (int col = 0; col < cols; col++)
        {
          int current_id = col + (row * cols);
          xo_log_debug (1, SDL_FALSE,
                        "Blitting extracted of surface to surface piece %d!\n",
                        current_id);

          SDL_Rect extract_rect = (SDL_Rect){ .w = XO_TILE_SIZE,
                                              .h = XO_TILE_SIZE,
                                              .x = col * XO_TILE_SIZE,
                                              .y = row * XO_TILE_SIZE };
          // Allocate and initialize the surface
          surface_pieces[current_id] = SDL_CreateRGBSurfaceWithFormat (
              0, XO_TILE_SIZE, XO_TILE_SIZE, 32, SDL_PIXELFORMAT_RGBA32);
          SDL_BlitSurface (tileset, &extract_rect, surface_pieces[current_id],
                           NULL);
        }
    }

  xo_make_board (app, surface_pieces);

  xo_make_gui (app, surface_pieces);

  xo_cleanup_images ((SDL_Point){ cols, rows }, surface_pieces);
  return 0;
}

/**
 * This function places the current player's symbol in the selected square. In
 * a board_data (either the real board or a predicted future).
 * @param board_data Board affected by the function
 * @param bit Side to xo_board_bit_set_at on the board
 * @param col X coordinate for the square
 * @param row Y coordinate for the square
 */
static void
xo_board_bit_set_at (struct xo_board_data *board_data,
                     enum xo_bit_meaning_type bit, int col, int row)
{
  board_data->squares[col][row] |= bit;
}

static void
xo_board_bit_clear_at (struct xo_board_data *board_data,
                       enum xo_bit_meaning_type bit, int col, int row)
{
  board_data->squares[col][row] &= ~bit;
}

static SDL_bool
xo_board_bit_check_at (struct xo_board_data *board,
                       enum xo_bit_meaning_type bit, int col, int row)
{
  return (board->squares[col][row] & bit) != 0;
}

static void
xo_board_bit_clear (struct xo_board_data *board, int col, int row)
{
  board->squares[col][row] = XO_BIT_MEANING_EMPTY;
}

/**
 * Verifies if the board is full. When this information is relevant, it is when
 * it would indicate a tie most of the time.
 * @param board_data
 * @return
 */
static SDL_bool
xo_board_check_if_full (struct xo_board_data *board_data)
{
  SDL_bool is_full = SDL_TRUE;
  for (uint8_t col = 0; col < XO_BOARD_SIZE; col++)
    {
      for (uint8_t row = 0; row < XO_BOARD_SIZE; row++)
        {
          if (xo_board_bit_check_at (board_data, XO_BIT_MEANING_EMPTY, col,
                                     row)
              == SDL_TRUE)
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
xo_board_validate_win_conditions_for (struct xo_board_data *board_data,
                                      enum xo_bit_meaning_type side)
{
  /* Check columns */
  SDL_bool column_win;
  for (uint8_t row = 0; row < XO_BOARD_SIZE; row++)
    {
      column_win = SDL_TRUE;
      for (uint8_t col = 0; col < XO_BOARD_SIZE && column_win; col++)
        {
          if (xo_board_bit_check_at (board_data, side, col, row) == SDL_FALSE)
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
  for (uint8_t col = 0; col < XO_BOARD_SIZE; col++)
    {
      row_win = SDL_TRUE;
      for (uint8_t row = 0; row < XO_BOARD_SIZE && row_win; row++)
        {
          if (xo_board_bit_check_at (board_data, side, col, row) == SDL_FALSE)
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
  for (uint8_t dia = 0; dia < XO_BOARD_SIZE; dia++)
    {
      if (xo_board_bit_check_at (board_data, side, dia, dia) == SDL_FALSE)
        {
          diag_win = SDL_FALSE;
        }
    }

  if (diag_win)
    {
      return SDL_TRUE;
    }

  diag_win = SDL_TRUE;
  for (uint8_t dia = 0; dia < XO_BOARD_SIZE; dia++)
    {
      /* Order on column value start from the end of the board for diagonal 2.
       */
      if (xo_board_bit_check_at (board_data, side, XO_BOARD_SIZE - (dia + 1),
                                 dia)
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

static void
xo_board_draw (struct xo_app *app)
{

  for (int col = 0; col < 3; col++)
    {
      for (int row = 0; row < 3; row++)
        {

          SDL_Rect dest
              = (SDL_Rect){ .w = XO_TILE_SIZE * 4,
                            .h = XO_TILE_SIZE * 4,
                            .x = (col * XO_TILE_SIZE * 4) - XO_BORDER / 2,
                            .y = (row * XO_TILE_SIZE * 4) - XO_BORDER };

          // Draw the board square at (x, y)
          if ((app->game->board->data->squares[col][row]
               & XO_BIT_MEANING_EMPTY)
              == XO_BIT_MEANING_EMPTY)
            {
            }
          else
            {
              if ((app->game->board->data->squares[col][row]
                   & XO_BIT_MEANING_SIDE_X)
                  == XO_BIT_MEANING_SIDE_X)
                {
                  SDL_RenderCopy (app->renderer, app->game->Xs, NULL, &dest);
                }
              else if ((app->game->board->data->squares[col][row]
                        & XO_BIT_MEANING_SIDE_O)
                       == XO_BIT_MEANING_SIDE_O)
                {
                  SDL_RenderCopy (app->renderer, app->game->Os, NULL, &dest);
                }
            }
        }
    }
}

/**
 *
 * @param board_data
 * @return
 */
static enum xo_win_state_type
xo_board_test_if_final_state (struct xo_board_data *board_data)
{
  if (xo_board_validate_win_conditions_for (board_data, XO_BIT_MEANING_SIDE_X)
      == SDL_TRUE)
    {
      return XO_WIN_STATE_WIN;
    }
  if (xo_board_validate_win_conditions_for (board_data, XO_BIT_MEANING_SIDE_O)
      == SDL_TRUE)
    {
      return XO_WIN_STATE_LOSE;
    }
  if (xo_board_check_if_full (board_data) == SDL_TRUE)
    {
      return XO_WIN_STATE_TIE;
    }

  return XO_WIN_STATE_NONE;
}

static SDL_bool
xo_game_play (struct xo_app *app, enum xo_bit_meaning_type side, int col,
              int row)
{
  if (xo_board_bit_check_at (app->game->board->data, XO_BIT_MEANING_EMPTY, col,
                             row))
    {
      xo_log_debug (1, SDL_FALSE, "Playing!");
      xo_board_bit_clear_at (app->game->board->data, XO_BIT_MEANING_EMPTY, col,
                             row);
      xo_board_bit_set_at (app->game->board->data, side, col, row);
      return SDL_TRUE;
    }
  else
    {
      xo_log_debug (1, SDL_FALSE, "Tile is occupied. Click ignored...");
      return SDL_FALSE;
    }
}

/**
 * Plays the move for the player.
 * @param app
 * @param mouse_x
 * @param mouse_y
 * @return Whether the player move was successful. The move can fail if the
 * tile clicked already contains something.
 */
static SDL_Point
xo_mouse_to_square (struct xo_app *app, int mouse_x, int mouse_y)
{

  int col = -1;
  int row = -1;

  if (mouse_x < XO_WINDOW_SIZE / 3)
    {
      // Player clicked in the left third of the window
      col = 0;
    }
  else if (mouse_x < XO_WINDOW_SIZE * 2 / 3)
    {
      // Player clicked in the middle third of the window
      col = 1;
    }
  else
    {
      // Player clicked in the right third of the window
      col = 2;
    }

  if (mouse_y < XO_WINDOW_SIZE / 3)
    {
      // Player clicked in the top third of the window
      row = 0;
    }
  else if (mouse_y < XO_WINDOW_SIZE * 2 / 3)
    {
      // Player clicked in the middle third of the window
      row = 1;
    }
  else
    {
      // Player clicked in the bottom third of the window
      row = 2;
    }
  xo_log_debug (1, SDL_FALSE, "Tile clicked: %d, %d.", col, row);

  return (SDL_Point){ col, row };
}

static struct xo_cpu_response
xo_game_cpu_minimax_eval (struct xo_board_data *last_board,
                          enum xo_bit_meaning_type simulated_side)
{
  xo_log_debug (1, SDL_FALSE, "New minimax branch opened . . .");

  struct xo_cpu_response new_response;

  /* Checks if the game is over (someone won or the board is full) */
  enum xo_win_state_type board_win_state
      = xo_board_test_if_final_state (last_board);

  /* A None state would mean the game is still on-going*/
  if (board_win_state != XO_WIN_STATE_NONE)
    {
      xo_log_debug (
          1, SDL_FALSE, "CPU found a terminal move of type: %s with score %d",
          xo_win_state_type_to_string (board_win_state), board_win_state);
      new_response.score = (int32_t)board_win_state;
      new_response.has_move = SDL_FALSE;
      return new_response;
    }

  /* Set the best score depending on which side is simulated. */
  int32_t best_score
      = simulated_side == XO_BIT_MEANING_SIDE_O ? INT32_MIN : INT32_MAX;
  SDL_Point best_move;
  SDL_bool move_found = SDL_FALSE;

  /* Resets the special stack to 0 (clear the previous branching from memory)
   */
  xo_stack_reset (minimax_stack);

  /* Iterates over the board */
  for (uint8_t col = 0; col < 3; col++)
    {
      for (uint8_t row = 0; row < 3; row++)
        {
          /* When we hit an empty square. . . */
          if (xo_board_bit_check_at (last_board, XO_BIT_MEANING_EMPTY, col,
                                     row)
              == SDL_TRUE)
            {
              /* Create a copy of the board data, set the empty square as
               * marked by the simulated_side. */
              struct xo_board_data *new_board
                  = (struct xo_board_data *)xo_stack_alloc (
                      minimax_stack, sizeof (struct xo_board_data));
              memcpy (new_board->squares, last_board->squares,
                      sizeof (uint8_t) * XO_BOARD_SIZE * XO_BOARD_SIZE);

              xo_board_bit_clear_at (new_board, XO_BIT_MEANING_EMPTY, col,
                                     row);
              xo_board_bit_set_at (new_board, (uint8_t)simulated_side, col,
                                   row);

              /* Create a nested minimax evaluation using that new board as a
               * root. */
              struct xo_cpu_response simulated_response
                  = xo_game_cpu_minimax_eval (
                      new_board, simulated_side == XO_BIT_MEANING_SIDE_O
                                     ? XO_BIT_MEANING_SIDE_X
                                     : XO_BIT_MEANING_SIDE_O);

              if (simulated_side == XO_BIT_MEANING_SIDE_O)
                {
                  if (simulated_response.score > best_score)
                    {
                      best_score = simulated_response.score;
                      best_move.x = col;
                      best_move.y = row;
                      move_found = SDL_TRUE;
                    }
                }
              else if (simulated_side == XO_BIT_MEANING_SIDE_X)
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
 * Bad version of the cpu AI which just picks the first available square
 * left-right, top-down. I put this while testing the minimax when it is
 * crashing. Leaving it here in case I rework AI and need it again.
 * @param app
 */
static SDL_Point
xo_game_cpu_find_next_play_BAD (struct xo_app *app)
{
  for (int row = 0; row < 3; row++)
    {
      for (int col = 0; col < 3; col++)
        {
          if (xo_board_bit_check_at (app->game->board->data,
                                     XO_BIT_MEANING_EMPTY, col, row)
              == SDL_TRUE)
            {
              return (SDL_Point){ col, row };
            }
        }
    }

  return (SDL_Point){ -1, -1 };
}

/**
 * Plays the AI move.
 * @param current_board the last board
 * @return
 */
static SDL_Point
xo_game_cpu_find_next_play (struct xo_app *app)
{
  minimax_stack = xo_stack_new (10000);

  xo_log_debug (1, SDL_FALSE, "CPU begins looking for move. . .");

  struct xo_cpu_response response = xo_game_cpu_minimax_eval (
      app->game->board->data, XO_BIT_MEANING_SIDE_O);

  xo_log_debug (1, SDL_FALSE, "CPU brain returned move %d, %d with score %d",
                response.move.x, response.move.y, response.score);

  xo_stack_destroy (minimax_stack);

  return response.move;
}

int32_t
xo_exit (int32_t code)
{
  xo_stack_destroy (generic);
  SDL_Quit ();
  exit (code);
  return code;
}

/* This is the actual program logical sequence : */

int
main (int argc, char *argv[])
{

  generic = xo_stack_new (1000);

  /* The game begins by initializing SDL2 with various flags */
  Uint32 init_flags = SDL_INIT_EVERYTHING;
  Uint32 window_flags = SDL_WINDOW_SHOWN;
  Uint32 renderer_flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
  int image_flags = IMG_INIT_PNG;
  int mixer_flags = MIX_INIT_MP3 | MIX_INIT_OGG;

  /* Memory allocation for app */

  struct xo_app *app
      = (struct xo_app *)xo_stack_alloc (generic, sizeof (struct xo_app));

  if (app == NULL)
    {
      xo_log_error (SDL_TRUE, "Failed to allocate memory for app\n");
      return xo_exit (1);
    }

  app->game
      = (struct xo_game *)xo_stack_alloc (generic, sizeof (struct xo_game));

  if (app->game == NULL)
    {
      xo_log_error (SDL_TRUE, "Failed to allocate memory for game\n");
      return xo_exit (1);
    }
  app->game->game_state = XO_GAME_STATE_NULL;

  // SDL2 init
  if (SDL_Init (init_flags) < 0)
    {
      xo_log_error (SDL_TRUE, "SDL_Init Error: %s\n", SDL_GetError ());
      return xo_exit (1);
    }

  if (TTF_Init () < 0)
    {
      xo_log_error (SDL_TRUE, "TTF_Init Error: %s\n", TTF_GetError ());
      return xo_exit (1);
    }

  if (IMG_Init (image_flags) != image_flags)
    {
      xo_log_error (SDL_TRUE, "IMG_Init Error: %s\n", IMG_GetError ());
      return xo_exit (1);
    }

  if (Mix_Init (mixer_flags) != mixer_flags)
    {
      xo_log_error (SDL_TRUE, "Mix_Init Error: %s\n", Mix_GetError ());
      return xo_exit (1);
    }

  if (Mix_OpenAudio (44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0)
    {
      xo_log_error (SDL_TRUE, "Mix_OpenAudio Error: %s\n", Mix_GetError ());
      return xo_exit (1);
    }

  app->window
      = SDL_CreateWindow ("XO", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                          XO_WINDOW_SIZE, XO_WINDOW_SIZE, window_flags);

  if (app->window == NULL)
    {
      xo_log_error (SDL_TRUE, "SDL_CreateWindow Error: %s\n", SDL_GetError ());
      return xo_exit (1);
    }

  app->renderer = SDL_CreateRenderer (app->window, -1, renderer_flags);

  if (app->renderer == NULL)
    {
      xo_log_error (SDL_TRUE, "SDL_CreateRenderer Error: %s\n",
                    SDL_GetError ());
      return xo_exit (1);
    }

  SDL_ShowCursor (SDL_DISABLE);

  xo_load_fonts (app);
  xo_load_images (app);
  xo_load_clips (app);

  xo_log_debug (0, SDL_FALSE, "Initialization complete!\n");

  double x = 0.0;
  double freq = .1;
  double amplitude = 2.5;
  double phase_offset = 0.5;

  SDL_Rect mouse_rect = { 0, 0, XO_TILE_SIZE * 3, XO_TILE_SIZE * 3 };

  app->game->game_state = XO_GAME_STATE_MENU;

  Mix_VolumeMusic (20);
  Mix_PlayMusic (app->musics[0], -1);

  SDL_Event event;
  while ((app->game->game_state != XO_GAME_STATE_OVER) == SDL_TRUE)
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
              xo_log_debug (2, SDL_FALSE, "Mouse clicked at %d, %d.",
                            app->game->mouse.coordinates.x,
                            app->game->mouse.coordinates.y);
              if (app->game->game_state == XO_GAME_STATE_MENU)
                {
                  app->game->game_state = XO_GAME_STATE_PLAY;
                }
              else
                {
                  /* Tries playing a move for the player, if the move passes,
                   * then the AI can play. */
                  SDL_Point square = xo_mouse_to_square (app, event.button.x,
                                                         event.button.y);
                  if (xo_game_play (app, XO_BIT_MEANING_SIDE_X, square.x,
                                    square.y)
                      == SDL_TRUE)
                    {
                      SDL_Point cpu_play = xo_game_cpu_find_next_play (app);
                      xo_game_play (app, XO_BIT_MEANING_SIDE_O, cpu_play.x,
                                    cpu_play.y);
                      xo_board_test_if_final_state (app->game->board->data);
                    }
                }
            }
        }

      double y = xo_sine_wave (x, freq, amplitude, phase_offset);
      x += 0.1;

      mouse_rect.x = app->game->mouse.coordinates.x;
      mouse_rect.y = app->game->mouse.coordinates.y;

      SDL_SetRenderDrawColor (app->renderer, 0, 0, 0, 255);
      SDL_RenderClear (app->renderer);
      SDL_RenderCopy (app->renderer, app->game->board->image, NULL, NULL);
      if (app->game->game_state == XO_GAME_STATE_MENU)
        {
          SDL_RenderCopy (app->renderer, app->game->logo, NULL,
                          &(SDL_Rect){ 60, 60 + (int)y, 270, 180 });
        }
      xo_board_draw (app);
      SDL_RenderCopy (app->renderer, app->game->mouse.cursor, NULL,
                      &mouse_rect);
      SDL_RenderPresent (app->renderer);
    }

  xo_exit (0);
  return 0;
}
