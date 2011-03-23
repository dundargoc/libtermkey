#include "termkey.h"
#include "termkey-internal.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>

#include <stdio.h>

void termkey_check_version(int major, int minor)
{
  if(major != TERMKEY_VERSION_MAJOR) {
    fprintf(stderr, "libtermkey major version mismatch; %d (wants) != %d (library)\n",
        major, TERMKEY_VERSION_MAJOR);
    exit(1);
  }

  if(minor > TERMKEY_VERSION_MINOR) {
    fprintf(stderr, "libtermkey minor version mismatch; %d (wants) > %d (library)\n",
        minor, TERMKEY_VERSION_MINOR);
    exit(1);
  }

  // Happy
}

static struct TermKeyDriver *drivers[] = {
  &termkey_driver_ti,
  &termkey_driver_csi,
  NULL,
};

// Forwards for the "protected" methods
// static void eat_bytes(TermKey *tk, size_t count);
static void emit_codepoint(TermKey *tk, long codepoint, TermKeyKey *key);
static TermKeyResult peekkey_simple(TermKey *tk, TermKeyKey *key, int force, size_t *nbytes);
static TermKeyResult peekkey_mouse(TermKey *tk, TermKeyKey *key, size_t *nbytes);

static TermKeySym register_c0(TermKey *tk, TermKeySym sym, unsigned char ctrl, const char *name);
static TermKeySym register_c0_full(TermKey *tk, TermKeySym sym, int modifier_set, int modifier_mask, unsigned char ctrl, const char *name);

static struct {
  TermKeySym sym;
  const char *name;
} keynames[] = {
  { TERMKEY_SYM_NONE,      "NONE" },
  { TERMKEY_SYM_BACKSPACE, "Backspace" },
  { TERMKEY_SYM_TAB,       "Tab" },
  { TERMKEY_SYM_ENTER,     "Enter" },
  { TERMKEY_SYM_ESCAPE,    "Escape" },
  { TERMKEY_SYM_SPACE,     "Space" },
  { TERMKEY_SYM_DEL,       "DEL" },
  { TERMKEY_SYM_UP,        "Up" },
  { TERMKEY_SYM_DOWN,      "Down" },
  { TERMKEY_SYM_LEFT,      "Left" },
  { TERMKEY_SYM_RIGHT,     "Right" },
  { TERMKEY_SYM_BEGIN,     "Begin" },
  { TERMKEY_SYM_FIND,      "Find" },
  { TERMKEY_SYM_INSERT,    "Insert" },
  { TERMKEY_SYM_DELETE,    "Delete" },
  { TERMKEY_SYM_SELECT,    "Select" },
  { TERMKEY_SYM_PAGEUP,    "PageUp" },
  { TERMKEY_SYM_PAGEDOWN,  "PageDown" },
  { TERMKEY_SYM_HOME,      "Home" },
  { TERMKEY_SYM_END,       "End" },
  { TERMKEY_SYM_CANCEL,    "Cancel" },
  { TERMKEY_SYM_CLEAR,     "Clear" },
  { TERMKEY_SYM_CLOSE,     "Close" },
  { TERMKEY_SYM_COMMAND,   "Command" },
  { TERMKEY_SYM_COPY,      "Copy" },
  { TERMKEY_SYM_EXIT,      "Exit" },
  { TERMKEY_SYM_HELP,      "Help" },
  { TERMKEY_SYM_MARK,      "Mark" },
  { TERMKEY_SYM_MESSAGE,   "Message" },
  { TERMKEY_SYM_MOVE,      "Move" },
  { TERMKEY_SYM_OPEN,      "Open" },
  { TERMKEY_SYM_OPTIONS,   "Options" },
  { TERMKEY_SYM_PRINT,     "Print" },
  { TERMKEY_SYM_REDO,      "Redo" },
  { TERMKEY_SYM_REFERENCE, "Reference" },
  { TERMKEY_SYM_REFRESH,   "Refresh" },
  { TERMKEY_SYM_REPLACE,   "Replace" },
  { TERMKEY_SYM_RESTART,   "Restart" },
  { TERMKEY_SYM_RESUME,    "Resume" },
  { TERMKEY_SYM_SAVE,      "Save" },
  { TERMKEY_SYM_SUSPEND,   "Suspend" },
  { TERMKEY_SYM_UNDO,      "Undo" },
  { TERMKEY_SYM_KP0,       "KP0" },
  { TERMKEY_SYM_KP1,       "KP1" },
  { TERMKEY_SYM_KP2,       "KP2" },
  { TERMKEY_SYM_KP3,       "KP3" },
  { TERMKEY_SYM_KP4,       "KP4" },
  { TERMKEY_SYM_KP5,       "KP5" },
  { TERMKEY_SYM_KP6,       "KP6" },
  { TERMKEY_SYM_KP7,       "KP7" },
  { TERMKEY_SYM_KP8,       "KP8" },
  { TERMKEY_SYM_KP9,       "KP9" },
  { TERMKEY_SYM_KPENTER,   "KPEnter" },
  { TERMKEY_SYM_KPPLUS,    "KPPlus" },
  { TERMKEY_SYM_KPMINUS,   "KPMinus" },
  { TERMKEY_SYM_KPMULT,    "KPMult" },
  { TERMKEY_SYM_KPDIV,     "KPDiv" },
  { TERMKEY_SYM_KPCOMMA,   "KPComma" },
  { TERMKEY_SYM_KPPERIOD,  "KPPeriod" },
  { TERMKEY_SYM_KPEQUALS,  "KPEquals" },
  { 0, NULL },
};

#define CHARAT(i) (tk->buffer[tk->buffstart + (i)])

#ifdef DEBUG
/* Some internal deubgging functions */

static void print_buffer(TermKey *tk)
{
  int i;
  for(i = 0; i < tk->buffcount && i < 20; i++)
    fprintf(stderr, "%02x ", CHARAT(i));
  if(tk->buffcount > 20)
    fprintf(stderr, "...");
}

static void print_key(TermKey *tk, TermKeyKey *key)
{
  switch(key->type) {
  case TERMKEY_TYPE_UNICODE:
    fprintf(stderr, "Unicode codepoint=U+%04lx utf8='%s'", key->code.codepoint, key->utf8);
    break;
  case TERMKEY_TYPE_FUNCTION:
    fprintf(stderr, "Function F%d", key->code.number);
    break;
  case TERMKEY_TYPE_KEYSYM:
    fprintf(stderr, "Keysym sym=%d(%s)", key->code.sym, termkey_get_keyname(tk, key->code.sym));
    break;
  case TERMKEY_TYPE_MOUSE:
    {
      TermKeyMouseEvent ev;
      int button, line, col;
      termkey_interpret_mouse(tk, key, &ev, &button, &line, &col);
      fprintf(stderr, "Mouse ev=%d button=%d pos=(%d,%d)\n", ev, button, line, col);
    }
    break;
  }

  int m = key->modifiers;
  fprintf(stderr, " mod=%s%s%s+%02x",
      (m & TERMKEY_KEYMOD_CTRL  ? "C" : ""),
      (m & TERMKEY_KEYMOD_ALT   ? "A" : ""),
      (m & TERMKEY_KEYMOD_SHIFT ? "S" : ""),
      m & ~(TERMKEY_KEYMOD_CTRL|TERMKEY_KEYMOD_ALT|TERMKEY_KEYMOD_SHIFT));
}

static const char *res2str(TermKeyResult res)
{
  switch(res) {
  case TERMKEY_RES_KEY:
    return "TERMKEY_RES_KEY";
  case TERMKEY_RES_EOF:
    return "TERMKEY_RES_EOF";
  case TERMKEY_RES_AGAIN:
    return "TERMKEY_RES_AGAIN";
  case TERMKEY_RES_NONE:
    return "TERMKEY_RES_NONE";
  }

  return "unknown";
}
#endif

/* We might expose this as public API one day, when the ideas are finalised.
 * As yet it isn't public, so keep it static
 */
static TermKey *termkey_new_full(int fd, int flags, size_t buffsize, int waittime)
{
  TermKey *tk = malloc(sizeof(*tk));
  if(!tk)
    return NULL;

  if(!(flags & (TERMKEY_FLAG_RAW|TERMKEY_FLAG_UTF8))) {
    int locale_is_utf8 = 0;
    char *e;

    if((e = getenv("LANG")) && strstr(e, "UTF-8"))
      locale_is_utf8 = 1;

    if(!locale_is_utf8 && (e = getenv("LC_MESSAGES")) && strstr(e, "UTF-8"))
      locale_is_utf8 = 1;

    if(!locale_is_utf8 && (e = getenv("LC_ALL")) && strstr(e, "UTF-8"))
      locale_is_utf8 = 1;

    if(locale_is_utf8)
      flags |= TERMKEY_FLAG_UTF8;
    else
      flags |= TERMKEY_FLAG_RAW;
  }

  tk->fd    = fd;
  tk->flags = flags;

  tk->buffer = malloc(buffsize);
  if(!tk->buffer)
    goto abort_free_tk;

  tk->buffstart = 0;
  tk->buffcount = 0;
  tk->buffsize  = buffsize;

  tk->restore_termios_valid = 0;

  tk->waittime = waittime;

  tk->is_closed = 0;

  tk->nkeynames = 64;
  tk->keynames = malloc(sizeof(tk->keynames[0]) * tk->nkeynames);
  if(!tk->keynames)
    goto abort_free_buffer;

  int i;
  for(i = 0; i < tk->nkeynames; i++)
    tk->keynames[i] = NULL;

  for(i = 0; i < 32; i++)
    tk->c0[i].sym = TERMKEY_SYM_NONE;

  tk->method.emit_codepoint = &emit_codepoint;
  tk->method.peekkey_simple = &peekkey_simple;
  tk->method.peekkey_mouse  = &peekkey_mouse;

  for(i = 0; keynames[i].name; i++)
    termkey_register_keyname(tk, keynames[i].sym, keynames[i].name);

  register_c0(tk, TERMKEY_SYM_BACKSPACE, 0x08, NULL);
  register_c0(tk, TERMKEY_SYM_TAB,       0x09, NULL);
  register_c0(tk, TERMKEY_SYM_ENTER,     0x0d, NULL);
  register_c0(tk, TERMKEY_SYM_ESCAPE,    0x1b, NULL);

  const char *term = getenv("TERM");

  struct TermKeyDriverNode *tail = NULL;

  for(i = 0; drivers[i]; i++) {
    void *info = (*drivers[i]->new_driver)(tk, term);
    if(!info)
      continue;

#ifdef DEBUG
    fprintf(stderr, "Loading the %s driver\n", drivers[i]->name);
#endif

    struct TermKeyDriverNode *thisdrv = malloc(sizeof(*thisdrv));
    if(!thisdrv)
      goto abort_free_drivers;

    thisdrv->driver = drivers[i];
    thisdrv->info = info;
    thisdrv->next = NULL;

    if(!tail)
      tk->drivers = thisdrv;
    else
      tail->next = thisdrv;

    tail = thisdrv;
  }

  if(!tk->drivers) {
    fprintf(stderr, "Unable to find a terminal driver\n");
    goto abort_free_keynames;
  }

  if(!(flags & TERMKEY_FLAG_NOTERMIOS)) {
    struct termios termios;
    if(tcgetattr(fd, &termios) == 0) {
      tk->restore_termios = termios;
      tk->restore_termios_valid = 1;

      termios.c_iflag &= ~(IXON|INLCR|ICRNL);
      termios.c_lflag &= ~(ICANON|ECHO);

      termios.c_cc[VQUIT] = 0;
      termios.c_cc[VSUSP] = 0;
      if(flags & TERMKEY_FLAG_CTRLC)
        termios.c_cc[VINTR] = 0;

      tcsetattr(fd, TCSANOW, &termios);
    }
  }

  struct TermKeyDriverNode *p;
  for(p = tk->drivers; p; p = p->next)
    if(p->driver->start_driver)
      (*p->driver->start_driver)(tk, p->info);

  return tk;

abort_free_drivers:
  for(p = tk->drivers; p; ) {
    (*p->driver->free_driver)(p->info);
    struct TermKeyDriverNode *next = p->next;
    free(p);
    p = next;
  }

abort_free_keynames:
  free(tk->keynames);

abort_free_buffer:
  free(tk->buffer);

abort_free_tk:
  free(tk);

  return NULL;
}

TermKey *termkey_new(int fd, int flags)
{
  return termkey_new_full(fd, flags, 256, 50);
}

void termkey_free(TermKey *tk)
{
  free(tk->buffer); tk->buffer = NULL;
  free(tk->keynames); tk->keynames = NULL;

  struct TermKeyDriverNode *p;
  for(p = tk->drivers; p; ) {
    (*p->driver->free_driver)(p->info);
    struct TermKeyDriverNode *next = p->next;
    free(p);
    p = next;
  }

  free(tk);
}

void termkey_destroy(TermKey *tk)
{
  struct TermKeyDriverNode *p;
  for(p = tk->drivers; p; p = p->next)
    if(p->driver->stop_driver)
      (*p->driver->stop_driver)(tk, p->info);

  if(tk->restore_termios_valid)
    tcsetattr(tk->fd, TCSANOW, &tk->restore_termios);

  termkey_free(tk);
}

int termkey_get_fd(TermKey *tk)
{
  return tk->fd;
}

int termkey_get_flags(TermKey *tk)
{
  return tk->flags;
}

void termkey_set_flags(TermKey *tk, int newflags)
{
  tk->flags = newflags;
}

void termkey_set_waittime(TermKey *tk, int msec)
{
  tk->waittime = msec;
}

int termkey_get_waittime(TermKey *tk)
{
  return tk->waittime;
}

static void eat_bytes(TermKey *tk, size_t count)
{
  if(count >= tk->buffcount) {
    tk->buffstart = 0;
    tk->buffcount = 0;
    return;
  }

  tk->buffstart += count;
  tk->buffcount -= count;
}

static inline unsigned int utf8_seqlen(long codepoint)
{
  if(codepoint < 0x0000080) return 1;
  if(codepoint < 0x0000800) return 2;
  if(codepoint < 0x0010000) return 3;
  if(codepoint < 0x0200000) return 4;
  if(codepoint < 0x4000000) return 5;
  return 6;
}

static void fill_utf8(TermKeyKey *key)
{
  long codepoint = key->code.codepoint;
  int nbytes = utf8_seqlen(codepoint);

  key->utf8[nbytes] = 0;

  // This is easier done backwards
  int b = nbytes;
  while(b > 1) {
    b--;
    key->utf8[b] = 0x80 | (codepoint & 0x3f);
    codepoint >>= 6;
  }

  switch(nbytes) {
    case 1: key->utf8[0] =        (codepoint & 0x7f); break;
    case 2: key->utf8[0] = 0xc0 | (codepoint & 0x1f); break;
    case 3: key->utf8[0] = 0xe0 | (codepoint & 0x0f); break;
    case 4: key->utf8[0] = 0xf0 | (codepoint & 0x07); break;
    case 5: key->utf8[0] = 0xf8 | (codepoint & 0x03); break;
    case 6: key->utf8[0] = 0xfc | (codepoint & 0x01); break;
  }
}

static void emit_codepoint(TermKey *tk, long codepoint, TermKeyKey *key)
{
  if(codepoint < 0x20) {
    // C0 range
    key->code.codepoint = 0;
    key->modifiers = 0;

    if(!(tk->flags & TERMKEY_FLAG_NOINTERPRET) && tk->c0[codepoint].sym != TERMKEY_SYM_UNKNOWN) {
      key->code.sym = tk->c0[codepoint].sym;
      key->modifiers |= tk->c0[codepoint].modifier_set;
    }

    if(!key->code.sym) {
      key->type = TERMKEY_TYPE_UNICODE;
      /* Generically modified Unicode ought not report the SHIFT state, or else
       * we get into complicationg trying to report Shift-; vs : and so on...
       * In order to be able to represent Ctrl-Shift-A as CTRL modified 
       * unicode A, we need to call Ctrl-A simply 'a', lowercase
       */
      if(codepoint+0x40 >= 'A' && codepoint+0x40 <= 'Z')
        // it's a letter - use lowecase instead
        key->code.codepoint = codepoint + 0x60;
      else
        key->code.codepoint = codepoint + 0x40;
      key->modifiers = TERMKEY_KEYMOD_CTRL;
    }
    else {
      key->type = TERMKEY_TYPE_KEYSYM;
    }
  }
  else if(codepoint == 0x20 && (tk->flags & TERMKEY_FLAG_SPACESYMBOL)) {
    // ASCII space
    key->type = TERMKEY_TYPE_KEYSYM;
    key->code.sym = TERMKEY_SYM_SPACE;
    key->modifiers = 0;
  }
  else if(codepoint == 0x7f && !(tk->flags & TERMKEY_FLAG_NOINTERPRET)) {
    // ASCII DEL
    key->type = TERMKEY_TYPE_KEYSYM;
    key->code.sym = TERMKEY_SYM_DEL;
    key->modifiers = 0;
  }
  else if(codepoint >= 0x20 && codepoint < 0x80) {
    // ASCII lowbyte range
    key->type = TERMKEY_TYPE_UNICODE;
    key->code.codepoint = codepoint;
    key->modifiers = 0;
  }
  else if(codepoint >= 0x80 && codepoint < 0xa0) {
    // UTF-8 never starts with a C1 byte. So we can be sure of these
    key->type = TERMKEY_TYPE_UNICODE;
    key->code.codepoint = codepoint - 0x40;
    key->modifiers = TERMKEY_KEYMOD_CTRL|TERMKEY_KEYMOD_ALT;
  }
  else {
    // UTF-8 codepoint
    key->type = TERMKEY_TYPE_UNICODE;
    key->code.codepoint = codepoint;
    key->modifiers = 0;
  }

  if(key->type == TERMKEY_TYPE_UNICODE)
    fill_utf8(key);
}

#define UTF8_INVALID 0xFFFD

static TermKeyResult peekkey(TermKey *tk, TermKeyKey *key, int force, size_t *nbytep)
{
  int again = 0;

#ifdef DEBUG
  fprintf(stderr, "getkey(force=%d): buffer ", force);
  print_buffer(tk);
  fprintf(stderr, "\n");
#endif

  TermKeyResult ret;
  struct TermKeyDriverNode *p;
  for(p = tk->drivers; p; p = p->next) {
    ret = (p->driver->peekkey)(tk, p->info, key, force, nbytep);

#ifdef DEBUG
    fprintf(stderr, "Driver %s yields %s\n", p->driver->name, res2str(ret));
#endif

    switch(ret) {
    case TERMKEY_RES_KEY:
#ifdef DEBUG
      print_key(tk, key); fprintf(stderr, "\n");
#endif
      // Slide the data down to stop it running away
      {
        size_t halfsize = tk->buffsize / 2;

        if(tk->buffstart > halfsize) {
          memcpy(tk->buffer, tk->buffer + halfsize, halfsize);
          tk->buffstart -= halfsize;
        }
      }

      /* fallthrough */
    case TERMKEY_RES_EOF:
      return ret;

    case TERMKEY_RES_AGAIN:
      if(!force)
        again = 1;

      /* fallthrough */
    case TERMKEY_RES_NONE:
      break;
    }
  }

  if(again)
    return TERMKEY_RES_AGAIN;

  ret = peekkey_simple(tk, key, force, nbytep);

#ifdef DEBUG
  fprintf(stderr, "getkey_simple(force=%d) yields %s\n", force, res2str(ret));
  if(ret == TERMKEY_RES_KEY) {
    print_key(tk, key); fprintf(stderr, "\n");
  }
#endif

  return ret;
}

static TermKeyResult peekkey_simple(TermKey *tk, TermKeyKey *key, int force, size_t *nbytep)
{
  if(tk->buffcount == 0)
    return tk->is_closed ? TERMKEY_RES_EOF : TERMKEY_RES_NONE;

  unsigned char b0 = CHARAT(0);

  if(b0 == 0x1b) {
    // Escape-prefixed value? Might therefore be Alt+key
    if(tk->buffcount == 1) {
      // This might be an <Esc> press, or it may want to be part of a longer
      // sequence
      if(!force)
        return TERMKEY_RES_AGAIN;

      (*tk->method.emit_codepoint)(tk, b0, key);
      *nbytep = 1;
      return TERMKEY_RES_KEY;
    }

    // Try another key there
    tk->buffstart++;
    tk->buffcount--;

    // Run the full driver
    TermKeyResult metakey_result = peekkey(tk, key, force, nbytep);

    tk->buffstart--;
    tk->buffcount++;

    switch(metakey_result) {
      case TERMKEY_RES_KEY:
        key->modifiers |= TERMKEY_KEYMOD_ALT;
        (*nbytep)++;
        break;

      case TERMKEY_RES_NONE:
      case TERMKEY_RES_EOF:
      case TERMKEY_RES_AGAIN:
        break;
    }

    return metakey_result;
  }
  else if(b0 < 0xa0) {
    // Single byte C0, G0 or C1 - C1 is never UTF-8 initial byte
    (*tk->method.emit_codepoint)(tk, b0, key);
    *nbytep = 1;
    return TERMKEY_RES_KEY;
  }
  else if(tk->flags & TERMKEY_FLAG_UTF8) {
    // Some UTF-8
    unsigned int nbytes;
    long codepoint;

    key->type = TERMKEY_TYPE_UNICODE;
    key->modifiers = 0;

    if(b0 < 0xc0) {
      // Starts with a continuation byte - that's not right
      (*tk->method.emit_codepoint)(tk, UTF8_INVALID, key);
      *nbytep = 1;
      return TERMKEY_RES_KEY;
    }
    else if(b0 < 0xe0) {
      nbytes = 2;
      codepoint = b0 & 0x1f;
    }
    else if(b0 < 0xf0) {
      nbytes = 3;
      codepoint = b0 & 0x0f;
    }
    else if(b0 < 0xf8) {
      nbytes = 4;
      codepoint = b0 & 0x07;
    }
    else if(b0 < 0xfc) {
      nbytes = 5;
      codepoint = b0 & 0x03;
    }
    else if(b0 < 0xfe) {
      nbytes = 6;
      codepoint = b0 & 0x01;
    }
    else {
      (*tk->method.emit_codepoint)(tk, UTF8_INVALID, key);
      *nbytep = 1;
      return TERMKEY_RES_KEY;
    }

    if(tk->buffcount < nbytes) {
      if(!force)
        return TERMKEY_RES_AGAIN;

      /* There weren't enough bytes for a complete UTF-8 sequence but caller
       * demands an answer. About the best thing we can do here is eat as many
       * bytes as we have, and emit a UTF8_INVALID. If the remaining bytes
       * arrive later, they'll be invalid too.
       */
      (*tk->method.emit_codepoint)(tk, UTF8_INVALID, key);
      *nbytep = tk->buffcount;
      return TERMKEY_RES_KEY;
    }

    for(unsigned int b = 1; b < nbytes; b++) {
      unsigned char cb = CHARAT(b);
      if(cb < 0x80 || cb >= 0xc0) {
        (*tk->method.emit_codepoint)(tk, UTF8_INVALID, key);
        *nbytep = b - 1;
        return TERMKEY_RES_KEY;
      }

      codepoint <<= 6;
      codepoint |= cb & 0x3f;
    }

    // Check for overlong sequences
    if(nbytes > utf8_seqlen(codepoint))
      codepoint = UTF8_INVALID;

    // Check for UTF-16 surrogates or invalid codepoints
    if((codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
       codepoint == 0xFFFE ||
       codepoint == 0xFFFF)
      codepoint = UTF8_INVALID;

    (*tk->method.emit_codepoint)(tk, codepoint, key);
    *nbytep = nbytes;
    return TERMKEY_RES_KEY;
  }
  else {
    // Non UTF-8 case - just report the raw byte
    key->type = TERMKEY_TYPE_UNICODE;
    key->code.codepoint = b0;
    key->modifiers = 0;

    key->utf8[0] = key->code.codepoint;
    key->utf8[1] = 0;

    *nbytep = 1;

    return TERMKEY_RES_KEY;
  }
}

static TermKeyResult peekkey_mouse(TermKey *tk, TermKeyKey *key, size_t *nbytep)
{
  if(tk->buffcount < 3)
    return TERMKEY_RES_AGAIN;

  key->type = TERMKEY_TYPE_MOUSE;
  key->code.mouse[0] = CHARAT(0) - 0x20;
  key->code.mouse[1] = CHARAT(1) - 0x20;
  key->code.mouse[2] = CHARAT(2) - 0x20;
  key->modifiers     = 0;
  *nbytep = 3;
  return TERMKEY_RES_KEY;
}

TermKeyResult termkey_interpret_mouse(TermKey *tk, TermKeyKey *key, TermKeyMouseEvent *event, int *button, int *line, int *col)
{
  if(key->type != TERMKEY_TYPE_MOUSE)
    return TERMKEY_RES_NONE;

  if(button)
    *button = 0;

  if(col)
    *col  = key->code.mouse[1];

  if(line)
    *line = key->code.mouse[2];

  if(!event)
    return TERMKEY_RES_KEY;

  int btn = 0;

  int code = key->code.mouse[0];

  int drag = code & 0x20;

  switch(code & ~0x20) {
  case 0:
  case 1:
  case 2:
    *event = drag ? TERMKEY_MOUSE_DRAG : TERMKEY_MOUSE_PRESS;
    btn = (code & ~0x20) + 1;
    break;

  case 3:
    *event = TERMKEY_MOUSE_RELEASE;
    // no button hint
    break;

  case 64:
  case 65:
    *event = drag ? TERMKEY_MOUSE_DRAG : TERMKEY_MOUSE_PRESS;
    btn = (code & ~0x20) + 4 - 64;
    break;

  default:
    *event = TERMKEY_MOUSE_UNKNOWN;
  }

  if(button)
    *button = btn;

  return TERMKEY_RES_KEY;
}

TermKeyResult termkey_getkey(TermKey *tk, TermKeyKey *key)
{
  size_t nbytes = 0;
  TermKeyResult ret = peekkey(tk, key, 0, &nbytes);

  if(ret == TERMKEY_RES_KEY)
    eat_bytes(tk, nbytes);

  if(ret == TERMKEY_RES_AGAIN)
    /* Call peekkey() again in force mode to obtain whatever it can */
    (void)peekkey(tk, key, 1, &nbytes);
    /* Don't eat it yet though */

  return ret;
}

TermKeyResult termkey_getkey_force(TermKey *tk, TermKeyKey *key)
{
  size_t nbytes = 0;
  TermKeyResult ret = peekkey(tk, key, 1, &nbytes);

  if(ret == TERMKEY_RES_KEY)
    eat_bytes(tk, nbytes);

  return ret;
}

TermKeyResult termkey_waitkey(TermKey *tk, TermKeyKey *key)
{
  while(1) {
    TermKeyResult ret = termkey_getkey(tk, key);

    switch(ret) {
      case TERMKEY_RES_KEY:
      case TERMKEY_RES_EOF:
        return ret;

      case TERMKEY_RES_NONE:
        termkey_advisereadable(tk);
        break;

      case TERMKEY_RES_AGAIN:
        {
          if(tk->is_closed)
            // We're closed now. Never going to get more bytes so just go with
            // what we have
            return termkey_getkey_force(tk, key);

          struct pollfd fd;

          fd.fd = tk->fd;
          fd.events = POLLIN;

          poll(&fd, 1, tk->waittime);

          if(fd.revents & (POLLIN|POLLHUP|POLLERR))
            ret = termkey_advisereadable(tk);
          else
            ret = TERMKEY_RES_NONE;

          if(ret == TERMKEY_RES_NONE)
            return termkey_getkey_force(tk, key);
        }
        break;
    }
  }

  /* UNREACHABLE */
}

void termkey_pushinput(TermKey *tk, unsigned char *input, size_t inputlen)
{
  if(tk->buffstart + tk->buffcount + inputlen > tk->buffsize) {
    while(tk->buffstart + tk->buffcount + inputlen > tk->buffsize)
      tk->buffsize *= 2;

    unsigned char *newbuffer = realloc(tk->buffer, tk->buffsize);
    // TODO: Handle realloc() failure
    tk->buffer = newbuffer;
  }

  // Not strcpy just in case of NUL bytes
  memcpy(tk->buffer + tk->buffstart + tk->buffcount, input, inputlen);
  tk->buffcount += inputlen;
}

TermKeyResult termkey_advisereadable(TermKey *tk)
{
  unsigned char buffer[64]; // Smaller than the default size
  ssize_t len = read(tk->fd, buffer, sizeof buffer);

  if(len == -1 && errno == EAGAIN)
    return TERMKEY_RES_NONE;
  else if(len < 1) {
    tk->is_closed = 1;
    return TERMKEY_RES_NONE;
  }
  else {
    termkey_pushinput(tk, buffer, len);
    return TERMKEY_RES_AGAIN;
  }
}

TermKeySym termkey_register_keyname(TermKey *tk, TermKeySym sym, const char *name)
{
  if(!sym)
    sym = tk->nkeynames;

  if(sym >= tk->nkeynames) {
    const char **new_keynames = realloc(tk->keynames, sizeof(new_keynames[0]) * (sym + 1));
    // TODO: Handle realloc() failure
    tk->keynames = new_keynames;

    // Fill in the hole
    for(int i = tk->nkeynames; i < sym; i++)
      tk->keynames[i] = NULL;

    tk->nkeynames = sym + 1;
  }

  tk->keynames[sym] = name;

  return sym;
}

const char *termkey_get_keyname(TermKey *tk, TermKeySym sym)
{
  if(sym == TERMKEY_SYM_UNKNOWN)
    return "UNKNOWN";

  if(sym < tk->nkeynames)
    return tk->keynames[sym];

  return "UNKNOWN";
}

TermKeySym termkey_keyname2sym(TermKey *tk, const char *keyname)
{
  /* We store an array, so we can't do better than a linear search. Doesn't
   * matter because user won't be calling this too often */

  TermKeySym sym;

  for(sym = 0; sym < tk->nkeynames; sym++)
    if(tk->keynames[sym] && strcmp(keyname, tk->keynames[sym]) == 0)
      return sym;

  return TERMKEY_SYM_UNKNOWN;
}

static TermKeySym register_c0(TermKey *tk, TermKeySym sym, unsigned char ctrl, const char *name)
{
  return register_c0_full(tk, sym, 0, 0, ctrl, name);
}

static TermKeySym register_c0_full(TermKey *tk, TermKeySym sym, int modifier_set, int modifier_mask, unsigned char ctrl, const char *name)
{
  if(ctrl >= 0x20) {
    fprintf(stderr, "Cannot register C0 key at ctrl 0x%02x - out of bounds\n", ctrl);
    return -1;
  }

  if(name)
    sym = termkey_register_keyname(tk, sym, name);

  tk->c0[ctrl].sym = sym;
  tk->c0[ctrl].modifier_set = modifier_set;
  tk->c0[ctrl].modifier_mask = modifier_mask;

  return sym;
}

size_t termkey_snprint_key(TermKey *tk, char *buffer, size_t len, TermKeyKey *key, TermKeyFormat format)
{
  size_t pos = 0;
  size_t l = 0;

  int longmod = format & TERMKEY_FORMAT_LONGMOD;

  int wrapbracket = (format & TERMKEY_FORMAT_WRAPBRACKET) &&
                    (key->type != TERMKEY_TYPE_UNICODE || key->modifiers != 0);

  if(format & TERMKEY_FORMAT_CARETCTRL && 
     key->type == TERMKEY_TYPE_UNICODE &&
     key->modifiers == TERMKEY_KEYMOD_CTRL) {
    long codepoint = key->code.codepoint;

    // Handle some of the special casesfirst
    if(codepoint >= 'a' && codepoint <= 'z') {
      l = snprintf(buffer + pos, len - pos, wrapbracket ? "<^%c>" : "^%c", (char)codepoint - 0x20);
      if(l <= 0) return pos;
      pos += l;
      return pos;
    }
    else if((codepoint >= '@' && codepoint < 'A') ||
            (codepoint > 'Z' && codepoint <= '_')) {
      l = snprintf(buffer + pos, len - pos, wrapbracket ? "<^%c>" : "^%c", (char)codepoint);
      if(l <= 0) return pos;
      pos += l;
      return pos;
    }
  }

  if(wrapbracket) {
    l = snprintf(buffer + pos, len - pos, "<");
    if(l <= 0) return pos;
    pos += l;
  }

  if(key->modifiers & TERMKEY_KEYMOD_ALT) {
    int altismeta = format & TERMKEY_FORMAT_ALTISMETA;

    l = snprintf(buffer + pos, len - pos, longmod ? ( altismeta ? "Meta-" : "Alt-" )
                                                  : ( altismeta ? "M-"    : "A-" ));
    if(l <= 0) return pos;
    pos += l;
  }

  if(key->modifiers & TERMKEY_KEYMOD_CTRL) {
    l = snprintf(buffer + pos, len - pos, longmod ? "Ctrl-" : "C-");
    if(l <= 0) return pos;
    pos += l;
  }

  if(key->modifiers & TERMKEY_KEYMOD_SHIFT) {
    l = snprintf(buffer + pos, len - pos, longmod ? "Shift-" : "S-");
    if(l <= 0) return pos;
    pos += l;
  }

  switch(key->type) {
  case TERMKEY_TYPE_UNICODE:
    l = snprintf(buffer + pos, len - pos, "%s", key->utf8);
    break;
  case TERMKEY_TYPE_KEYSYM:
    l = snprintf(buffer + pos, len - pos, "%s", termkey_get_keyname(tk, key->code.sym));
    break;
  case TERMKEY_TYPE_FUNCTION:
    l = snprintf(buffer + pos, len - pos, "F%d", key->code.number);
    break;
  case TERMKEY_TYPE_MOUSE:
    {
      TermKeyMouseEvent ev;
      int button;
      int line, col;
      termkey_interpret_mouse(tk, key, &ev, &button, &line, &col);

      static char *evnames[] = { "Unknown", "Press", "Drag", "Release" };

      l = snprintf(buffer + pos, len - pos, "Mouse%s(%d)",
          evnames[ev], button);

      if(format & TERMKEY_FORMAT_MOUSE_POS) {
        if(l <= 0) return pos;
        pos += l;

        l = snprintf(buffer + pos, len - pos, " @ (%d,%d)", col, line);
      }
    }
    break;
  }

  if(l <= 0) return pos;
  pos += l;

  if(wrapbracket) {
    l = snprintf(buffer + pos, len - pos, ">");
    if(l <= 0) return pos;
    pos += l;
  }

  return pos;
}
