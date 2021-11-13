/* pcresearch.c - searching subroutines using PCRE for grep.
   Copyright 2000, 2007, 2009-2021 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston, MA
   02110-1301, USA.  */

/* Written August 1992 by Mike Haertel. */
/* Updated for PCRE2 by Carlo Arenas. */

#include <config.h>
#include "search.h"
#include "die.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

/* Needed for backward compatibility for PCRE2 < 10.30  */
#ifndef PCRE2_CONFIG_DEPTHLIMIT
#define PCRE2_CONFIG_DEPTHLIMIT PCRE2_CONFIG_RECURSIONLIMIT
#define PCRE2_ERROR_DEPTHLIMIT  PCRE2_ERROR_RECURSIONLIMIT
#define pcre2_set_depth_limit   pcre2_set_recursion_limit
#endif

struct pcre_comp
{
  /* The JIT stack and its maximum size.  */
  pcre2_jit_stack *jit_stack;
  PCRE2_SIZE jit_stack_size;

  /* Compiled internal form of a Perl regular expression.  */
  pcre2_code *cre;
  pcre2_match_context *mcontext;
  pcre2_match_data *data;
  /* Table, indexed by ! (flag & PCRE_NOTBOL), of whether the empty
     string matches when that flag is used.  */
  int empty_match[2];
};


/* Match the already-compiled PCRE pattern against the data in SUBJECT,
   of size SEARCH_BYTES and starting with offset SEARCH_OFFSET, with
   options OPTIONS.
   Return the (nonnegative) match count or a (negative) error number.  */
static int
jit_exec (struct pcre_comp *pc, char const *subject, PCRE2_SIZE search_bytes,
          PCRE2_SIZE search_offset, int options)
{
  while (true)
    {
      int e = pcre2_match (pc->cre, (PCRE2_SPTR)subject, search_bytes,
                           search_offset, options, pc->data, pc->mcontext);
      if (e == PCRE2_ERROR_JIT_STACKLIMIT
          && 0 < pc->jit_stack_size && pc->jit_stack_size <= INT_MAX / 2)
        {
          PCRE2_SIZE old_size = pc->jit_stack_size;
          PCRE2_SIZE new_size = pc->jit_stack_size = old_size * 2;

          if (pc->jit_stack)
            pcre2_jit_stack_free (pc->jit_stack);
          pc->jit_stack = pcre2_jit_stack_create (old_size, new_size, NULL);

          if (!pc->mcontext)
            pc->mcontext = pcre2_match_context_create (NULL);

          if (!pc->jit_stack || !pc->mcontext)
            die (EXIT_TROUBLE, 0,
                 _("failed to allocate memory for the PCRE JIT stack"));
          pcre2_jit_stack_assign (pc->mcontext, NULL, pc->jit_stack);
          continue;
        }
      if (e == PCRE2_ERROR_DEPTHLIMIT)
        {
          uint32_t lim;
          pcre2_config (PCRE2_CONFIG_DEPTHLIMIT, &lim);
          if (lim >= UINT32_MAX / 2)
            return e;

          lim <<= 1;
          if (!pc->mcontext)
            pc->mcontext = pcre2_match_context_create (NULL);

          pcre2_set_depth_limit (pc->mcontext, lim);
          continue;
        }
      return e;
    }
}

/* Compile the -P style PATTERN, containing SIZE bytes that are
   followed by '\n'.  Return a description of the compiled pattern.  */

void *
Pcompile (char *pattern, idx_t size, reg_syntax_t ignored, bool exact)
{
  PCRE2_SIZE e;
  int ec;
  PCRE2_UCHAR8 ep[128]; /* 120 code units is suggested to avoid truncation  */
  static char const wprefix[] = "(?<!\\w)(?:";
  static char const wsuffix[] = ")(?!\\w)";
  static char const xprefix[] = "^(?:";
  static char const xsuffix[] = ")$";
  int fix_len_max = MAX (sizeof wprefix - 1 + sizeof wsuffix - 1,
                         sizeof xprefix - 1 + sizeof xsuffix - 1);
  unsigned char *re = xmalloc (size + fix_len_max + 1);
  int flags = PCRE2_DOLLAR_ENDONLY | (match_icase ? PCRE2_CASELESS : 0);
  char *patlim = pattern + size;
  char *n = (char *)re;
  struct pcre_comp *pc = xcalloc (1, sizeof (*pc));
  pcre2_compile_context *ccontext = pcre2_compile_context_create(NULL);

  if (localeinfo.multibyte)
    {
      if (! localeinfo.using_utf8)
        die (EXIT_TROUBLE, 0, _("-P supports only unibyte and UTF-8 locales"));
      flags |= PCRE2_UTF;
#if 0
      /* do not match individual code units but only UTF-8  */
      flags |= PCRE2_NEVER_BACKSLASH_C;
#endif
#ifdef PCRE2_MATCH_INVALID_UTF
      /* consider invalid UTF-8 as a barrier, instead of error  */
      flags |= PCRE2_MATCH_INVALID_UTF;
#endif
    }

  /* FIXME: Remove this restriction.  */
  if (rawmemchr (pattern, '\n') != patlim)
    die (EXIT_TROUBLE, 0, _("the -P option only supports a single pattern"));

  *n = '\0';
  if (match_words)
    strcpy (n, wprefix);
  if (match_lines)
    strcpy (n, xprefix);
  n += strlen (n);
  memcpy (n, pattern, size);
  n += size;
  if (match_words && !match_lines)
    {
    strcpy (n, wsuffix);
    n += strlen(wsuffix);
    }
  if (match_lines)
    {
    strcpy (n, xsuffix);
    n += strlen(xsuffix);
    }

  pcre2_set_character_tables (ccontext, pcre2_maketables (NULL));
  pc->cre = pcre2_compile (re, n - (char *)re, flags, &ec, &e, ccontext);
  if (!pc->cre)
    {
      pcre2_get_error_message (ec, ep, sizeof (ep));
      die (EXIT_TROUBLE, 0, "%s", ep);
    }

  pc->data = pcre2_match_data_create_from_pattern (pc->cre, NULL);

  ec = pcre2_jit_compile (pc->cre, PCRE2_JIT_COMPLETE);
  if (ec && ec != PCRE2_ERROR_JIT_BADOPTION && ec != PCRE2_ERROR_NOMEMORY)
    die (EXIT_TROUBLE, 0, _("JIT internal error: %d"), ec);
  else
    {
      /* The PCRE documentation says that a 32 KiB stack is the default.  */
      pc->jit_stack_size = 32 << 10;
    }

  free (re);

  pc->empty_match[false] = jit_exec (pc, "", 0, 0, PCRE2_NOTBOL);
  pc->empty_match[true] = jit_exec (pc, "", 0, 0, 0);

  return pc;
}

ptrdiff_t
Pexecute (void *vcp, char const *buf, idx_t size, idx_t *match_size,
          char const *start_ptr)
{
  char const *p = start_ptr ? start_ptr : buf;
  bool bol = p[-1] == eolbyte;
  char const *line_start = buf;
  int e = PCRE2_ERROR_NOMATCH;
  char const *line_end;
  struct pcre_comp *pc = vcp;
  PCRE2_SIZE *sub = pcre2_get_ovector_pointer (pc->data);

  /* The search address to pass to PCRE.  This is the start of
     the buffer, or just past the most-recently discovered encoding
     error or line end.  */
  char const *subject = buf;

  do
    {
      /* Search line by line.  Although this code formerly used
         PCRE_MULTILINE for performance, the performance wasn't always
         better and the correctness issues were too puzzling.  See
         Bug#22655.  */
      line_end = rawmemchr (p, eolbyte);
      if (PCRE2_SIZE_MAX < line_end - p)
        die (EXIT_TROUBLE, 0, _("exceeded PCRE's line length limit"));

      for (;;)
        {
          /* Skip past bytes that are easily determined to be encoding
             errors, treating them as data that cannot match.  This is
             faster than having PCRE check them.  */
          while (localeinfo.sbclen[to_uchar (*p)] == -1)
            {
              p++;
              subject = p;
              bol = false;
            }

          PCRE2_SIZE search_offset = p - subject;

          /* Check for an empty match; this is faster than letting
             PCRE do it.  */
          if (p == line_end)
            {
              sub[0] = sub[1] = search_offset;
              e = pc->empty_match[bol];
              break;
            }

          int options = 0;
          if (!bol)
            options |= PCRE2_NOTBOL;

          e = jit_exec (pc, subject, line_end - subject,
                        search_offset, options);
          /* PCRE2 provides 22 different error codes for bad UTF-8  */
          if (! (PCRE2_ERROR_UTF8_ERR21 <= e && e < PCRE2_ERROR_UTF8_ERR1))
            break;
          PCRE2_SIZE valid_bytes = pcre2_get_startchar (pc->data);

          if (search_offset <= valid_bytes)
            {
              /* Try to match the string before the encoding error.  */
              if (valid_bytes == 0)
                {
                  /* Handle the empty-match case specially, for speed.
                     This optimization is valid if VALID_BYTES is zero,
                     which means SEARCH_OFFSET is also zero.  */
                  sub[0] = valid_bytes;
                  sub[1] = 0;
                  e = pc->empty_match[bol];
                }
              else
                e = jit_exec (pc, subject, valid_bytes, search_offset,
                              options | PCRE2_NO_UTF_CHECK | PCRE2_NOTEOL);

              if (e != PCRE2_ERROR_NOMATCH)
                break;

              /* Treat the encoding error as data that cannot match.  */
              p = subject + valid_bytes + 1;
              bol = false;
            }

          subject += valid_bytes + 1;
        }

      if (e != PCRE2_ERROR_NOMATCH)
        break;
      bol = true;
      p = subject = line_start = line_end + 1;
    }
  while (p < buf + size);

  if (e <= 0)
    {
      switch (e)
        {
        case PCRE2_ERROR_NOMATCH:
          break;

        case PCRE2_ERROR_NOMEMORY:
          die (EXIT_TROUBLE, 0, _("%s: memory exhausted"), input_filename ());

        case PCRE2_ERROR_JIT_STACKLIMIT:
          die (EXIT_TROUBLE, 0, _("%s: exhausted PCRE JIT stack"),
               input_filename ());

        case PCRE2_ERROR_MATCHLIMIT:
          die (EXIT_TROUBLE, 0, _("%s: exceeded PCRE's backtracking limit"),
               input_filename ());

        case PCRE2_ERROR_DEPTHLIMIT:
          die (EXIT_TROUBLE, 0,
               _("%s: exceeded PCRE's nested backtracking limit"),
               input_filename ());

        case PCRE2_ERROR_RECURSELOOP:
          die (EXIT_TROUBLE, 0, _("%s: PCRE detected recurse loop"),
               input_filename ());

#ifdef PCRE2_ERROR_HEAPLIMIT
        case PCRE2_ERROR_HEAPLIMIT:
          die (EXIT_TROUBLE, 0, _("%s: exceeded PCRE's heap limit"),
               input_filename ());
#endif

        default:
          /* For now, we lump all remaining PCRE failures into this basket.
             If anyone cares to provide sample grep usage that can trigger
             particular PCRE errors, we can add to the list (above) of more
             detailed diagnostics.  */
          die (EXIT_TROUBLE, 0, _("%s: internal PCRE error: %d"),
               input_filename (), e);
        }

      return -1;
    }
  else
    {
      char const *matchbeg = subject + sub[0];
      char const *matchend = subject + sub[1];
      char const *beg;
      char const *end;
      if (start_ptr)
        {
          beg = matchbeg;
          end = matchend;
        }
      else
        {
          beg = line_start;
          end = line_end + 1;
        }
      *match_size = end - beg;
      return beg - buf;
    }
}
