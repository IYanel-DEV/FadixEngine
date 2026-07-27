// Thin Lexilla entry: only FXS (lua) and C++ lexers. Avoids shipping every Lexilla language.
#include <cstring>
#include <initializer_list>
#include <vector>

#include "ILexer.h"
#include "LexerModule.h"
#include "CatalogueModules.h"

using Scintilla::ILexer5;
using Lexilla::CatalogueModules;
using Lexilla::LexerModule;

extern const LexerModule lmLua;
extern const LexerModule lmCPP;

namespace
{
CatalogueModules g_Catalogue;
bool g_Ready = false;

void EnsureCatalogue()
{
    if (g_Ready)
    {
        return;
    }
    g_Catalogue.AddLexerModules({&lmLua, &lmCPP});
    g_Ready = true;
}
}

extern "C" ILexer5* __stdcall CreateLexer(const char* name)
{
    EnsureCatalogue();
    if (name == nullptr)
    {
        return nullptr;
    }
    for (size_t i = 0; i < g_Catalogue.Count(); ++i)
    {
        const char* lexerName = g_Catalogue.Name(i);
        if (lexerName != nullptr && std::strcmp(lexerName, name) == 0)
        {
            return g_Catalogue.Create(i);
        }
    }
    return nullptr;
}
