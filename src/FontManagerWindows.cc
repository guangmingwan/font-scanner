#define WINVER 0x0600
#include "FontDescriptor.h"
#include <dwrite.h>
/*#include <dwrite_1.h>*/
#include <unordered_set>
#include <new>

// throws a JS error when there is some exception in DirectWrite
#define HR(hr) \
  if (FAILED(hr)) throw "Font loading error";
#define RETURN_ERROR_CODE(hr) \
  { long error = hr; if (FAILED(error)) return error; }
#define CONTINUE_ON_HRESULT_ERROR(hr) \
  if (FAILED(hr)) continue;

WCHAR *utf8ToUtf16(const char *input) {
  unsigned int len = MultiByteToWideChar(CP_UTF8, 0, input, -1, NULL, 0);
  WCHAR *output = new WCHAR[len];
  MultiByteToWideChar(CP_UTF8, 0, input, -1, output, len);
  return output;
}

char *utf16ToUtf8(const WCHAR *input) {
  unsigned int len = WideCharToMultiByte(CP_UTF8, 0, input, -1, NULL, 0, NULL, NULL);
  char *output = new char[len];
  WideCharToMultiByte(CP_UTF8, 0, input, -1, output, len, NULL, NULL);
  return output;
}

// returns the index of the user's locale in the set of localized strings
unsigned int getLocaleIndex(IDWriteLocalizedStrings *strings, bool useLocal) {
  unsigned int index = 0;
  BOOL exists = false;
  if(useLocal) {
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH];

    // Get the default locale for this user.
    int success = GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH);

    // If the default locale is returned, find that locale name, otherwise use "en-us".
    if (success) {
      HR(strings->FindLocaleName(localeName, &index, &exists));
    }
  }

  // if the above find did not find a match, retry with US English
  if (!exists) {
    HR(strings->FindLocaleName(L"en-us", &index, &exists));
  }

  if (!exists)
    index = 0;

  return index;
}

// gets a localized string for a font
char *getString(IDWriteFont *font, DWRITE_INFORMATIONAL_STRING_ID string_id, bool useLocal) {
  char *res = NULL;
  IDWriteLocalizedStrings *strings = NULL;

  BOOL exists = false;
  HR(font->GetInformationalStrings(
    string_id,
    &strings,
    &exists
  ));

  if (exists) {
    unsigned int index = getLocaleIndex(strings,useLocal);
    unsigned int len = 0;
    WCHAR *str = NULL;

    HR(strings->GetStringLength(index, &len));
    str = new WCHAR[len + 1];

    HR(strings->GetString(index, str, len + 1));

    // convert to utf8
    res = utf16ToUtf8(str);
    SafeDeleteArray(str);

    strings->Release();
  }

  if (!res) {
    res = new char[1];
    res[0] = '\0';
  }

  return res;
}

long resultFromFont(FontDescriptor **res, IDWriteFont *font) {
  IDWriteFontFace *face = NULL;
  unsigned int numFiles = 0;

  RETURN_ERROR_CODE(font->CreateFontFace(&face));

  // get the font files from this font face
  IDWriteFontFile *files = NULL;
  RETURN_ERROR_CODE(face->GetFiles(&numFiles, NULL));
  RETURN_ERROR_CODE(face->GetFiles(&numFiles, &files));

  // return the first one
  if (numFiles > 0) {
    IDWriteFontFileLoader *loader = NULL;
    IDWriteLocalFontFileLoader *fileLoader = NULL;
    unsigned int nameLength = 0;
    const void *referenceKey = NULL;
    unsigned int referenceKeySize = 0;
    WCHAR *name = NULL;

    RETURN_ERROR_CODE(files[0].GetLoader(&loader));

    // check if this is a local font file
    HRESULT hr = loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), (void **)&fileLoader);
    if (SUCCEEDED(hr)) {
      // get the file path
      RETURN_ERROR_CODE(files[0].GetReferenceKey(&referenceKey, &referenceKeySize));
      RETURN_ERROR_CODE(fileLoader->GetFilePathLengthFromKey(referenceKey, referenceKeySize, &nameLength));

      name = new WCHAR[nameLength + 1];
      RETURN_ERROR_CODE(fileLoader->GetFilePathFromKey(referenceKey, referenceKeySize, name, nameLength + 1));

      char *psName = utf16ToUtf8(name);
      char *postscriptName = getString(font, DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, true);
      char *family = getString(font, DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, false);
      char *style = getString(font, DWRITE_INFORMATIONAL_STRING_WIN32_SUBFAMILY_NAMES, true);

      bool monospace = false;
      // this method requires windows 7, so we need to cast to an IDWriteFontFace1
      /*
      IDWriteFontFace1 *face1;
      HRESULT hr = face->QueryInterface(__uuidof(IDWriteFontFace1), (void **)&face1);
      if (SUCCEEDED(hr)) {
        monospace = face1->IsMonospacedFont() == TRUE;
      };
      */
      *res = new FontDescriptor(
        psName,
        postscriptName,
        family,
        style,
        (FontWeight) font->GetWeight(),
        (FontWidth) font->GetStretch(),
        font->GetStyle() == DWRITE_FONT_STYLE_ITALIC,
        monospace
      );

      SafeDeleteArray(psName);
      SafeDeleteArray(name);
      SafeDeleteArray(postscriptName);
      SafeDeleteArray(family);
      SafeDeleteArray(style);
      SafeRelease(fileLoader);
    }

    SafeRelease(loader);
  }

  SafeRelease(face);
  SafeRelease(files);

  return 0;
}

long getAvailableFonts(ResultSet **resultSet) {
  int count = 0;

  IDWriteFactory *factory = NULL;

  RETURN_ERROR_CODE(DWriteCreateFactory(
    DWRITE_FACTORY_TYPE_SHARED,
    __uuidof(IDWriteFactory),
    reinterpret_cast<IUnknown**>(&factory)
  ));

  // Get the system font collection.
  IDWriteFontCollection *collection = NULL;

  RETURN_ERROR_CODE(factory->GetSystemFontCollection(&collection));

  // Get the number of font families in the collection.
  int familyCount = collection->GetFontFamilyCount();

  // track postscript names we've already added
  // using a set so we don't get any duplicates.
  std::unordered_set<std::string> psNames;

  *resultSet = new ResultSet();
  for (int i = 0; i < familyCount; i++) {
    IDWriteFontFamily *family = NULL;

    // Get the font family.
    CONTINUE_ON_HRESULT_ERROR(collection->GetFontFamily(i, &family));
    int fontCount = family->GetFontCount();

    for (int j = 0; j < fontCount; j++) {
      IDWriteFont *desc = NULL;
      FontDescriptor *result = NULL;

      CONTINUE_ON_HRESULT_ERROR(family->GetFont(j, &desc));
      CONTINUE_ON_HRESULT_ERROR(resultFromFont(&result, desc));
      const char* findKey = result->postscriptName;
      if(strlen(findKey) <= 0) {
        findKey = result->family;
      }
      if(strlen(findKey)>0) {
        if (psNames.count(findKey) == 0) {
          (*resultSet)->push_back(result);
          psNames.insert(findKey);
        } else {
          
          SafeDelete(result);
          
        }
      }
    }

    SafeRelease(family);
  }

  SafeRelease(collection);
  SafeRelease(factory);

  return 0;
}

bool resultMatches(FontDescriptor *result, FontDescriptor *desc) {
  if (desc->postscriptName && strcmp(desc->postscriptName, result->postscriptName) != 0)
    return false;

  if (desc->family && strcmp(desc->family, result->family) != 0)
    return false;

  if (desc->style && strcmp(desc->style, result->style) != 0)
    return false;

  if (desc->weight && desc->weight != result->weight)
    return false;

  if (desc->width && desc->width != result->width)
    return false;

  if (desc->italic != result->italic)
    return false;

  if (desc->monospace != result->monospace)
    return false;

  return true;
}

long findFonts(ResultSet** fonts, FontDescriptor *desc) {
  RETURN_ERROR_CODE(getAvailableFonts(fonts));

  for (ResultSet::iterator it = (*fonts)->begin(); it != (*fonts)->end();) {
    if (!resultMatches(*it, desc)) {
      
      SafeDelete(*it);
      
      it = (*fonts)->erase(it);
    } else {
      it++;
    }
  }

  return 0;
}

long findFont(FontDescriptor **foundFont, FontDescriptor *desc) {
  ResultSet *fonts;
  *foundFont = NULL;
  RETURN_ERROR_CODE(findFonts(&fonts, desc));

  // if we didn't find anything, try again with only the font traits, no string names
  if (fonts->size() == 0) {
    
    SafeDelete(fonts);
    
    FontDescriptor *fallback = new FontDescriptor(
      NULL, NULL, NULL, NULL,
      desc->weight, desc->width, desc->italic, false
    );

    RETURN_ERROR_CODE(findFonts(&fonts, fallback));
  }

  // ok, nothing. shouldn't happen often.
  // just return the first available font
  if (fonts->size() == 0) {
   
    SafeDelete(fonts);
     
    RETURN_ERROR_CODE(getAvailableFonts(&fonts));
  }

  // hopefully we found something now.
  // copy and return the first result
  if (fonts->size() > 0) {
    *foundFont = new FontDescriptor(fonts->front());
   
    SafeDelete(fonts);
    
    return 0;
  }

  // whoa, weird. no fonts installed or something went wrong.
 
    SafeDelete(fonts);
  
  return 1;
}

// custom text renderer used to determine the fallback font for a given char
class FontFallbackRenderer : public IDWriteTextRenderer {
public:
  IDWriteFontCollection *systemFonts;
  IDWriteFont *font;
  unsigned long refCount;

  FontFallbackRenderer(IDWriteFontCollection *collection) {
    refCount = 0;
    collection->AddRef();
    systemFonts = collection;
    font = NULL;
  }

  ~FontFallbackRenderer() {
    if (systemFonts)
      SafeRelease(systemFonts);

    if (font)
      SafeRelease(font);
  }

  // IDWriteTextRenderer methods
  IFACEMETHOD(DrawGlyphRun)(
      void *clientDrawingContext,
      FLOAT baselineOriginX,
      FLOAT baselineOriginY,
      DWRITE_MEASURING_MODE measuringMode,
      DWRITE_GLYPH_RUN const *glyphRun,
      DWRITE_GLYPH_RUN_DESCRIPTION const *glyphRunDescription,
      IUnknown *clientDrawingEffect) {

    // save the font that was actually rendered
    return systemFonts->GetFontFromFontFace(glyphRun->fontFace, &font);
  }

  IFACEMETHOD(DrawUnderline)(
      void *clientDrawingContext,
      FLOAT baselineOriginX,
      FLOAT baselineOriginY,
      DWRITE_UNDERLINE const *underline,
      IUnknown *clientDrawingEffect) {
    return E_NOTIMPL;
  }


  IFACEMETHOD(DrawStrikethrough)(
      void *clientDrawingContext,
      FLOAT baselineOriginX,
      FLOAT baselineOriginY,
      DWRITE_STRIKETHROUGH const *strikethrough,
      IUnknown *clientDrawingEffect) {
    return E_NOTIMPL;
  }


  IFACEMETHOD(DrawInlineObject)(
      void *clientDrawingContext,
      FLOAT originX,
      FLOAT originY,
      IDWriteInlineObject *inlineObject,
      BOOL isSideways,
      BOOL isRightToLeft,
      IUnknown *clientDrawingEffect) {
    return E_NOTIMPL;
  }

  // IDWritePixelSnapping methods
  IFACEMETHOD(IsPixelSnappingDisabled)(void *clientDrawingContext, BOOL *isDisabled) {
    *isDisabled = FALSE;
    return S_OK;
  }

  IFACEMETHOD(GetCurrentTransform)(void *clientDrawingContext, DWRITE_MATRIX *transform) {
    const DWRITE_MATRIX ident = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    *transform = ident;
    return S_OK;
  }

  IFACEMETHOD(GetPixelsPerDip)(void *clientDrawingContext, FLOAT *pixelsPerDip) {
    *pixelsPerDip = 1.0f;
    return S_OK;
  }

  // IUnknown methods
  IFACEMETHOD_(unsigned long, AddRef)() {
    return InterlockedIncrement(&refCount);
  }

  IFACEMETHOD_(unsigned long,  Release)() {
    unsigned long newCount = InterlockedDecrement(&refCount);
    if (newCount == 0) {
      
      delete this;
      
      return 0;
    }

    return newCount;
  }

  IFACEMETHOD(QueryInterface)(IID const& riid, void **ppvObject) {
    if (__uuidof(IDWriteTextRenderer) == riid) {
      *ppvObject = this;
    } else if (__uuidof(IDWritePixelSnapping) == riid) {
      *ppvObject = this;
    } else if (__uuidof(IUnknown) == riid) {
      *ppvObject = this;
    } else {
      *ppvObject = nullptr;
      return E_FAIL;
    }

    this->AddRef();
    return S_OK;
  }
};

long substituteFont(FontDescriptor **res, char *postscriptName, char *string) {
  IDWriteFactory *factory = NULL;
  RETURN_ERROR_CODE(DWriteCreateFactory(
    DWRITE_FACTORY_TYPE_SHARED,
    __uuidof(IDWriteFactory),
    reinterpret_cast<IUnknown**>(&factory)
  ));

  // Get the system font collection.
  IDWriteFontCollection *collection = NULL;
  RETURN_ERROR_CODE(factory->GetSystemFontCollection(&collection));

  // find the font for the given postscript name
  FontDescriptor *desc = new FontDescriptor();
  desc->postscriptName = postscriptName;
  FontDescriptor *font = NULL;
  RETURN_ERROR_CODE(findFont(&font, desc));

  // create a text format object for this font
  IDWriteTextFormat *format = NULL;
  if (font) {
    WCHAR *familyName = utf8ToUtf16(font->family);

    // create a text format
    RETURN_ERROR_CODE(factory->CreateTextFormat(
      familyName,
      collection,
      (DWRITE_FONT_WEIGHT)font->weight,
      font->italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
      (DWRITE_FONT_STRETCH)font->width,
      12.0,
      L"en-us",
      &format
    ));
   
    SafeDeleteArray(familyName);
    SafeDelete(font);
    
  } else {
    // this should never happen, but just in case, let the system
    // decide the default font in case findFont returned nothing.
    RETURN_ERROR_CODE(factory->CreateTextFormat(
      L"",
      collection,
      DWRITE_FONT_WEIGHT_REGULAR,
      DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL,
      12.0,
      L"en-us",
      &format
    ));
  }

  // convert utf8 string for substitution to utf16
  WCHAR *str = utf8ToUtf16(string);

  // create a text layout for the substitution string
  IDWriteTextLayout *layout = NULL;
  RETURN_ERROR_CODE(factory->CreateTextLayout(
    str,
    wcslen(str),
    format,
    100.0,
    100.0,
    &layout
  ));

  // render it using a custom renderer that saves the physical font being used
  FontFallbackRenderer *renderer = new FontFallbackRenderer(collection);
  RETURN_ERROR_CODE(layout->Draw(NULL, renderer, 100.0, 100.0));

  // if we found something, create a result object
  if (renderer->font) {
    RETURN_ERROR_CODE(resultFromFont(&font, renderer->font));
  }

  // free all the things
 
  SafeDelete(renderer);
  
  SafeRelease(layout);
  SafeRelease(format);

  desc->postscriptName = NULL;

  
  SafeDelete(desc);
  SafeDeleteArray(str);
  
  SafeRelease(collection);
  SafeRelease( factory );

  return 0;
}
