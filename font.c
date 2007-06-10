/*
 * font.c: Font handling for the DVB On Screen Display
 *
 * See the main source file 'vdr.c' for copyright information and
 * how to reach the author.
 *
 * $Id: font.c 1.15 2007/06/09 14:41:27 kls Exp $
 */

#include "font.h"
#include <ctype.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "config.h"
#include "osd.h"
#include "tools.h"

// --- cFreetypeFont ---------------------------------------------------------

#define KERNING_UNKNOWN  (-10000)

struct tKerning {
  uint prevSym;
  int kerning;
  tKerning(uint PrevSym, int Kerning) { prevSym = PrevSym; kerning = Kerning; }
  };

class cGlyph : public cListObject {
private:
  uint charCode;
  uchar *bitmap;
  int advanceX;
  int advanceY;
  int left;  ///< The bitmap's left bearing expressed in integer pixels.
  int top;   ///< The bitmap's top bearing expressed in integer pixels.
  int width; ///< The number of pixels per bitmap row.
  int rows;  ///< The number of bitmap rows.
  int pitch; ///< The pitch's absolute value is the number of bytes taken by one bitmap row, including padding.
  cVector<tKerning> kerningCache;
public:
  cGlyph(uint CharCode, FT_GlyphSlotRec_ *GlyphData);
  virtual ~cGlyph();
  uint CharCode(void) const { return charCode; }
  uchar *Bitmap(void) const { return bitmap; }
  int AdvanceX(void) const { return advanceX; }
  int AdvanceY(void) const { return advanceY; }
  int Left(void) const { return left; }
  int Top(void) const { return top; }
  int Width(void) const { return width; }
  int Rows(void) const { return rows; }
  int Pitch(void) const { return pitch; }
  int GetKerningCache(uint PrevSym) const;
  void SetKerningCache(uint PrevSym, int Kerning);
  };

cGlyph::cGlyph(uint CharCode, FT_GlyphSlotRec_ *GlyphData)
{
  charCode = CharCode;
  advanceX = GlyphData->advance.x >> 6;
  advanceY = GlyphData->advance.y >> 6;
  left = GlyphData->bitmap_left;
  top = GlyphData->bitmap_top;
  width = GlyphData->bitmap.width;
  rows = GlyphData->bitmap.rows;
  pitch = GlyphData->bitmap.pitch;
  bitmap = MALLOC(uchar, rows * pitch);
  memcpy(bitmap, GlyphData->bitmap.buffer, rows * pitch);
}

cGlyph::~cGlyph()
{
  free(bitmap);
}

int cGlyph::GetKerningCache(uint PrevSym) const
{
  for (int i = kerningCache.Size(); --i > 0; ) {
      if (kerningCache[i].prevSym == PrevSym)
         return kerningCache[i].kerning;
      }
  return KERNING_UNKNOWN;
}

void cGlyph::SetKerningCache(uint PrevSym, int Kerning)
{
  kerningCache.Append(tKerning(PrevSym, Kerning));
}

class cFreetypeFont : public cFont {
private:
  int height;
  int bottom;
  FT_Library library; ///< Handle to library
  FT_Face face; ///< Handle to face object
  mutable cList<cGlyph> glyphCacheMonochrome;
  mutable cList<cGlyph> glyphCacheAntiAliased;
  int Bottom(void) const { return bottom; }
  int Kerning(cGlyph *Glyph, uint PrevSym) const;
  cGlyph* Glyph(uint CharCode, bool AntiAliased = false) const;
public:
  cFreetypeFont(const char *Name, int CharHeight);
  virtual ~cFreetypeFont();
  virtual int Width(uint c) const;
  virtual int Width(const char *s) const;
  virtual int Height(void) const { return height; }
  virtual void DrawText(cBitmap *Bitmap, int x, int y, const char *s, tColor ColorFg, tColor ColorBg, int Width) const;
  };

cFreetypeFont::cFreetypeFont(const char *Name, int CharHeight)
{
  height = 0;
  bottom = 0;
  int error = FT_Init_FreeType(&library);
  if (!error) {
     error = FT_New_Face(library, Name, 0, &face);
     if (!error) {
        if (face->num_fixed_sizes && face->available_sizes) { // fixed font
           // TODO what exactly does all this mean?
           height = face->available_sizes->height;
           for (uint sym ='A'; sym < 'z'; sym++) { // search for descender for fixed font FIXME
               FT_UInt glyph_index = FT_Get_Char_Index(face, sym);
               error = FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
               if (!error) {
                  error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
                  if (!error) {
                     if (face->glyph->bitmap.rows-face->glyph->bitmap_top > bottom)
                        bottom = face->glyph->bitmap.rows-face->glyph->bitmap_top;
                     }
                  else
                     esyslog("ERROR: FreeType: error %d in FT_Render_Glyph", error);
                  }
               else
                  esyslog("ERROR: FreeType: error %d in FT_Load_Glyph", error);
               }
           }
        else {
           error = FT_Set_Char_Size(face, // handle to face object
                                    0,    // char_width in 1/64th of points
                                    CharHeight * 64, // CharHeight in 1/64th of points
                                    0,    // horizontal device resolution
                                    0);   // vertical device resolution
           if (!error) {
              height = ((face->size->metrics.ascender-face->size->metrics.descender) + 63) / 64;
              bottom = abs((face->size->metrics.descender - 63) / 64);
              }
           else
              esyslog("ERROR: FreeType: error %d during FT_Set_Char_Size (font = %s)\n", error, Name);
           }
        }
     else
        esyslog("ERROR: FreeType: load error %d (font = %s)", error, Name);
     }
  else
     esyslog("ERROR: FreeType: initialization error %d (font = %s)", error, Name);
}

cFreetypeFont::~cFreetypeFont()
{
  FT_Done_Face(face);
  FT_Done_FreeType(library);
}

int cFreetypeFont::Kerning(cGlyph *Glyph, uint PrevSym) const
{
  int kerning = 0;
  if (Glyph && PrevSym) {
     kerning = Glyph->GetKerningCache(PrevSym);
     if (kerning == KERNING_UNKNOWN) {
        FT_Vector delta;
        FT_UInt glyph_index = FT_Get_Char_Index(face, Glyph->CharCode());
        FT_UInt glyph_index_prev = FT_Get_Char_Index(face, PrevSym);
        FT_Get_Kerning(face, glyph_index_prev, glyph_index, FT_KERNING_DEFAULT, &delta);
        kerning = delta.x / 64;
        Glyph->SetKerningCache(PrevSym, kerning);
        }
     }
  return kerning;
}

cGlyph* cFreetypeFont::Glyph(uint CharCode, bool AntiAliased) const
{
  // Lookup in cache:
  cList<cGlyph> *glyphCache = AntiAliased ? &glyphCacheAntiAliased : &glyphCacheMonochrome;
  for (cGlyph *g = glyphCache->First(); g; g = glyphCache->Next(g)) {
      if (g->CharCode() == CharCode)
         return g;
      }

  FT_UInt glyph_index = FT_Get_Char_Index(face, CharCode);

  // Load glyph image into the slot (erase previous one):
  int error = FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
  if (error)
     esyslog("ERROR: FreeType: error during FT_Load_Glyph");
  else {
#if ((FREETYPE_MAJOR == 2 && FREETYPE_MINOR == 1 && FREETYPE_PATCH >= 7) || (FREETYPE_MAJOR == 2 && FREETYPE_MINOR == 2 && FREETYPE_PATCH <= 1))// TODO workaround for bug? which one?
     if (AntiAliased || CharCode == 32)
#else
     if (AntiAliased)
#endif
        error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
     else
        error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO);
     if (error)
        esyslog("ERROR: FreeType: error during FT_Render_Glyph %d, %d\n", CharCode, glyph_index);
     else { //new bitmap
        cGlyph *Glyph = new cGlyph(CharCode, face->glyph);
        glyphCache->Add(Glyph);
        return Glyph;
        }
     }
  return NULL;
}

int cFreetypeFont::Width(uint c) const
{
  cGlyph *g = Glyph(c, Setup.AntiAlias);
  return g ? g->AdvanceX() : 0;
}

int cFreetypeFont::Width(const char *s) const
{
  int w = 0;
  if (s) {
     uint prevSym = 0;
     while (*s) {
           int sl = Utf8CharLen(s);
           uint sym = Utf8CharGet(s, sl);
           s += sl;
           cGlyph *g = Glyph(sym, Setup.AntiAlias);
           if (g)
              w += g->AdvanceX() + Kerning(g, prevSym);
           prevSym = sym;
           }
     }
  return w;
}

void cFreetypeFont::DrawText(cBitmap *Bitmap, int x, int y, const char *s, tColor ColorFg, tColor ColorBg, int Width) const
{
  if (s && height) { // checking height to make sure we actually have a valid font
     bool AntiAliased = Setup.AntiAlias && Bitmap->Bpp() >= 8;
     tIndex fg = Bitmap->Index(ColorFg);
     uint prevSym = 0;
     while (*s) {
           int sl = Utf8CharLen(s);
           uint sym = Utf8CharGet(s, sl);
           s += sl;
           cGlyph *g = Glyph(sym, AntiAliased);
           int kerning = Kerning(g, prevSym);
           prevSym = sym;
           uchar *buffer = g->Bitmap();
           int symWidth = g->Width();
           if (Width && x + symWidth + g->Left() + kerning - 1 > Width)
              break; // we don't draw partial characters
           if (x + symWidth + g->Left() + kerning > 0) {
              for (int row = 0; row < g->Rows(); row++) {
                  for (int pitch = 0; pitch < g->Pitch(); pitch++) {
                      uchar bt = *(buffer + (row * g->Pitch() + pitch));
                      if (AntiAliased) {
                         if (bt > 0x00) {
                            int px = x + pitch + g->Left() + kerning;
                            int py = y + row + (height - Bottom() - g->Top());
                            if (bt == 0xFF)
                               Bitmap->SetIndex(px, py, fg);
                            else {
                               tColor bg = (ColorBg != clrTransparent) ? ColorBg : Bitmap->GetColor(px, py);
                               Bitmap->SetIndex(px, py, Bitmap->Index(Bitmap->Blend(ColorFg, bg, bt)));
                               }
                            }
                         }
                      else { //monochrome rendering
                         for (int col = 0; col < 8 && col + pitch * 8 <= symWidth; col++) {
                             if (bt & 0x80)
                                Bitmap->SetIndex(x + col + pitch * 8 + g->Left() + kerning, y + row + (height - Bottom() - g->Top()), fg);
                             bt <<= 1;
                             }
                         }
                      }
                  }
              }
           x += g->AdvanceX() + kerning;
           if (x > Bitmap->Width() - 1)
              break;
           }
     }
}

// --- cFont -----------------------------------------------------------------

cFont *cFont::fonts[eDvbFontSize] = { NULL };

void cFont::SetFont(eDvbFont Font, const char *Name, int CharHeight)
{
  delete fonts[Font];
  fonts[Font] = new cFreetypeFont(*Name == '/' ? Name : *AddDirectory(FONTDIR, Name), CharHeight);
}

const cFont *cFont::GetFont(eDvbFont Font)
{
  if (Setup.UseSmallFont == 0 && Font == fontSml)
     Font = fontOsd;
  else if (Setup.UseSmallFont == 2)
     Font = fontSml;
  if (!fonts[Font]) {
     switch (Font) {
       case fontOsd: SetFont(Font, AddDirectory(FONTDIR, Setup.FontOsd), Setup.FontOsdSize); break;
       case fontSml: SetFont(Font, AddDirectory(FONTDIR, Setup.FontSml), Setup.FontSmlSize); break;
       case fontFix: SetFont(Font, AddDirectory(FONTDIR, Setup.FontFix), Setup.FontFixSize); break;
       }
     }
  return fonts[Font];
}

// --- cTextWrapper ----------------------------------------------------------

cTextWrapper::cTextWrapper(void)
{
  text = eol = NULL;
  lines = 0;
  lastLine = -1;
}

cTextWrapper::cTextWrapper(const char *Text, const cFont *Font, int Width)
{
  text = NULL;
  Set(Text, Font, Width);
}

cTextWrapper::~cTextWrapper()
{
  free(text);
}

void cTextWrapper::Set(const char *Text, const cFont *Font, int Width)
{
  free(text);
  text = Text ? strdup(Text) : NULL;
  eol = NULL;
  lines = 0;
  lastLine = -1;
  if (!text)
     return;
  lines = 1;
  if (Width <= 0)
     return;

  char *Blank = NULL;
  char *Delim = NULL;
  int w = 0;

  stripspace(text); // strips trailing newlines

  for (char *p = text; *p; ) {
      int sl = Utf8CharLen(p);
      uint sym = Utf8CharGet(p, sl);
      if (sym == '\n') {
         lines++;
         w = 0;
         Blank = Delim = NULL;
         p++;
         continue;
         }
      else if (sl == 1 && isspace(sym))
         Blank = p;
      int cw = Font->Width(sym);
      if (w + cw > Width) {
         if (Blank) {
            *Blank = '\n';
            p = Blank;
            continue;
            }
         else {
            // Here's the ugly part, where we don't have any whitespace to
            // punch in a newline, so we need to make room for it:
            if (Delim)
               p = Delim + 1; // let's fall back to the most recent delimiter
            char *s = MALLOC(char, strlen(text) + 2); // The additional '\n' plus the terminating '\0'
            int l = p - text;
            strncpy(s, text, l);
            s[l] = '\n';
            strcpy(s + l + 1, p);
            free(text);
            text = s;
            p = text + l;
            continue;
            }
         }
      else
         w += cw;
      if (strchr("-.,:;!?_", *p)) {
         Delim = p;
         Blank = NULL;
         }
      p += sl;
      }
}

const char *cTextWrapper::Text(void)
{
  if (eol) {
     *eol = '\n';
     eol = NULL;
     }
  return text;
}

const char *cTextWrapper::GetLine(int Line)
{
  char *s = NULL;
  if (Line < lines) {
     if (eol) {
        *eol = '\n';
        if (Line == lastLine + 1)
           s = eol + 1;
        eol = NULL;
        }
     if (!s) {
        s = text;
        for (int i = 0; i < Line; i++) {
            s = strchr(s, '\n');
            if (s)
               s++;
            else
               break;
            }
        }
     if (s) {
        if ((eol = strchr(s, '\n')) != NULL)
           *eol = 0;
        }
     lastLine = Line;
     }
  return s;
}
